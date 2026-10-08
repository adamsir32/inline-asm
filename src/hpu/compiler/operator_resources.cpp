#include "hpu/compiler/operator_resources.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace hpu::resource {
namespace {
thread_local Recording* current = nullptr;

// Frozen compatibility ABI. The managed IR never stores these object IDs.
int compatibility_slot(Operand operand)
{
    switch (operand.role) {
    case Role::value: return 0;
    case Role::auxiliary: return 1;
    case Role::accumulator: return 2;
    case Role::transform_table: return 3;
    case Role::modulus_table: return 4;
    case Role::correction: return 5;
    case Role::temporary: return 6;
    case Role::extra: return 7;
    }
    throw std::invalid_argument("unknown operator operand role");
}

std::string arithmetic(hpu::Mnemonic mnemonic, Operand destination,
                       Operand left, Operand right, int immediate = -1)
{
    Operation op;
    op.instruction.mnemonic = mnemonic;
    op.instruction.imm8 = immediate;
    op.inputs = {left};
    if (immediate < 0) op.inputs.push_back(right);
    if (mnemonic == hpu::Mnemonic::kPmac) op.inputs.push_back(destination);
    op.outputs = {destination};
    record(std::move(op));
    const int d = compatibility_slot(destination), a = compatibility_slot(left);
    const int b = compatibility_slot(right);
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
    Operation op;
    op.instruction.mnemonic = inverse ? hpu::Mnemonic::kPintt : hpu::Mnemonic::kPntt;
    op.instruction.idx0 = stage;
    op.instruction.mode = static_cast<std::uint8_t>(mode);
    op.instruction.flag = static_cast<std::uint8_t>(flag);
    op.inputs = {data, table};
    op.outputs = {data};
    record(std::move(op));
    return inverse ? hpu::pintt(compatibility_slot(data), compatibility_slot(table), stage, mode, flag)
                   : hpu::pntt(compatibility_slot(data), compatibility_slot(table), stage, mode, flag);
}
} // namespace

void record(Operation operation)
{
    if (current) current->program_.operations.push_back(std::move(operation));
}

void OperatorProgram::append(const OperatorProgram& source, std::size_t begin, std::size_t end)
{
    end = std::min(end, source.operations.size());
    if (begin > end) throw std::invalid_argument("invalid operator fragment range");
    operations.insert(operations.end(), source.operations.begin() + begin,
                      source.operations.begin() + end);
}

Recording::Recording() : previous_(current) { current = this; }
Recording::~Recording() { current = previous_; }
OperatorProgram Recording::finish() { return std::move(program_); }

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
    default: throw std::invalid_argument("invalid compatibility object slot");
    }
}

std::string compatibility_name(Operand operand) { return hpu::pobj(compatibility_slot(operand)); }

std::string read(Operand object, hpu::DataType type, hpu::DloadFlag flag)
{
    Operation op;
    op.instruction.mnemonic = hpu::Mnemonic::kDload;
    op.instruction.rs1 = 10;
    op.instruction.rs2 = 11;
    op.instruction.type = static_cast<std::uint8_t>(type);
    op.instruction.dma_flag = static_cast<std::uint8_t>(flag);
    op.outputs = {object};
    record(std::move(op));
    return hpu::dload(compatibility_slot(object), type, flag);
}

std::string publish(Operand object, int release)
{
    Operation op;
    op.instruction.mnemonic = hpu::Mnemonic::kDstore;
    op.instruction.rs1 = 10;
    op.instruction.rs2 = 11;
    op.instruction.type = static_cast<std::uint8_t>(release);
    op.inputs = {object};
    record(std::move(op));
    return hpu::dstore(compatibility_slot(object), release);
}

std::string discard(Operand object)
{
    Operation op;
    op.instruction.mnemonic = hpu::Mnemonic::kPfree;
    op.inputs = {object};
    record(std::move(op));
    return hpu::pfree(compatibility_slot(object));
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
    Operation op;
    op.instruction.mnemonic = hpu::Mnemonic::kPmodld;
    op.instruction.mod_id = context;
    record(std::move(op));
    return hpu::pmodld(context);
}

std::string finish_program()
{
    Operation op;
    op.instruction.mnemonic = hpu::Mnemonic::kPsync;
    record(std::move(op));
    return hpu::psync();
}

} // namespace hpu::resource
