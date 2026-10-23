#include "hpu/seal/bgv_modswitch_application.hpp"
#include "native_application_bindings.hpp"

#include "hpu/model/hardware_ntt.hpp"
#include "hpu/seal/ntt_bridge.hpp"
#include "scheme/bgv/modswitch.hpp"
#include "hpu/compiler/operator_resources.hpp"
#include "util/validation.hpp"

#include "assembler.hpp"
#include "executable.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace hpu::seal_adapter {
namespace {

std::uint32_t narrow(std::uint64_t value)
{
    if (value < 65537 || value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("BGV modulus is outside the HPU PE range");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint64_t barrett_mu(std::uint32_t modulus)
{
    return static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(1) << 64) / modulus);
}

std::string mod_id(const std::string& prefix, int id)
{
    return prefix + "/mod" + std::to_string(id);
}

std::string stage_id(const std::string& prefix, std::size_t stage)
{
    return prefix + "/stage" + std::to_string(stage);
}

std::vector<std::uint32_t> splat(std::size_t degree, std::uint32_t value)
{
    return std::vector<std::uint32_t>(degree, value);
}

std::size_t ntt_stage_count(std::size_t degree)
{
    std::size_t result = 0;
    for (std::size_t remaining = degree; remaining > 1; remaining >>= 1U) {
        ++result;
    }
    return result;
}

void add_twiddles(hpu::runtime::HpuMemImage& image, std::size_t degree,
                  int modulus_id, std::uint32_t modulus, std::uint32_t psi)
{
    const std::string prefix = mod_id("constants/twiddle/canonical", modulus_id);
    hpu::model::HardwareNttModel model(
        degree, modulus, hpu::model::pow_mod(psi, 2, modulus));
    const auto pre_twist = hpu::model::pack_online_pmul_parameters(
        1, psi, modulus, degree);
    image.add(prefix + "/ntt/pre_twist", pre_twist,
              hpu::runtime::AllocationKind::twiddle);
    const auto forward = model.forward_twiddles();
    for (std::size_t stage = 0; stage < forward.size(); ++stage) {
        image.add(stage_id(prefix + "/ntt", stage),
                  hpu::model::pack_stage_twiddles(forward[stage], modulus, degree),
                  hpu::runtime::AllocationKind::twiddle);
    }
    const auto inverse = model.inverse_twiddles();
    for (std::size_t stage = 0; stage < inverse.stages.size(); ++stage) {
        image.add(stage_id(prefix + "/intt", stage),
                  hpu::model::pack_stage_twiddles(inverse.stages[stage], modulus, degree),
                  hpu::runtime::AllocationKind::twiddle);
    }
    const auto post = hpu::model::pack_online_pmul_parameters(
        hpu::model::inverse_mod_prime(static_cast<std::uint32_t>(degree), modulus),
        hpu::model::inverse_mod_prime(psi, modulus), modulus, degree);
    image.add(prefix + "/intt/post_untwist_scale", post,
              hpu::runtime::AllocationKind::twiddle);
}

std::string csv_field(const std::string& value)
{
    std::string result = "\"";
    for (char character : value) {
        if (character == '"') {
            result += '"';
        }
        result += character;
    }
    return result + '"';
}

} // namespace

std::vector<hpu::runtime::HpuMemSpan> BgvModSwitchApplication::spans() const
{
    std::vector<hpu::runtime::HpuMemSpan> result;
    result.reserve(dma.size());
    for (const auto& binding : dma) {
        result.push_back(binding.span);
    }
    return result;
}

BgvModSwitchApplication build_bgv_modswitch_application(
    const ::seal::SEALContext& context, const ::seal::Ciphertext& input,
    std::uint64_t capacity_lines)
{
    const auto key_data = context.key_context_data();
    const auto first_data = context.first_context_data();
    const auto source = context.get_context_data(input.parms_id());
    if (!context.parameters_set() || !key_data || !first_data || !source ||
        !source->next_context_data() ||
        key_data->parms().scheme() != ::seal::scheme_type::bgv ||
        !::seal::is_valid_for(input, context) || !input.is_ntt_form() ||
        input.size() < 2) {
        throw std::invalid_argument("BGV ModSwitch requires a valid NTT ciphertext with a next level");
    }
    const std::size_t degree = source->parms().poly_modulus_degree();
    const auto& key_moduli = key_data->parms().coeff_modulus();
    const auto& top_moduli = first_data->parms().coeff_modulus();
    const auto& source_moduli = source->parms().coeff_modulus();
    const auto& destination_moduli =
        source->next_context_data()->parms().coeff_modulus();
    if (key_moduli.size() != top_moduli.size() + 1 ||
        key_moduli.size() + 1 > static_cast<std::size_t>(hpu::kMaxModContexts) ||
        source_moduli.size() < 2 ||
        destination_moduli.size() + 1 != source_moduli.size() ||
        source_moduli.size() > top_moduli.size() ||
        degree > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !hpu::is_valid_ntt_size(static_cast<int>(degree))) {
        throw std::invalid_argument("unsupported BGV Qmax|P|t level layout");
    }
    std::vector<std::uint32_t> q_moduli;
    for (std::size_t index = 0; index < source_moduli.size(); ++index) {
        if (source_moduli[index].value() != key_moduli[index].value() ||
            (index < destination_moduli.size() &&
             destination_moduli[index].value() != source_moduli[index].value())) {
            throw std::invalid_argument("BGV active Q is not a drop-last key-context prefix");
        }
        q_moduli.push_back(narrow(source_moduli[index].value()));
    }
    const std::uint32_t t = narrow(source->parms().plain_modulus().value());
    const auto constants = hpu::scheme::bgv::prepare_ntt_modswitch_constants(q_moduli, t);
    const int t_id = static_cast<int>(key_moduli.size());
    hpu::scheme::bgv::NttModSwitchLayout layout;
    layout.plaintext_mod_id = t_id;
    for (std::size_t index = 0; index < q_moduli.size(); ++index) {
        layout.q_mod_ids.push_back(static_cast<int>(index));
    }

    hpu::runtime::HpuMemImage image(capacity_lines);
    std::vector<std::uint32_t> modulus_words;
    for (const auto& modulus : key_moduli) {
        const std::uint32_t value = narrow(modulus.value());
        const std::uint64_t mu = barrett_mu(value);
        modulus_words.insert(modulus_words.end(), {
            value, static_cast<std::uint32_t>(mu),
            static_cast<std::uint32_t>(mu >> 32U), 0});
    }
    const std::uint64_t t_mu = barrett_mu(t);
    modulus_words.insert(modulus_words.end(), {
        t, static_cast<std::uint32_t>(t_mu),
        static_cast<std::uint32_t>(t_mu >> 32U), 0});
    image.add("constants/modulus_table", modulus_words,
              hpu::runtime::AllocationKind::modulus_table);
    const auto* seal_tables = source->small_ntt_tables();
    for (std::size_t basis = 0; basis < q_moduli.size(); ++basis) {
        add_twiddles(image, degree, static_cast<int>(basis), q_moduli[basis],
                     static_cast<std::uint32_t>(seal_tables[basis].get_root()));
    }
    const std::string constant_prefix = "constants/bgv_modswitch";
    image.add(constant_prefix + "/neg_inv_t",
              splat(degree, constants.neg_q_last_inverse_mod_t),
              hpu::runtime::AllocationKind::constant);
    for (std::size_t basis = 0; basis + 1 < q_moduli.size(); ++basis) {
        const int id = static_cast<int>(basis);
        image.add(mod_id(constant_prefix + "/q_last_mod", id),
                  splat(degree, constants.q_last_mod_q[basis]),
                  hpu::runtime::AllocationKind::constant);
        image.add(mod_id(constant_prefix + "/q_last_inv", id),
                  splat(degree, constants.q_last_inverse_mod_q[basis]),
                  hpu::runtime::AllocationKind::constant);
    }
    for (std::size_t component = 0; component < input.size(); ++component) {
        const auto prepared = ciphertext_component_to_hpu(input, component, context);
        for (std::size_t basis = 0; basis < q_moduli.size(); ++basis) {
            const auto first = prepared.words.begin() +
                static_cast<std::ptrdiff_t>(basis * degree);
            image.add(mod_id("input/c" + std::to_string(component), static_cast<int>(basis)),
                      std::vector<std::uint32_t>(first, first + degree),
                      hpu::runtime::AllocationKind::ciphertext);
        }
        image.reserve("workspace/bgv_modswitch/c" + std::to_string(component) + "/last_coeff",
                      degree, hpu::runtime::AllocationKind::workspace);
        image.reserve("workspace/bgv_modswitch/c" + std::to_string(component) + "/u_t",
                      degree, hpu::runtime::AllocationKind::workspace);
        for (std::size_t basis = 0; basis + 1 < q_moduli.size(); ++basis) {
            image.reserve(mod_id("output/c" + std::to_string(component), static_cast<int>(basis)),
                          degree, hpu::runtime::AllocationKind::output);
        }
    }

    hpu::resource::Recording recording;
    const auto body = hpu::scheme::bgv::generate_modswitch_ntt_body_asm(
        static_cast<int>(degree), layout, static_cast<int>(input.size()), true, true);
    auto instructions = hpu::assemble_source(body);
    hpu::validate_executable_program(instructions);
    auto native_program = recording.finish();
    auto bindings = detail::bind_native_application_ports<BgvModSwitchDmaBinding>(
        native_program, image, degree);
    BgvModSwitchApplication result{
        std::move(image), input.parms_id(), source->next_context_data()->parms_id(),
        hpu::scheme::bgv::modswitch_correction_factor(
            input.correction_factor(), q_moduli.back(), t),
        std::move(instructions), std::move(bindings),
            std::make_shared<hpu::resource::OperatorProgram>(std::move(native_program))};
    return result;
}

BgvModSwitchRuntimeArtifacts render_bgv_modswitch_runtime_artifacts(
    const std::string& stem, const BgvModSwitchApplication& application)
{
    if (application.image.capacity_lines() > std::numeric_limits<std::uint32_t>::max() ||
        application.dma.empty()) {
        throw std::invalid_argument("BGV ModSwitch runtime needs a nonempty uint32-addressable image");
    }
    const auto encoded = hpu::collect_dma_relocations(application.instructions);
    hpu::validate_executable_program(application.instructions);
    if (encoded.size() != application.dma.size()) {
        throw std::invalid_argument("BGV ModSwitch encoded DMA count differs from bindings");
    }
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const auto& binding = application.dma[index];
        const auto& allocation = application.image.allocation(binding.allocation_id);
        if (binding.dma_index != index ||
            binding.instruction_index != encoded[index].instruction_index ||
            binding.direction != encoded[index].direction ||
            binding.object_slot != encoded[index].object_id ||
            binding.type_or_release != encoded[index].type_or_release ||
            binding.flag != encoded[index].flag ||
            binding.cp != encoded[index].cp ||
            binding.span.line_offset != allocation.span.line_offset ||
            binding.span.line_count != allocation.span.line_count ||
            binding.span.line_offset >= application.image.capacity_lines() ||
            binding.span.line_count > application.image.capacity_lines() - binding.span.line_offset) {
            throw std::invalid_argument("BGV ModSwitch runtime binding is unresolved or out of range");
        }
    }
    const std::string identifier = hpu::c_identifier(stem);
    BgvModSwitchRuntimeArtifacts result;
    result.header = hpu::render_executable_header(stem, application.dma.size());
    const std::string marker = "#ifdef __cplusplus\n}\n#endif\n\n#endif\n";
    const auto position = result.header.find(marker);
    if (position == std::string::npos) {
        throw std::logic_error("BGV ModSwitch executable header has unexpected structure");
    }
    result.header.insert(position, "int hpu_run_" + identifier + "(void);\n\n");
    std::vector<hpu::ExecutableDmaSpan> fixed_spans;
    for (const auto& binding : application.dma) {
        fixed_spans.push_back({binding.span.line_offset, binding.span.line_count});
    }
    result.source = hpu::render_fixed_executable_source(
        stem, application.instructions, application.image.capacity_lines(), fixed_spans);

    std::ostringstream manifest;
    manifest << "instruction_index,dma_index,allocation_id,line_offset,line_count,word_hex\n";
    for (std::size_t index = 0; index < application.dma.size(); ++index) {
        const auto& binding = application.dma[index];
        manifest << binding.instruction_index << ',' << index << ','
                 << csv_field(binding.allocation_id) << ','
                 << binding.span.line_offset << ',' << binding.span.line_count << ','
                 << hpu::format_word_hex(
                        application.instructions[binding.instruction_index].word) << '\n';
    }
    result.resolved_dma_manifest = manifest.str();
    return result;
}

} // namespace hpu::seal_adapter
