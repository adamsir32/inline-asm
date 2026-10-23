#pragma once

#include "hpu/compiler/operator_resources.hpp"
#include "hpu/model/hardware_ntt.hpp"

#include <map>
#include <stdexcept>

namespace hpu::seal_adapter::detail {

template <class Binding>
auto set_local_dma_index(Binding& binding, std::size_t index, int)
    -> decltype(binding.operation_dma_index = index, void())
{
    binding.operation_dma_index = index;
}

template <class Binding>
void set_local_dma_index(Binding&, std::size_t, long) {}

// Resolve logical buffer references directly. Repeated references intentionally
// share one allocation; their eventual DMA order is a backend decision.
template <class Binding>
std::vector<Binding> bind_native_application_ports(
    hpu::resource::OperatorProgram& program,
    const hpu::runtime::HpuMemImage& image,
    std::size_t degree,
    const std::map<std::string, std::string>& aliases = {})
{
    std::vector<Binding> result;
    result.reserve(program.ports.size());
    for (std::size_t index = 0; index < program.ports.size(); ++index) {
        const auto& port = program.ports[index];
        if (port.logical_binding.empty())
            throw std::logic_error("native application has an unnamed data reference");
        const auto alias = aliases.find(port.logical_binding);
        const auto& id = alias == aliases.end() ? port.logical_binding : alias->second;
        const auto& allocation = image.allocation(id);
        const auto& operation = program.operations.at(port.operation_index);
        const auto& instruction = operation.instruction;
        const auto direction = port.load ? hpu::Mnemonic::kDload : hpu::Mnemonic::kDstore;
        const bool modulus = port.load && instruction.type == 1;
        const bool stage = port.load && allocation.kind == hpu::runtime::AllocationKind::twiddle
            && port.logical_binding.find("/stage") != std::string::npos;
        const auto words = modulus ? allocation.word_count
            : stage ? hpu::model::kStageTwiddleParameterWords : degree;
        if (instruction.mnemonic != direction || allocation.word_count != words
            || allocation.span.line_count == 0 || (!port.load && allocation.read_only)
            || (modulus && (allocation.kind != hpu::runtime::AllocationKind::modulus_table
                            || !allocation.read_only))) {
            throw std::invalid_argument("invalid native application data reference: " + id);
        }
        const auto value = port.load ? operation.outputs.at(0) : operation.inputs.at(0);
        Binding binding;
        binding.dma_index = index;
        binding.instruction_index = port.operation_index;
        binding.direction = direction;
        binding.object_slot = static_cast<std::uint8_t>(
            program.values.at(value).variable.compatibility_slot);
        binding.type_or_release = instruction.type;
        binding.flag = instruction.dma_flag;
        binding.allocation_id = allocation.id;
        binding.span = allocation.span;
        binding.cp = instruction.cp;
        set_local_dma_index(binding, index, 0);
        result.push_back(std::move(binding));
        program.bind_port(index, {port.operation_index, allocation.id,
                                 allocation.span, 0, "native", index});
    }
    return result;
}

} // namespace hpu::seal_adapter::detail
