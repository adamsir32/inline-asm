#include "hpu/seal/operation_relocation.hpp"

#include "hpu/seal/ckks_level.hpp"
#include "hpu/model/hardware_ntt.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace hpu::seal_adapter {
namespace {

constexpr const char* kModulusTableId = "constants/modulus_table";
constexpr const char* kApplicationOperationId = "$application";
constexpr int kModulusTableObject = 4;

struct DmaInstruction {
    CkksDmaDirection direction = CkksDmaDirection::load;
    int object_slot = 0;
    hpu::DataType load_type = hpu::DataType::poly;
    hpu::DloadFlag load_flag = hpu::DloadFlag::regular_bank;
    int store_release = 0;
    std::uint8_t cp = 0;
    std::string logical_binding;
};

std::vector<DmaInstruction> declared_dma_instructions(
    const hpu::resource::OperatorProgram* program)
{
    if (!program)
        throw std::invalid_argument("CKKS relocation requires native operator IR");
    std::vector<DmaInstruction> result;
    for (const auto& port : program->ports) {
        if (port.logical_binding.empty())
            throw std::invalid_argument("CKKS relocation requires named logical data ports");
        const auto& node = program->operations.at(port.operation_index);
        DmaInstruction dma;
        dma.direction = port.load ? CkksDmaDirection::load : CkksDmaDirection::store;
        const auto id = port.load ? node.outputs.at(0) : node.inputs.at(0);
        dma.object_slot = program->values.at(id).variable.compatibility_slot;
        dma.load_type = node.instruction.type == 1 ? hpu::DataType::mod_ctx : hpu::DataType::poly;
        dma.load_flag = static_cast<hpu::DloadFlag>(node.instruction.dma_flag);
        dma.store_release = node.instruction.type;
        dma.cp = node.instruction.cp;
        dma.logical_binding = port.logical_binding;
        result.push_back(dma);
    }
    return result;
}

bool same_instruction(
    const DmaInstruction& left,
    const DmaInstruction& right)
{
    return left.cp == right.cp && left.direction == right.direction
        && left.logical_binding == right.logical_binding
        && (left.direction == CkksDmaDirection::load
            ? left.load_type == right.load_type
                && left.load_flag == right.load_flag
            : left.store_release == right.store_release);
}

std::string limb_id(
    const CkksPlannedValue& value,
    std::size_t component,
    int modulus_id)
{
    return value.id + "/c" + std::to_string(component)
        + "/mod" + std::to_string(modulus_id);
}

class OperationBindingBuilder {
public:
    OperationBindingBuilder(
        CkksRelocationSchedule& schedule,
        const hpu::runtime::HpuMemImage& image,
        const CkksLoweredOperation& operation,
        std::size_t operation_index,
        std::size_t first_program_dma_index,
        std::size_t polynomial_words,
        std::vector<DmaInstruction> instructions)
        : schedule_(schedule), image_(image), operation_(operation),
          operation_index_(operation_index),
          first_program_dma_index_(first_program_dma_index),
          polynomial_words_(polynomial_words),
          instructions_(std::move(instructions))
    {}

    void load_named(const std::string& name, const std::string& allocation_id,
                    std::size_t words = 0)
    {
        bind_named(name, allocation_id, true, words ? words : polynomial_words_);
    }

    void store_named(const std::string& name, const std::string& allocation_id)
    {
        bind_named(name, allocation_id, false, polynomial_words_);
    }

    void finish() const
    {
        if (cursor_ != instructions_.size()) {
            throw std::logic_error(
                "relocation recipe did not consume every generated DMA instruction for "
                + operation_.operation.id);
        }
    }

private:
    void bind_named(const std::string& name, const std::string& allocation_id, bool load,
                    std::size_t expected_words)
    {
        const auto found = std::find_if(instructions_.begin(), instructions_.end(),
            [&](const auto& instruction) { return instruction.logical_binding == name; });
        if (found == instructions_.end()) {
            throw std::logic_error("missing logical data port: " + name);
        }
        const auto index = static_cast<std::size_t>(found - instructions_.begin());
        append(load ? CkksDmaDirection::load : CkksDmaDirection::store,
               found->object_slot, allocation_id, found->load_type,
               found->load_flag, found->store_release, expected_words, index);
    }

    void append(
        CkksDmaDirection direction,
        int slot,
        const std::string& allocation_id,
        hpu::DataType load_type,
        hpu::DloadFlag load_flag,
        int store_release,
        std::size_t expected_words,
        std::optional<std::size_t> port_index = {})
    {
        const auto index = port_index.value_or(cursor_);
        if (index >= instructions_.size() || bound_.count(index)) {
            throw std::logic_error(
                "relocation recipe exceeds generated DMA instructions for "
                + operation_.operation.id);
        }
        const auto& instruction = instructions_[index];
        if (instruction.direction != direction
            || instruction.object_slot != slot
            || (direction == CkksDmaDirection::load
                && (hpu::dma_mod_bit(instruction.load_type) != hpu::dma_mod_bit(load_type)
                    || instruction.load_flag != load_flag))
            || (direction == CkksDmaDirection::store
                && instruction.store_release != store_release)) {
            throw std::logic_error(
                "relocation recipe disagrees with generated DMA ABI for "
                + operation_.operation.id);
        }

        const auto& allocation = image_.allocation(allocation_id);
        if (allocation.word_count != expected_words
            || allocation.span.line_count == 0) {
            throw std::invalid_argument(
                "CKKS polynomial relocation has an incompatible allocation: "
                + allocation_id);
        }
        if (direction == CkksDmaDirection::store && allocation.read_only) {
            throw std::invalid_argument(
                "CKKS dstore relocation targets a read-only allocation: "
                + allocation_id);
        }

        CkksDmaBinding binding;
        binding.program_dma_index = first_program_dma_index_ + index;
        binding.operation_index = operation_index_;
        binding.operation_dma_index = index;
        binding.operation_id = operation_.operation.id;
        binding.direction = direction;
        binding.object_slot = instruction.object_slot;
        binding.load_type = instruction.load_type;
        binding.cp = instruction.cp;
        binding.load_flag = instruction.load_flag;
        binding.store_release = instruction.store_release;
        binding.allocation_id = allocation.id;
        binding.span = allocation.span;
        schedule_.bindings.push_back(std::move(binding));
        bound_.insert(index);
        while (bound_.count(cursor_)) ++cursor_;
    }

    CkksRelocationSchedule& schedule_;
    const hpu::runtime::HpuMemImage& image_;
    const CkksLoweredOperation& operation_;
    std::size_t operation_index_;
    std::size_t first_program_dma_index_;
    std::size_t polynomial_words_;
    std::vector<DmaInstruction> instructions_;
    std::size_t cursor_ = 0;
    std::set<std::size_t> bound_;
};

void bind_square(
    OperationBindingBuilder& bindings,
    const CkksOperationStep& step,
    const CkksLevelDescriptor& level)
{
    if (step.inputs.size() != 1 || step.inputs[0].component_count != 2
        || step.output.component_count != 3) {
        throw std::invalid_argument("invalid Square relocation manifest");
    }
    const auto& input = step.inputs[0];
    for (int modulus_id : level.rns_layout.q_mod_ids) {
        const auto prefix = "tensor/mod" + std::to_string(modulus_id) + "/";
        bindings.load_named(prefix + "left/c0", limb_id(input, 0, modulus_id));
        bindings.load_named(prefix + "right/c0", limb_id(input, 0, modulus_id));
        bindings.load_named(prefix + "left/c1", limb_id(input, 1, modulus_id));
        bindings.load_named(prefix + "right/c1", limb_id(input, 1, modulus_id));
        for (std::size_t component = 0; component < 3; ++component) {
            bindings.store_named(prefix + "output/c" + std::to_string(component),
                                 limb_id(step.output, component, modulus_id));
        }
    }
    bindings.finish();
}

void bind_multiply(
    OperationBindingBuilder& bindings,
    const CkksOperationStep& step,
    const CkksLevelDescriptor& level)
{
    if (step.inputs.size() != 2 || step.inputs[0].component_count != 2
        || step.inputs[1].component_count != 2
        || step.output.component_count != 3) {
        throw std::invalid_argument("invalid Multiply relocation manifest");
    }
    const auto& left = step.inputs[0];
    const auto& right = step.inputs[1];
    for (int modulus_id : level.rns_layout.q_mod_ids) {
        const auto prefix = "tensor/mod" + std::to_string(modulus_id) + "/";
        bindings.load_named(prefix + "left/c0", limb_id(left, 0, modulus_id));
        bindings.load_named(prefix + "right/c0", limb_id(right, 0, modulus_id));
        bindings.load_named(prefix + "left/c1", limb_id(left, 1, modulus_id));
        bindings.load_named(prefix + "right/c1", limb_id(right, 1, modulus_id));
        for (std::size_t component = 0; component < 3; ++component) {
            bindings.store_named(prefix + "output/c" + std::to_string(component),
                                 limb_id(step.output, component, modulus_id));
        }
    }
    bindings.finish();
}

void bind_ciphertext_binary(
    OperationBindingBuilder& bindings,
    const CkksOperationStep& step,
    const CkksLevelDescriptor& level)
{
    if (step.inputs.size() != 2 || step.inputs[0].component_count != 2
        || step.inputs[1].component_count != 2
        || step.output.component_count != 2) {
        throw std::invalid_argument(
            "invalid Add/Subtract relocation manifest");
    }
    const auto& left = step.inputs[0];
    const auto& right = step.inputs[1];
    for (std::size_t component = 0; component < 2; ++component) {
        for (int modulus_id : level.rns_layout.q_mod_ids) {
            const auto prefix = "pointwise/mod" + std::to_string(modulus_id) + "/";
            const auto limb = "c" + std::to_string(component);
            bindings.load_named(prefix + "left/" + limb, limb_id(left, component, modulus_id));
            bindings.load_named(prefix + "right/" + limb, limb_id(right, component, modulus_id));
            bindings.store_named(prefix + "output/" + limb, limb_id(step.output, component, modulus_id));
        }
    }
    bindings.finish();
}

void bind_plain_binary(
    OperationBindingBuilder& bindings,
    const CkksOperationStep& step,
    const CkksLevelDescriptor& level)
{
    if (step.inputs.size() != 2 || step.inputs[0].component_count != 2
        || step.inputs[1].component_count != 1
        || step.output.component_count != 2) {
        throw std::invalid_argument("invalid AddPlain relocation manifest");
    }
    const auto& ciphertext = step.inputs[0];
    const auto& plaintext = step.inputs[1];
    for (int modulus_id : level.rns_layout.q_mod_ids) {
        const auto prefix = "pointwise/mod" + std::to_string(modulus_id) + "/";
        bindings.load_named(prefix + "left/c0", limb_id(ciphertext, 0, modulus_id));
        bindings.load_named(prefix + "plaintext", limb_id(plaintext, 0, modulus_id));
        bindings.store_named(prefix + "output/c0", limb_id(step.output, 0, modulus_id));
        bindings.load_named(prefix + "left/c1", limb_id(ciphertext, 1, modulus_id));
        bindings.store_named(prefix + "output/c1", limb_id(step.output, 1, modulus_id));
    }
    bindings.finish();
}

void bind_multiply_plain(
    OperationBindingBuilder& bindings,
    const CkksOperationStep& step,
    const CkksLevelDescriptor& level)
{
    if (step.inputs.size() != 2 || step.inputs[0].component_count != 2
        || step.inputs[1].component_count != 1
        || step.output.component_count != 2) {
        throw std::invalid_argument(
            "invalid MultiplyPlain relocation manifest");
    }
    const auto& ciphertext = step.inputs[0];
    const auto& plaintext = step.inputs[1];
    for (int modulus_id : level.rns_layout.q_mod_ids) {
        const auto prefix = "pointwise/mod" + std::to_string(modulus_id) + "/";
        bindings.load_named(prefix + "left/c0", limb_id(ciphertext, 0, modulus_id));
        bindings.load_named(prefix + "plaintext", limb_id(plaintext, 0, modulus_id));
        bindings.store_named(prefix + "output/c0", limb_id(step.output, 0, modulus_id));
        bindings.load_named(prefix + "left/c1", limb_id(ciphertext, 1, modulus_id));
        bindings.store_named(prefix + "output/c1", limb_id(step.output, 1, modulus_id));
    }
    bindings.finish();
}

void bind_negate(
    OperationBindingBuilder& bindings,
    const CkksOperationStep& step,
    const CkksLevelDescriptor& level)
{
    if (step.inputs.size() != 1 || step.inputs[0].component_count != 2
        || step.output.component_count != 2) {
        throw std::invalid_argument("invalid Negate relocation manifest");
    }
    const auto& input = step.inputs[0];
    for (std::size_t component = 0; component < 2; ++component) {
        for (int modulus_id : level.rns_layout.q_mod_ids) {
            const auto prefix = "pointwise/mod" + std::to_string(modulus_id) + "/";
            const auto limb = "c" + std::to_string(component);
            bindings.load_named(prefix + "left/" + limb, limb_id(input, component, modulus_id));
            bindings.store_named(prefix + "output/" + limb, limb_id(step.output, component, modulus_id));
        }
    }
    bindings.finish();
}

std::size_t ntt_stage_count(std::size_t degree)
{
    std::size_t stages = 0;
    for (std::size_t remaining = degree; remaining > 1; remaining >>= 1U) {
        ++stages;
    }
    return stages;
}

std::string canonical_twiddle_id(
    int modulus_id,
    bool inverse,
    const std::string& suffix)
{
    return "constants/twiddle/canonical/mod" + std::to_string(modulus_id)
        + (inverse ? "/intt/" : "/ntt/") + suffix;
}

std::string workspace_mod_id(
    const std::string& prefix,
    const std::string& role,
    int modulus_id)
{
    return prefix + "/workspace/" + role
        + "/mod" + std::to_string(modulus_id);
}

void bind_transform_limb(
    OperationBindingBuilder& bindings,
    const std::string& port_prefix,
    const std::string& data_id,
    int modulus_id,
    std::size_t degree,
    bool inverse,
    const std::string& output_id = {},
    const std::string& inverse_twiddle_prefix = {})
{
    bindings.load_named(port_prefix + "/input", data_id);
    const auto table = port_prefix + "/table/";
    const auto twiddle = [&](const std::string& suffix) {
        return inverse && !inverse_twiddle_prefix.empty()
            ? inverse_twiddle_prefix + "/" + suffix
            : canonical_twiddle_id(modulus_id, inverse, suffix);
    };
    if (!inverse) bindings.load_named(table + "pre_twist", twiddle("pre_twist"),
                                     hpu::model::kOnlinePmulParameterWords);
    for (std::size_t stage = 0; stage < ntt_stage_count(degree); ++stage) {
        const auto suffix = "stage" + std::to_string(stage);
        bindings.load_named(table + suffix, twiddle(suffix),
                            hpu::model::kStageTwiddleParameterWords);
    }
    if (inverse) bindings.load_named(table + "post_untwist_scale", twiddle("post_untwist_scale"),
                                    hpu::model::kOnlinePmulParameterWords);
    bindings.store_named(port_prefix + "/output", output_id.empty() ? data_id : output_id);
}

void bind_bconv_ports(
    OperationBindingBuilder& bindings,
    const std::string& prefix,
    const std::vector<int>& sources,
    const std::vector<int>& targets,
    const std::vector<std::string>& source_ids,
    const std::vector<std::string>& target_ids,
    const std::string& constant_prefix,
    const std::string& normalized_prefix)
{
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto source = std::to_string(sources[index]);
        bindings.load_named(prefix + "/source/mod" + source, source_ids.at(index));
        bindings.load_named(prefix + "/inverse/mod" + source,
            constant_prefix + "/qhat_inv/mod" + source);
        bindings.store_named(prefix + "/normalized/mod" + source,
            normalized_prefix + std::to_string(index));
    }
    for (std::size_t target_index = 0; target_index < targets.size(); ++target_index) {
        const auto target = std::to_string(targets[target_index]);
        for (std::size_t source_index = 0; source_index < sources.size(); ++source_index) {
            const auto source = std::to_string(sources[source_index]);
            bindings.load_named(prefix + "/accumulate/to" + target + "/source/mod" + source,
                normalized_prefix + std::to_string(source_index));
            bindings.load_named(prefix + "/factor/to" + target + "/from" + source,
                constant_prefix + "/qhat_mod_target/target" + target + "/source" + source);
        }
        bindings.store_named(prefix + "/output/mod" + target, target_ids.at(target_index));
    }
}

std::vector<std::string> workspace_ids(
    const std::string& prefix, const std::string& role, const std::vector<int>& contexts)
{
    std::vector<std::string> result;
    for (int context : contexts) result.push_back(workspace_mod_id(prefix, role, context));
    return result;
}

void bind_keyswitch_core(
    OperationBindingBuilder& bindings,
    const std::string& port_prefix,
    const CkksPlannedValue& switching,
    std::size_t switching_component,
    const CkksPlannedValue& base,
    std::size_t base_component,
    const CkksPlannedValue& output,
    const hpu::RnsDecompositionLayout& layout,
    const std::string& evaluation_key_id,
    const std::string& hardware_prefix,
    std::size_t degree,
    bool direct_second_output)
{
    std::vector<int> full_contexts = layout.q_mod_ids;
    full_contexts.insert(full_contexts.end(), layout.p_mod_ids.begin(), layout.p_mod_ids.end());
    const auto normalized = hardware_prefix + "/workspace/bconv/normalized";
    for (std::size_t digit = 0; digit < layout.key_digits.size(); ++digit) {
        const auto logical = port_prefix + "/digit" + std::to_string(digit);
        const auto constants = hardware_prefix + "/modup/d" + std::to_string(digit);
        const auto& sources = layout.key_digits[digit];
        std::vector<int> targets;
        std::vector<std::string> source_ids;
        for (int context : layout.q_mod_ids) {
            if (std::find(sources.begin(), sources.end(), context) == sources.end())
                targets.push_back(context);
        }
        targets.insert(targets.end(), layout.p_mod_ids.begin(), layout.p_mod_ids.end());
        for (int source : sources) {
            const auto suffix = "/mod" + std::to_string(source);
            const auto input = limb_id(switching, switching_component, source);
            source_ids.push_back(input);
            bindings.load_named(logical + "/modup/copy/input" + suffix, input);
            bindings.store_named(logical + "/modup/copy/output" + suffix,
                workspace_mod_id(hardware_prefix, "modup", source));
        }
        bind_bconv_ports(bindings, logical + "/modup/bconv", sources, targets,
            source_ids, workspace_ids(hardware_prefix, "modup", targets), constants, normalized);
        for (int context : full_contexts) {
            bind_transform_limb(bindings, logical + "/forward/mod" + std::to_string(context),
                workspace_mod_id(hardware_prefix, "modup", context), context, degree, false);
        }
        for (int component = 0; component < 2; ++component) {
            for (int context : full_contexts) {
                const auto suffix = "/c" + std::to_string(component) + "/mod" + std::to_string(context);
                const auto product = logical + "/product" + suffix;
                const auto accumulator = workspace_mod_id(hardware_prefix,
                    "accumulator/c" + std::to_string(component), context);
                bindings.load_named(product + "/input", workspace_mod_id(hardware_prefix, "modup", context));
                bindings.load_named(product + "/key", evaluation_key_id + "/d" + std::to_string(digit) + suffix);
                if (digit != 0) bindings.load_named(product + "/accumulator", accumulator);
                bindings.store_named(product + "/output", accumulator);
            }
        }
    }
    for (int component = 0; component < 2; ++component) {
        const auto component_name = "c" + std::to_string(component);
        const auto accumulator_role = "accumulator/" + component_name;
        for (int context : full_contexts) {
            bind_transform_limb(bindings, port_prefix + "/inverse/" + component_name
                + "/mod" + std::to_string(context),
                workspace_mod_id(hardware_prefix, accumulator_role, context), context, degree, true);
        }
        const auto rounded = port_prefix + "/rounded_moddown/" + component_name;
        for (int context : full_contexts) {
            const auto suffix = "/mod" + std::to_string(context);
            const auto accumulator = workspace_mod_id(hardware_prefix, accumulator_role, context);
            bindings.load_named(rounded + "/half/input" + suffix, accumulator);
            bindings.load_named(rounded + "/half/constant" + suffix, hardware_prefix + "/half" + suffix);
            bindings.store_named(rounded + "/half/output" + suffix, accumulator);
        }
        const auto moddown = rounded + "/moddown";
        bind_bconv_ports(bindings, moddown + "/bconv", layout.p_mod_ids, layout.q_mod_ids,
            workspace_ids(hardware_prefix, accumulator_role, layout.p_mod_ids),
            workspace_ids(hardware_prefix, "moddown/correction", layout.q_mod_ids),
            hardware_prefix + "/moddown", normalized);
        for (int context : layout.q_mod_ids) {
            const auto suffix = "/mod" + std::to_string(context);
            const auto accumulator = workspace_mod_id(hardware_prefix, accumulator_role, context);
            bindings.load_named(moddown + "/input" + suffix, accumulator);
            bindings.load_named(moddown + "/correction" + suffix,
                workspace_mod_id(hardware_prefix, "moddown/correction", context));
            bindings.load_named(moddown + "/inverse" + suffix,
                hardware_prefix + "/moddown/p_inverse" + suffix);
            bindings.store_named(moddown + "/output" + suffix,
                direct_second_output && component == 1 ? limb_id(output, 1, context) : accumulator);
        }
    }
    for (int context : layout.q_mod_ids) {
        const auto logical = port_prefix + "/merge/mod" + std::to_string(context);
        bindings.load_named(logical + "/value", workspace_mod_id(hardware_prefix, "accumulator/c0", context));
        bindings.load_named(logical + "/base", limb_id(base, base_component, context));
        bindings.store_named(logical + "/output", limb_id(output, 0, context));
    }
}

void bind_relinearize(
    OperationBindingBuilder& bindings,
    const CkksOperationStep& step,
    const CkksLevelDescriptor& level,
    std::size_t degree)
{
    if (step.inputs.size() != 1 || step.inputs[0].component_count != 3
        || step.workspaces.size() > 1
        || step.output.component_count != 2
        || step.resources.evaluation_key_ids.size() != 1
        || step.resources.constant_ids.size() != 1
        || !step.resources.requires_canonical_twiddles) {
        throw std::invalid_argument(
            "invalid Relinearize relocation manifest");
    }
    const auto& tensor = step.inputs[0];
    const auto& coefficients = step.workspaces.empty() ? tensor : step.workspaces.front();
    const auto& layout = level.rns_layout;
    const std::string hardware_prefix =
        step.resources.constant_ids[0] + "/hardware";

    for (std::size_t component = 0; component < 3; ++component) {
        for (int context : layout.q_mod_ids) {
            bind_transform_limb(
                bindings, "relinearize/inverse/c" + std::to_string(component)
                    + "/mod" + std::to_string(context), limb_id(tensor, component, context),
                context, degree, true, limb_id(coefficients, component, context));
        }
    }

    bind_keyswitch_core(
        bindings, "relinearize/keyswitch", coefficients, 2, coefficients, 0, step.output, layout,
        step.resources.evaluation_key_ids[0], hardware_prefix,
        degree, false);

    for (int context : layout.q_mod_ids) {
        const auto prefix = "relinearize/merge/mod" + std::to_string(context);
        bindings.load_named(prefix + "/tensor", limb_id(coefficients, 1, context));
        bindings.load_named(prefix + "/switched",
            workspace_mod_id(hardware_prefix, "accumulator/c1", context));
        bindings.store_named(prefix + "/output", limb_id(step.output, 1, context));
    }

    for (std::size_t component = 0; component < 2; ++component) {
        for (int context : layout.q_mod_ids) {
            bind_transform_limb(
                bindings, "relinearize/forward/c" + std::to_string(component)
                    + "/mod" + std::to_string(context),
                limb_id(step.output, component, context), context, degree, false);
        }
    }
    bindings.finish();
}

void bind_galois(
    OperationBindingBuilder& bindings,
    const CkksOperationStep& step,
    const CkksLevelDescriptor& level,
    std::size_t degree)
{
    if (step.inputs.size() != 1 || step.inputs[0].component_count != 2
        || step.workspaces.size() != 1
        || step.workspaces[0].component_count != 2
        || step.workspaces[0].domain
            != hpu::runtime::PolynomialDomain::coefficient
        || step.workspaces[0].key_domain
            != step.resources.galois_element
        || step.output.component_count != 2
        || step.resources.evaluation_key_ids.size() != 1
        || step.resources.constant_ids.size() != 1
        || step.resources.fused_twiddle_ids.size()
            != level.rns_layout.q_mod_ids.size()
        || !step.resources.requires_canonical_twiddles) {
        throw std::invalid_argument(
            "invalid Rotate/Conjugate relocation manifest");
    }
    const auto& input = step.inputs[0];
    const auto& workspace = step.workspaces[0];
    const auto& layout = level.rns_layout;
    const std::string hardware_prefix =
        step.resources.constant_ids[0] + "/hardware";

    for (std::size_t component = 0; component < 2; ++component) {
        for (std::size_t basis = 0;
             basis < layout.q_mod_ids.size(); ++basis) {
            const int context = layout.q_mod_ids[basis];
            bind_transform_limb(bindings, "galois/inverse/c" + std::to_string(component)
                    + "/mod" + std::to_string(context),
                limb_id(input, component, context), context, degree, true,
                limb_id(workspace, component, context), step.resources.fused_twiddle_ids[basis]);
        }
    }

    bind_keyswitch_core(
        bindings, "keyswitch", workspace, 1, workspace, 0, step.output, layout,
        step.resources.evaluation_key_ids[0], hardware_prefix,
        degree, true);

    for (std::size_t component = 0; component < 2; ++component) {
        for (int context : layout.q_mod_ids) {
            bind_transform_limb(
                bindings, "galois/forward/c" + std::to_string(component)
                    + "/mod" + std::to_string(context),
                limb_id(step.output, component, context), context, degree, false);
        }
    }
    bindings.finish();
}

void bind_rescale(
    OperationBindingBuilder& bindings,
    const CkksOperationStep& step,
    const CkksLevelDescriptor& source_level,
    const CkksLevelDescriptor& destination_level,
    std::size_t degree)
{
    if (step.inputs.size() != 1
        || step.workspaces.size() > 1
        || (step.inputs[0].component_count != 2 && step.inputs[0].component_count != 3)
        || step.output.component_count != step.inputs[0].component_count
        || step.resources.constant_ids.size() != 1
        || !step.resources.evaluation_key_ids.empty()
        || !step.resources.requires_canonical_twiddles
        || source_level.rns_layout.q_mod_ids.size() < 2
        || destination_level.rns_layout.q_mod_ids.size() + 1
            != source_level.rns_layout.q_mod_ids.size()
        || !std::equal(
            destination_level.rns_layout.q_mod_ids.begin(),
            destination_level.rns_layout.q_mod_ids.end(),
            source_level.rns_layout.q_mod_ids.begin())) {
        throw std::invalid_argument("invalid Rescale relocation manifest");
    }
    const auto& input = step.inputs[0];
    const auto& coefficients = step.workspaces.empty() ? input : step.workspaces.front();
    const auto& source_contexts = source_level.rns_layout.q_mod_ids;
    const auto& destination_contexts =
        destination_level.rns_layout.q_mod_ids;
    const int dropped_context = source_contexts.back();
    const std::string hardware_prefix =
        step.resources.constant_ids[0] + "/hardware";

    for (std::size_t component = 0; component < input.component_count; ++component) {
        for (int context : source_contexts) {
            bind_transform_limb(
                bindings, "rescale/inverse/c" + std::to_string(component)
                    + "/mod" + std::to_string(context), limb_id(input, component, context),
                context, degree, true, limb_id(coefficients, component, context));
        }
    }

    for (std::size_t component = 0; component < input.component_count; ++component) {
        const auto role = "rounded/c" + std::to_string(component);
        const auto prefix = "rounded_drop_last/c" + std::to_string(component);
        for (int context : source_contexts) {
            const auto suffix = "/mod" + std::to_string(context);
            bindings.load_named(prefix + "/half/input" + suffix, limb_id(coefficients, component, context));
            bindings.load_named(prefix + "/half/constant" + suffix, hardware_prefix + "/half" + suffix);
            bindings.store_named(prefix + "/half/output" + suffix,
                workspace_mod_id(hardware_prefix, role, context));
        }
        const auto moddown = prefix + "/moddown";
        bind_bconv_ports(bindings, moddown + "/bconv", {dropped_context}, destination_contexts,
            workspace_ids(hardware_prefix, role, {dropped_context}),
            workspace_ids(hardware_prefix, "moddown/correction", destination_contexts),
            hardware_prefix + "/moddown", hardware_prefix + "/workspace/bconv/normalized");
        for (int context : destination_contexts) {
            const auto suffix = "/mod" + std::to_string(context);
            bindings.load_named(moddown + "/input" + suffix, workspace_mod_id(hardware_prefix, role, context));
            bindings.load_named(moddown + "/correction" + suffix,
                workspace_mod_id(hardware_prefix, "moddown/correction", context));
            bindings.load_named(moddown + "/inverse" + suffix,
                hardware_prefix + "/moddown/q_last_inverse" + suffix);
            bindings.store_named(moddown + "/output" + suffix, limb_id(step.output, component, context));
        }
    }
    for (std::size_t component = 0; component < input.component_count; ++component) {
        for (int context : destination_contexts) {
            bind_transform_limb(bindings, "rescale/forward/c" + std::to_string(component)
                + "/mod" + std::to_string(context),
                limb_id(step.output, component, context), context, degree, false);
        }
    }
    bindings.finish();
}

} // namespace

CkksRelocationSchedule build_ckks_relocation_schedule(
    const CkksLoweredProgram& program,
    const hpu::runtime::HpuMemImage& image,
    const ::seal::SEALContext& context)
{
    if (program.operations.empty()) {
        throw std::invalid_argument(
            "cannot relocate an empty CKKS lowered program");
    }
    const auto key_data = context.key_context_data();
    if (!key_data || key_data->parms().scheme() != ::seal::scheme_type::ckks
        || key_data->parms().poly_modulus_degree() == 0) {
        throw std::invalid_argument(
            "CKKS relocation requires a valid CKKS context");
    }
    const std::size_t degree = key_data->parms().poly_modulus_degree();
    const CkksLevelChain level_chain(context);

    const auto program_instructions = declared_dma_instructions(program.operator_program.get());
    std::vector<std::vector<DmaInstruction>> operation_instructions;
    operation_instructions.reserve(program.operations.size());
    std::size_t operation_dma_count = 0;
    for (const auto& operation : program.operations) {
        operation_instructions.push_back(
            declared_dma_instructions(operation.operator_program.get()));
        operation_dma_count += operation_instructions.back().size();
    }
    if (program_instructions.size() < operation_dma_count
        || program_instructions.size() - operation_dma_count > 1) {
        throw std::invalid_argument(
            "lowered CKKS program has an unsupported global DMA layout");
    }

    const std::size_t global_dma_count =
        program_instructions.size() - operation_dma_count;
    CkksRelocationSchedule result;
    result.expected_dma_count = program_instructions.size();
    if (global_dma_count == 1) {
        DmaInstruction expected{
            CkksDmaDirection::load,
            kModulusTableObject,
            hpu::DataType::mod_ctx,
            hpu::DloadFlag::small_bank,
            0};
        expected.cp = program_instructions.front().cp;
        expected.logical_binding = kModulusTableId;
        if (!same_instruction(program_instructions.front(), expected)) {
            throw std::invalid_argument(
                "lowered CKKS global DMA is not the modulus-table load");
        }
        const auto& allocation = image.allocation(kModulusTableId);
        if (allocation.kind != hpu::runtime::AllocationKind::modulus_table
            || !allocation.read_only || allocation.span.line_count == 0) {
            throw std::invalid_argument(
                "CKKS modulus-table relocation has an incompatible allocation");
        }
        CkksDmaBinding binding;
        binding.program_dma_index = 0;
        binding.operation_dma_index = 0;
        binding.operation_id = kApplicationOperationId;
        binding.direction = CkksDmaDirection::load;
        binding.object_slot = kModulusTableObject;
        binding.load_type = hpu::DataType::mod_ctx;
        binding.cp = program_instructions.front().cp;
        binding.load_flag = hpu::DloadFlag::small_bank;
        binding.allocation_id = allocation.id;
        binding.span = allocation.span;
        result.bindings.push_back(std::move(binding));
    }

    std::size_t first_program_dma_index = global_dma_count;
    for (std::size_t operation_index = 0;
         operation_index < program.operations.size(); ++operation_index) {
        const auto& operation = program.operations[operation_index];
        const auto& instructions = operation_instructions[operation_index];
        for (std::size_t local = 0; local < instructions.size(); ++local) {
            if (!same_instruction(
                    instructions[local],
                    program_instructions[first_program_dma_index + local])) {
                throw std::invalid_argument(
                    "lowered operation DMA stream differs from program body");
            }
        }

        const auto& step = operation.operation;
        switch (step.kind) {
        case CkksOperationKind::add:
        case CkksOperationKind::subtract: {
            if (step.inputs.empty()) {
                throw std::invalid_argument(
                    "Add/Subtract relocation manifest has no left input");
            }
            OperationBindingBuilder bindings(
                result, image, operation, operation_index,
                first_program_dma_index, degree, instructions);
            bind_ciphertext_binary(
                bindings, step,
                level_chain.require(step.inputs[0].metadata.parms_id));
            break;
        }
        case CkksOperationKind::multiply: {
            if (step.inputs.empty()) {
                throw std::invalid_argument(
                    "Multiply relocation manifest has no left input");
            }
            OperationBindingBuilder bindings(
                result, image, operation, operation_index,
                first_program_dma_index, degree, instructions);
            bind_multiply(
                bindings, step,
                level_chain.require(step.inputs[0].metadata.parms_id));
            break;
        }
        case CkksOperationKind::multiply_plain: {
            if (step.inputs.empty()) {
                throw std::invalid_argument(
                    "MultiplyPlain relocation manifest has no ciphertext input");
            }
            OperationBindingBuilder bindings(
                result, image, operation, operation_index,
                first_program_dma_index, degree, instructions);
            bind_multiply_plain(
                bindings, step,
                level_chain.require(step.inputs[0].metadata.parms_id));
            break;
        }
        case CkksOperationKind::square: {
            if (step.inputs.empty()) {
                throw std::invalid_argument(
                    "Square relocation manifest has no input");
            }
            OperationBindingBuilder bindings(
                result, image, operation, operation_index,
                first_program_dma_index, degree, instructions);
            bind_square(
                bindings, step, level_chain.require(step.inputs[0].metadata.parms_id));
            break;
        }
        case CkksOperationKind::add_plain:
        case CkksOperationKind::subtract_plain: {
            if (step.inputs.empty()) {
                throw std::invalid_argument(
                    "AddPlain relocation manifest has no ciphertext input");
            }
            OperationBindingBuilder bindings(
                result, image, operation, operation_index,
                first_program_dma_index, degree, instructions);
            bind_plain_binary(
                bindings, step, level_chain.require(step.inputs[0].metadata.parms_id));
            break;
        }
        case CkksOperationKind::negate: {
            if (step.inputs.empty()) {
                throw std::invalid_argument(
                    "Negate relocation manifest has no input");
            }
            OperationBindingBuilder bindings(
                result, image, operation, operation_index,
                first_program_dma_index, degree, instructions);
            bind_negate(
                bindings, step,
                level_chain.require(step.inputs[0].metadata.parms_id));
            break;
        }
        case CkksOperationKind::rotate:
        case CkksOperationKind::conjugate: {
            if (step.inputs.empty()) {
                throw std::invalid_argument(
                    "Rotate/Conjugate relocation manifest has no input");
            }
            OperationBindingBuilder bindings(
                result, image, operation, operation_index,
                first_program_dma_index, degree, instructions);
            bind_galois(
                bindings, step,
                level_chain.require(step.inputs[0].metadata.parms_id),
                degree);
            break;
        }
        case CkksOperationKind::relinearize: {
            if (step.inputs.empty()) {
                throw std::invalid_argument(
                    "Relinearize relocation manifest has no tensor input");
            }
            OperationBindingBuilder bindings(
                result, image, operation, operation_index,
                first_program_dma_index, degree, instructions);
            bind_relinearize(
                bindings, step,
                level_chain.require(step.inputs[0].metadata.parms_id),
                degree);
            break;
        }
        case CkksOperationKind::rescale: {
            if (step.inputs.empty()) {
                throw std::invalid_argument(
                    "Rescale relocation manifest has no ciphertext input");
            }
            const auto& source_level =
                level_chain.require(step.inputs[0].metadata.parms_id);
            OperationBindingBuilder bindings(
                result, image, operation, operation_index,
                first_program_dma_index, degree, instructions);
            bind_rescale(
                bindings, step, source_level,
                level_chain.next(source_level.parms_id), degree);
            break;
        }
        }
        first_program_dma_index += instructions.size();
    }
    if (first_program_dma_index != program_instructions.size()) {
        throw std::logic_error(
            "CKKS relocation did not account for the complete DMA stream");
    }
    std::sort(result.bindings.begin(), result.bindings.end(), [](const auto& left, const auto& right) {
        return left.program_dma_index < right.program_dma_index;
    });
    return result;
}

} // namespace hpu::seal_adapter
