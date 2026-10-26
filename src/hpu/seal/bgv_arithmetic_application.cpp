#include "hpu/seal/bgv_arithmetic_application.hpp"

#include "hpu/seal/ntt_bridge.hpp"
#include "scheme/bgv/basic_arithmetic.hpp"
#include "scheme/ckks/basic_arithmetic.hpp"
#include "hpu/compiler/operator_resources.hpp"
#include "util/validation.hpp"

#include "assembler.hpp"
#include "executable.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace hpu::seal_adapter {
namespace {

enum class Operation { add, subtract, negate, add_plain, subtract_plain, multiply_plain };

bool is_plain(Operation operation)
{
    return operation == Operation::add_plain ||
           operation == Operation::subtract_plain ||
           operation == Operation::multiply_plain;
}

std::uint32_t narrow(std::uint64_t value)
{
    if (value < 65537 || value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("BGV arithmetic modulus is outside the HPU PE range");
    }
    return static_cast<std::uint32_t>(value);
}

std::string mod_id(const std::string& prefix, std::size_t basis)
{
    return prefix + "/mod" + std::to_string(basis);
}

std::uint64_t barrett_mu(std::uint32_t modulus)
{
    return static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(1) << 64) / modulus);
}

void add_component(hpu::runtime::HpuMemImage& image,
                   const ::seal::Ciphertext& ciphertext,
                   const ::seal::SEALContext& context,
                   const std::string& prefix,
                   std::size_t component, std::size_t q_count,
                   std::size_t degree)
{
    const auto converted = ciphertext_component_to_hpu(
        ciphertext, component, context);
    for (std::size_t basis = 0; basis < q_count; ++basis) {
        const auto first = converted.words.begin() +
            static_cast<std::ptrdiff_t>(basis * degree);
        image.add(mod_id(prefix + "/c" + std::to_string(component), basis),
                  std::vector<std::uint32_t>(first, first + degree),
                  hpu::runtime::AllocationKind::ciphertext);
    }
}

void add_plaintext(hpu::runtime::HpuMemImage& image,
                   const ::seal::Plaintext& plaintext,
                   const ::seal::Ciphertext& ciphertext,
                   const ::seal::SEALContext& context,
                   Operation operation, std::size_t q_count,
                   std::size_t degree)
{
    if (!::seal::is_valid_for(plaintext, context) || plaintext.is_ntt_form() ||
        plaintext.coeff_count() > degree) {
        throw std::invalid_argument("BGV plaintext must be valid coefficient-domain input");
    }
    ::seal::Plaintext prepared = plaintext;
    if (operation != Operation::multiply_plain) {
        const auto t = context.get_context_data(ciphertext.parms_id())
            ->parms().plain_modulus().value();
        for (std::size_t index = 0; index < prepared.coeff_count(); ++index) {
            prepared[index] = static_cast<std::uint64_t>(
                (static_cast<unsigned __int128>(prepared[index]) *
                 ciphertext.correction_factor()) % t);
        }
    }
    ::seal::Evaluator evaluator(context);
    evaluator.transform_to_ntt_inplace(prepared, ciphertext.parms_id());
    const auto converted = plaintext_to_hpu(prepared, context);
    for (std::size_t basis = 0; basis < q_count; ++basis) {
        const auto first = converted.words.begin() +
            static_cast<std::ptrdiff_t>(basis * degree);
        image.add(mod_id("input/plain", basis),
                  std::vector<std::uint32_t>(first, first + degree),
                  hpu::runtime::AllocationKind::plaintext);
    }
}

class DmaRecipe {
public:
    DmaRecipe(const hpu::runtime::HpuMemImage& image,
              hpu::resource::OperatorProgram& program, std::size_t degree)
        : image_(image), program_(program), degree_(degree),
          bindings_(program.ports.size()), bound_(program.ports.size())
    {
        for (std::size_t id = 0; id < program.ports.size(); ++id) {
            const auto& name = program.ports[id].logical_binding;
            if (name.empty() || !ports_.emplace(name, id).second)
                throw std::logic_error("BGV arithmetic requires unique logical data ports");
        }
    }

    void load(const std::string& role, const std::string& id,
              hpu::DataType type = hpu::DataType::poly,
              hpu::DloadFlag flag = hpu::DloadFlag::regular_bank)
    {
        bind(role, id, true,
             hpu::dma_mod_bit(type), hpu::dma_bank_mask(type, flag));
    }

    void store(const std::string& role, const std::string& id)
    {
        bind(role, id, false, 0, 0);
    }

    std::vector<BgvKeySwitchDmaBinding> finish()
    {
        if (std::find(bound_.begin(), bound_.end(), false) != bound_.end()) {
            throw std::logic_error("BGV arithmetic DMA recipe is incomplete");
        }
        return std::move(bindings_);
    }

private:
    void bind(const std::string& role, const std::string& id, bool load,
              std::uint8_t type_or_release, std::uint8_t flag)
    {
        const auto found = ports_.find(role);
        if (found == ports_.end())
            throw std::logic_error("missing BGV arithmetic logical data port: " + role);
        const std::size_t index = found->second;
        const auto& port = program_.ports.at(index);
        const auto& operation = program_.operations.at(port.operation_index);
        const auto& instruction = operation.instruction;
        const auto direction = load ? hpu::Mnemonic::kDload : hpu::Mnemonic::kDstore;
        const auto& allocation = image_.allocation(id);
        const auto expected_words = load && type_or_release == 1
            ? allocation.word_count : degree_;
        if (bound_[index] || port.load != load || instruction.mnemonic != direction ||
            instruction.type != type_or_release || (load && instruction.dma_flag != flag) ||
            allocation.word_count != expected_words ||
            allocation.span.line_count == 0 ||
            (!load && allocation.read_only) ||
            (load && type_or_release == 1 &&
             allocation.kind != hpu::runtime::AllocationKind::modulus_table)) {
            throw std::invalid_argument(
                "BGV arithmetic data binding disagrees with native declaration: " + role);
        }
        const auto value = load ? operation.outputs.at(0) : operation.inputs.at(0);
        const auto slot = program_.values.at(value).variable.compatibility_slot;
        auto& binding = bindings_[index];
        binding.dma_index = index;
        binding.instruction_index = port.operation_index;
        binding.direction = direction;
        binding.object_slot = static_cast<std::uint8_t>(slot);
        binding.type_or_release = instruction.type;
        binding.flag = instruction.dma_flag;
        binding.allocation_id = id;
        binding.span = allocation.span;
        binding.operation_dma_index = index;
        binding.cp = instruction.cp;
        program_.bind_port(index, {port.operation_index, id, allocation.span,
                                  0, "pointwise", index});
        bound_[index] = true;
    }

    const hpu::runtime::HpuMemImage& image_;
    hpu::resource::OperatorProgram& program_;
    std::size_t degree_;
    std::map<std::string, std::size_t> ports_;
    std::vector<BgvKeySwitchDmaBinding> bindings_;
    std::vector<bool> bound_;
};

BgvArithmeticApplication build(
    const ::seal::SEALContext& context, const ::seal::Ciphertext& left,
    const ::seal::Ciphertext* right, const ::seal::Plaintext* plaintext,
    Operation operation,
    std::uint64_t capacity_lines)
{
    const auto source = context.get_context_data(left.parms_id());
    const auto key_data = context.key_context_data();
    const auto first_data = context.first_context_data();
    if (!source || !key_data || !first_data ||
        source->parms().scheme() != ::seal::scheme_type::bgv ||
        !::seal::is_valid_for(left, context) || !left.is_ntt_form() ||
        left.size() != 2 ||
        (is_plain(operation) ? !plaintext : operation != Operation::negate &&
         (!right || !::seal::is_valid_for(*right, context) ||
          !right->is_ntt_form() || right->size() != 2 ||
          right->parms_id() != left.parms_id()))) {
        throw std::invalid_argument("BGV arithmetic requires matching two-component NTT ciphertexts");
    }
    const auto& key_moduli = key_data->parms().coeff_modulus();
    const auto& top_moduli = first_data->parms().coeff_modulus();
    const auto& q_moduli = source->parms().coeff_modulus();
    const std::size_t degree = source->parms().poly_modulus_degree();
    const std::size_t q_count = q_moduli.size();
    if (key_moduli.size() != top_moduli.size() + 1 ||
        key_moduli.size() + 1 > static_cast<std::size_t>(hpu::kMaxModContexts) ||
        q_count == 0 || q_count > top_moduli.size() ||
        degree > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !hpu::is_valid_ntt_size(static_cast<int>(degree))) {
        throw std::invalid_argument("unsupported BGV arithmetic Qmax|P|t layout");
    }
    for (std::size_t basis = 0; basis < q_count; ++basis) {
        if (q_moduli[basis].value() != key_moduli[basis].value()) {
            throw std::invalid_argument("BGV arithmetic active Q is not a key-context prefix");
        }
    }
    const std::uint32_t t = narrow(source->parms().plain_modulus().value());
    const auto balance = operation == Operation::negate || is_plain(operation)
        ? hpu::scheme::bgv::CorrectionBalance{
            static_cast<std::uint32_t>(left.correction_factor()), 1, 1}
        : hpu::scheme::bgv::balance_correction_factors(
            static_cast<std::uint32_t>(left.correction_factor()),
            static_cast<std::uint32_t>(right->correction_factor()), t);

    hpu::runtime::HpuMemImage image(capacity_lines);
    std::vector<std::uint32_t> table;
    for (const auto& modulus : key_moduli) {
        const std::uint32_t value = narrow(modulus.value());
        const std::uint64_t mu = barrett_mu(value);
        table.insert(table.end(), {value, static_cast<std::uint32_t>(mu),
                                   static_cast<std::uint32_t>(mu >> 32U), 0});
    }
    const std::uint64_t mu_t = barrett_mu(t);
    table.insert(table.end(), {t, static_cast<std::uint32_t>(mu_t),
                               static_cast<std::uint32_t>(mu_t >> 32U), 0});
    image.add("constants/modulus_table", table,
              hpu::runtime::AllocationKind::modulus_table);
    for (std::size_t component = 0; component < 2; ++component) {
        add_component(image, left, context, "input/left", component,
                      q_count, degree);
        if (right && !is_plain(operation)) {
            add_component(image, *right, context, "input/right", component,
                          q_count, degree);
        }
    }
    if (is_plain(operation)) {
        add_plaintext(image, *plaintext, left, context, operation, q_count, degree);
    }
    if (operation != Operation::negate) {
        for (std::size_t basis = 0; basis < q_count; ++basis) {
            const auto q = narrow(q_moduli[basis].value());
            if (balance.left_scalar != 1) {
                image.add(mod_id("constants/bgv_balance/left", basis),
                          std::vector<std::uint32_t>(degree, balance.left_scalar % q),
                          hpu::runtime::AllocationKind::constant);
            }
            if (balance.right_scalar != 1) {
                image.add(mod_id("constants/bgv_balance/right", basis),
                          std::vector<std::uint32_t>(degree, balance.right_scalar % q),
                          hpu::runtime::AllocationKind::constant);
            }
        }
    }
    for (std::size_t component = 0; component < 2; ++component) {
        for (std::size_t basis = 0; basis < q_count; ++basis) {
            image.reserve(mod_id("output/c" + std::to_string(component), basis),
                          degree, hpu::runtime::AllocationKind::output);
        }
    }
    hpu::resource::Recording recording;
    std::string body;
    if (operation == Operation::negate) {
        body = hpu::scheme::bgv::generate_negate_body_asm(
            static_cast<int>(q_count), true, true);
    } else if (operation == Operation::add_plain) {
        body = hpu::scheme::ckks::generate_add_plain_body_asm(
            static_cast<int>(q_count), true, true);
    } else if (operation == Operation::subtract_plain) {
        body = hpu::scheme::ckks::generate_subtract_plain_body_asm(
            static_cast<int>(q_count), true, true);
    } else if (operation == Operation::multiply_plain) {
        body = hpu::scheme::ckks::generate_multiply_plain_body_asm(
            static_cast<int>(q_count), true, true);
    } else if (operation == Operation::add) {
        body = hpu::scheme::bgv::generate_add_body_asm(
            static_cast<int>(q_count), balance, true, true);
    } else {
        body = hpu::scheme::bgv::generate_subtract_body_asm(
            static_cast<int>(q_count), balance, true, true);
    }
    auto instructions = hpu::assemble_source(body);
    hpu::validate_executable_program(instructions);
    auto native = recording.finish();
    DmaRecipe recipe(image, native, degree);
    recipe.load("constants/modulus_table", "constants/modulus_table", hpu::DataType::mod_ctx,
                hpu::DloadFlag::small_bank);
    if (is_plain(operation)) {
        for (std::size_t basis = 0; basis < q_count; ++basis) {
            const auto prefix = "pointwise/mod" + std::to_string(basis) + "/";
            recipe.load(prefix + "left/c0", mod_id("input/left/c0", basis));
            recipe.load(prefix + "plaintext", mod_id("input/plain", basis));
            recipe.store(prefix + "output/c0", mod_id("output/c0", basis));
            recipe.load(prefix + "left/c1", mod_id("input/left/c1", basis));
            recipe.store(prefix + "output/c1", mod_id("output/c1", basis));
        }
    } else for (std::size_t component = 0; component < 2; ++component) {
        for (std::size_t basis = 0; basis < q_count; ++basis) {
            const auto prefix = "pointwise/mod" + std::to_string(basis) + "/";
            const auto limb = "c" + std::to_string(component);
            recipe.load(prefix + "left/" + limb, mod_id("input/left/" + limb, basis));
            if (operation != Operation::negate) {
                if (balance.left_scalar != 1) {
                    recipe.load(prefix + "balance/left/" + limb,
                                mod_id("constants/bgv_balance/left", basis));
                }
                recipe.load(prefix + "right/" + limb, mod_id("input/right/" + limb, basis));
                if (balance.right_scalar != 1) {
                    recipe.load(prefix + "balance/right/" + limb,
                                mod_id("constants/bgv_balance/right", basis));
                }
            }
            recipe.store(prefix + "output/" + limb, mod_id("output/" + limb, basis));
        }
    }
    auto bindings = recipe.finish();
    return {std::move(image), left.parms_id(), balance.output_factor,
            std::move(instructions), std::move(bindings),
            std::make_shared<hpu::resource::OperatorProgram>(std::move(native))};
}

} // namespace

BgvArithmeticApplication build_bgv_add_application(
    const ::seal::SEALContext& context, const ::seal::Ciphertext& left,
    const ::seal::Ciphertext& right, std::uint64_t capacity_lines)
{
    return build(context, left, &right, nullptr, Operation::add, capacity_lines);
}

BgvArithmeticApplication build_bgv_subtract_application(
    const ::seal::SEALContext& context, const ::seal::Ciphertext& left,
    const ::seal::Ciphertext& right, std::uint64_t capacity_lines)
{
    return build(context, left, &right, nullptr, Operation::subtract, capacity_lines);
}

BgvArithmeticApplication build_bgv_negate_application(
    const ::seal::SEALContext& context, const ::seal::Ciphertext& input,
    std::uint64_t capacity_lines)
{
    return build(context, input, nullptr, nullptr, Operation::negate, capacity_lines);
}

BgvArithmeticApplication build_bgv_add_plain_application(
    const ::seal::SEALContext& context, const ::seal::Ciphertext& ciphertext,
    const ::seal::Plaintext& plaintext, std::uint64_t capacity_lines)
{
    return build(context, ciphertext, nullptr, &plaintext,
                 Operation::add_plain, capacity_lines);
}

BgvArithmeticApplication build_bgv_subtract_plain_application(
    const ::seal::SEALContext& context, const ::seal::Ciphertext& ciphertext,
    const ::seal::Plaintext& plaintext, std::uint64_t capacity_lines)
{
    return build(context, ciphertext, nullptr, &plaintext,
                 Operation::subtract_plain, capacity_lines);
}

BgvArithmeticApplication build_bgv_multiply_plain_application(
    const ::seal::SEALContext& context, const ::seal::Ciphertext& ciphertext,
    const ::seal::Plaintext& plaintext, std::uint64_t capacity_lines)
{
    return build(context, ciphertext, nullptr, &plaintext,
                 Operation::multiply_plain, capacity_lines);
}

BgvArithmeticRuntimeArtifacts render_bgv_arithmetic_runtime_artifacts(
    const std::string& stem, const BgvArithmeticApplication& application)
{
    return render_bgv_keyswitch_runtime_artifacts(stem, application);
}

} // namespace hpu::seal_adapter
