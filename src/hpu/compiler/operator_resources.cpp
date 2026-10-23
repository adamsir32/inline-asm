#include "hpu/compiler/operator_resources.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace hpu::resource {
namespace {
thread_local Recording* current = nullptr;

int compatibility_slot(const Operand& operand)
{
    return operand.compatibility_slot;
}

void emit(hpu::Instruction instruction, const std::vector<Operand>& inputs,
          const std::vector<Operand>& outputs,
          std::optional<std::size_t> tied = {})
{
    if (!current) return;
    Operation op;
    op.instruction = instruction;
    for (const auto& input : inputs) op.inputs.push_back(current->value(input));
    if (!outputs.empty() && instruction.mnemonic != hpu::Mnemonic::kDload)
        op.reuse = current->existing(outputs.front());
    for (const auto& output : outputs) op.outputs.push_back(current->define(output));
    op.tied_input = tied;
    current->emit(std::move(op));
}

std::string arithmetic(hpu::Mnemonic mnemonic, Operand destination,
                       Operand left, Operand right, int immediate = -1)
{
    hpu::Instruction instruction;
    instruction.mnemonic = mnemonic;
    instruction.imm8 = immediate;
    std::vector<Operand> inputs{left};
    if (immediate < 0) inputs.push_back(right);
    if (mnemonic == hpu::Mnemonic::kPmac) inputs.push_back(destination);
    emit(instruction, inputs, {destination}, mnemonic == hpu::Mnemonic::kPmac
        ? std::optional<std::size_t>(inputs.size() - 1) : std::nullopt);
    const int d = compatibility_slot(destination), a = compatibility_slot(left);
    const int b = compatibility_slot(right);
    if (d < 0 || a < 0 || (immediate < 0 && b < 0)) return {};
    switch (mnemonic) {
    case hpu::Mnemonic::kPadd: return hpu::padd(d, a, b);
    case hpu::Mnemonic::kPsub: return hpu::psub(d, a, b);
    case hpu::Mnemonic::kPmul:
        return immediate < 0 ? hpu::pmul(d, a, b) : hpu::pmul_imm(d, a, immediate);
    case hpu::Mnemonic::kPmac:
        return immediate < 0 ? hpu::pmac(d, a, b) : hpu::pmac_imm(d, a, immediate);
    default: throw std::logic_error("invalid operator arithmetic");
    }
}

std::string transform(bool inverse, Operand data, Operand table,
                      int stage, int mode, int flag)
{
    hpu::Instruction instruction;
    instruction.mnemonic = inverse ? hpu::Mnemonic::kPintt : hpu::Mnemonic::kPntt;
    instruction.idx0 = stage;
    instruction.mode = static_cast<std::uint8_t>(mode);
    instruction.flag = static_cast<std::uint8_t>(flag);
    emit(instruction, {data, table}, {data}, 0);
    if (compatibility_slot(data) < 0 || compatibility_slot(table) < 0) return {};
    return inverse ? hpu::pintt(compatibility_slot(data), compatibility_slot(table), stage, mode, flag)
                   : hpu::pntt(compatibility_slot(data), compatibility_slot(table), stage, mode, flag);
}
} // namespace

Operand make_operand(std::string label, int slot)
{
    static std::atomic<std::size_t> next{1};
    if (slot < -1 || slot > 63) throw std::invalid_argument("invalid compatibility slot");
    return {next.fetch_add(1, std::memory_order_relaxed), std::move(label), slot};
}

void OperatorProgram::append(const OperatorProgram& source, std::size_t begin, std::size_t end)
{
    end = std::min(end, source.operations.size());
    if (begin > end) throw std::invalid_argument("invalid operator fragment range");
    std::map<ValueId, ValueId> remapped;
    auto input = [&](ValueId old) {
        const auto known = remapped.find(old);
        if (known != remapped.end()) return known->second;
        const auto& value = source.values.at(old);
        const auto live = current_values.find(value.variable.symbol);
        if (live != current_values.end()) return remapped.emplace(old, live->second).first->second;
        const auto id = values.size();
        values.push_back({value.variable, true});
        current_values[value.variable.symbol] = id;
        remapped[old] = id;
        return id;
    };
    for (std::size_t i = begin; i < end; ++i) {
        auto op = source.operations[i];
        for (auto& value : op.inputs) value = input(value);
        if (op.reuse) op.reuse = input(*op.reuse);
        for (auto& value : op.outputs) {
            const auto old = value;
            value = values.size();
            values.push_back({source.values.at(old).variable, false});
            remapped[old] = value;
            current_values[values.back().variable.symbol] = value;
        }
        if (op.port) {
            auto port = source.ports.at(*op.port);
            port.operation_index = operations.size();
            if (port.binding) port.binding->instruction_index = port.operation_index;
            op.port = ports.size();
            ports.push_back(std::move(port));
        }
        operations.push_back(std::move(op));
    }
}

void OperatorProgram::bind_port(PortId id, hpu::compiler::ResolvedDma binding)
{
    auto& port = ports.at(id);
    if (port.logical_binding.empty())
        port.logical_binding = std::string(port.load ? "read:" : "write:") +
            binding.allocation_id + "@" + std::to_string(binding.span.line_offset) +
            ":" + std::to_string(binding.span.line_count);
    binding.instruction_index = port.operation_index;
    port.binding = std::move(binding);
}

void OperatorProgram::bind_ports(const std::vector<hpu::compiler::ResolvedDma>& bindings)
{
    if (bindings.size() != ports.size()) throw std::invalid_argument("operator data ports are incomplete");
    std::map<std::size_t, PortId> by_instruction;
    for (PortId id = 0; id < ports.size(); ++id)
        if (!by_instruction.emplace(ports[id].operation_index, id).second)
            throw std::invalid_argument("operator data port is duplicated");
    std::vector<bool> seen(ports.size());
    for (const auto& binding : bindings) {
        const auto found = by_instruction.find(binding.instruction_index);
        if (found == by_instruction.end() || seen[found->second])
            throw std::invalid_argument("operator binding targets a missing or duplicated port");
        seen[found->second] = true;
        bind_port(found->second, binding);
    }
}

void OperatorProgram::bind_named_ports(
    const std::map<std::string, hpu::compiler::ResolvedDma>& bindings)
{
    for (PortId id = 0; id < ports.size(); ++id) {
        const auto& name = ports[id].logical_binding;
        if (name.empty()) continue;
        const auto found = bindings.find(name);
        if (found == bindings.end()) {
            if (ports[id].binding) continue;
            throw std::invalid_argument("missing named operator data binding: " + name);
        }
        bind_port(id, found->second);
    }
}

Recording::Recording() : previous_(current) { current = this; }
Recording::~Recording() { current = previous_; }
OperatorProgram Recording::finish() { return std::move(program_); }
void Recording::append(const OperatorProgram& program)
{
    const auto begin = program_.operations.size();
    program_.append(program);
    for (std::size_t i = begin; i < program_.operations.size(); ++i) {
        auto& operation = program_.operations[i];
        if (operation.instruction.mnemonic == hpu::Mnemonic::kPmodld)
            modulus_context_ = operation.instruction.mod_id;
        else if (!operation.modulus_context) operation.modulus_context = modulus_context_;
    }
}
std::optional<ValueId> Recording::existing(const Operand& variable) const
{
    const auto found = program_.current_values.find(variable.symbol);
    return found == program_.current_values.end() ? std::nullopt : std::optional<ValueId>(found->second);
}
ValueId Recording::value(const Operand& variable)
{
    const auto found = program_.current_values.find(variable.symbol);
    if (found != program_.current_values.end()) return found->second;
    const auto id = program_.values.size();
    program_.values.push_back({variable, true});
    program_.current_values[variable.symbol] = id;
    return id;
}
ValueId Recording::define(const Operand& variable)
{
    const auto id = program_.values.size();
    program_.values.push_back({variable, false});
    program_.current_values[variable.symbol] = id;
    return id;
}
void Recording::emit(Operation op)
{
    if (op.instruction.mnemonic == hpu::Mnemonic::kPmodld)
        modulus_context_ = op.instruction.mod_id;
    else if (!op.modulus_context) op.modulus_context = modulus_context_;
    if (op.instruction.mnemonic == hpu::Mnemonic::kDload || op.instruction.mnemonic == hpu::Mnemonic::kDstore) {
        if (op.port) {
            if (*op.port >= program_.ports.size()) throw std::invalid_argument("unknown data port");
        } else {
            op.port = program_.ports.size();
            program_.ports.push_back({program_.operations.size(), op.instruction.mnemonic == hpu::Mnemonic::kDload, {}});
        }
    }
    program_.operations.push_back(std::move(op));
}

void Recording::name_last_port(std::string logical_binding)
{
    if (program_.operations.empty() || !program_.operations.back().port)
        throw std::logic_error("last operator operation has no data port");
    program_.ports.at(*program_.operations.back().port).logical_binding = std::move(logical_binding);
}
void Recording::bind_last_port(hpu::compiler::ResolvedDma binding)
{
    if (program_.operations.empty() || !program_.operations.back().port)
        throw std::logic_error("last operator operation has no data port");
    program_.bind_port(*program_.operations.back().port, std::move(binding));
}

Operand compatibility_operand(int slot)
{
    switch (slot) {
    case 0: return value;
    case 1: return auxiliary;
    case 2: return accumulator;
    case 3: return transform_table;
    case 4: return modulus_table;
    case 5: return correction;
    case 6: return temporary;
    case 7: return extra;
    default:
        if (slot < 8 || slot > 63)
            throw std::invalid_argument("invalid compatibility object slot");
        static const auto high = [] {
            std::array<Operand, 56> result;
            for (std::size_t i = 0; i < result.size(); ++i)
                result[i] = make_operand("p" + std::to_string(i + 8), static_cast<int>(i + 8));
            return result;
        }();
        return high[static_cast<std::size_t>(slot - 8)];
    }
}

std::string compatibility_name(Operand operand) { return hpu::pobj(compatibility_slot(operand)); }

std::string read(Operand object, hpu::DataType type, hpu::DloadFlag flag, hpu::CachePolicy cp)
{
    hpu::Instruction op;
    op.mnemonic = hpu::Mnemonic::kDload;
    op.rs1 = 10; op.rs2 = 11;
    // DataType describes the logical payload; the v1.2 wire flag is MOD only.
    op.type = static_cast<std::uint8_t>(hpu::dma_mod_bit(type));
    op.dma_flag = static_cast<std::uint8_t>(hpu::dma_bank_mask(type, flag));
    op.cp = static_cast<std::uint8_t>(cp);
    emit(op, {}, {object});
    return object.compatibility_slot < 0 ? std::string{} : hpu::dload(object.compatibility_slot, type, flag, cp);
}
std::string read(Operand object, hpu::DataType type, const std::string& logical_binding,
                 hpu::DloadFlag flag, hpu::CachePolicy cp)
{
    auto result = read(object, type, flag, cp);
    if (current) current->name_last_port(logical_binding);
    return result;
}
std::string input(Operand object, hpu::compiler::ResolvedDma binding,
                  hpu::DataType type, hpu::DloadFlag flag, hpu::CachePolicy cp)
{
    auto result = read(object, type, flag, cp);
    if (current) current->bind_last_port(std::move(binding));
    return result;
}
std::string output(Operand object, hpu::CachePolicy cp)
{
    return publish(object, 0, cp);
}
std::string output(Operand object, const std::string& logical_binding, hpu::CachePolicy cp)
{
    auto result = output(object, cp);
    if (current) current->name_last_port(logical_binding);
    return result;
}
std::string output(Operand object, hpu::compiler::ResolvedDma binding, hpu::CachePolicy cp)
{
    auto result = output(object, cp);
    if (current) current->bind_last_port(std::move(binding));
    return result;
}
std::string publish(Operand object, const std::string& logical_binding)
{
    return output(object, logical_binding);
}
std::string publish(Operand object, int release, hpu::CachePolicy cp)
{
    hpu::Instruction op;
    op.mnemonic = hpu::Mnemonic::kDstore;
    op.rs1 = 10; op.rs2 = 11; op.type = static_cast<std::uint8_t>(release);
    op.cp = static_cast<std::uint8_t>(cp);
    emit(op, {object}, {});
    return object.compatibility_slot < 0 ? std::string{} : hpu::dstore(object.compatibility_slot, release, cp);
}
std::string discard(Operand object)
{
    hpu::Instruction op; op.mnemonic = hpu::Mnemonic::kPfree;
    emit(op, {object}, {});
    return object.compatibility_slot < 0 ? std::string{} : hpu::pfree(object.compatibility_slot);
}
std::string add(Operand d, Operand a, Operand b) { return arithmetic(hpu::Mnemonic::kPadd, d, a, b); }
std::string subtract(Operand d, Operand a, Operand b) { return arithmetic(hpu::Mnemonic::kPsub, d, a, b); }
std::string multiply(Operand d, Operand a, Operand b) { return arithmetic(hpu::Mnemonic::kPmul, d, a, b); }
std::string accumulate(Operand d, Operand a, Operand b) { return arithmetic(hpu::Mnemonic::kPmac, d, a, b); }
std::string multiply_immediate(Operand d, Operand a, int i) { return arithmetic(hpu::Mnemonic::kPmul, d, a, a, i); }
std::string accumulate_immediate(Operand d, Operand a, int i) { return arithmetic(hpu::Mnemonic::kPmac, d, a, a, i); }
std::string ntt_stage(Operand d, Operand t, int s, int m, int f) { return transform(false, d, t, s, m, f); }
std::string intt_stage(Operand d, Operand t, int s, int m, int f) { return transform(true, d, t, s, m, f); }
std::string select_modulus(int context)
{
    hpu::Instruction op; op.mnemonic = hpu::Mnemonic::kPmodld; op.mod_id = context;
    emit(op, {}, {});
    return hpu::pmodld(context);
}
std::string finish_program()
{
    hpu::Instruction op; op.mnemonic = hpu::Mnemonic::kPsync;
    emit(op, {}, {});
    return hpu::psync();
}
std::string render_operator_ir(const OperatorProgram& program)
{
    std::ostringstream out;
    out << "operator_program values=" << program.values.size() << " ports=" << program.ports.size()
        << " operations=" << program.operations.size() << '\n';
    for (ValueId i = 0; i < program.values.size(); ++i) {
        const auto& value = program.values[i];
        out << '%' << i << " name=" << std::quoted(value.variable.label);
        if (value.parameter) out << " parameter";
        out << '\n';
    }
    for (std::size_t i = 0; i < program.operations.size(); ++i) {
        const auto& op = program.operations[i];
        out << "i" << i << ": " << hpu::to_string(op.instruction.mnemonic) << " uses=[";
        for (auto id : op.inputs) out << '%' << id << ' ';
        out << "] defs=[";
        for (auto id : op.outputs) out << '%' << id << ' ';
        out << ']';
        if (op.modulus_context) out << " modulus_context=" << *op.modulus_context;
        if (op.tied_input) out << " tied_input=" << *op.tied_input;
        if (op.reuse) out << " reuse=%" << *op.reuse;
        const auto& inst = op.instruction;
        switch (inst.mnemonic) {
        case hpu::Mnemonic::kPntt: case hpu::Mnemonic::kPintt:
            out << " stage=" << inst.idx0 << " mode=" << unsigned(inst.mode) << " flag=" << unsigned(inst.flag); break;
        case hpu::Mnemonic::kPmodld: out << " mod_id=" << inst.mod_id; break;
        case hpu::Mnemonic::kDload:
            out << " mod=" << unsigned(inst.type) << " cp=" << unsigned(inst.cp)
                << " bank_mask=" << unsigned(inst.dma_flag); break;
        case hpu::Mnemonic::kDstore:
            out << " release=" << unsigned(inst.type) << " cp=" << unsigned(inst.cp); break;
        default: if (inst.imm8 >= 0) out << " imm=" << inst.imm8; break;
        }
        if (op.port) {
            out << " port=" << *op.port;
            const auto& port = program.ports.at(*op.port);
            if (!port.logical_binding.empty()) out << " role=" << std::quoted(port.logical_binding);
            if (port.binding) out << " buffer=" << std::quoted(port.binding->allocation_id)
                << " line_offset=" << port.binding->span.line_offset << " lines=" << port.binding->span.line_count;
        }
        out << '\n';
    }
    return out.str();
}

} // namespace hpu::resource
