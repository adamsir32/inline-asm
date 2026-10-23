#pragma once

#include "hpu/compiler/resource_ir.hpp"
#include "util/hpu_asm.hpp"


namespace hpu::resource {

using ValueId = std::size_t;
using PortId = std::size_t;

// A source-level variable. Each assignment emits a fresh SSA ValueId. The
// optional slot hint belongs only to the historical assembly renderer.
struct Operand {
    std::size_t symbol = 0;
    std::string label;
    int compatibility_slot = -1;
};
Operand make_operand(std::string label, int compatibility_slot = -1);
inline bool operator==(const Operand& a, const Operand& b) { return a.symbol == b.symbol; }
inline bool operator!=(const Operand& a, const Operand& b) { return !(a == b); }

// Legacy aliases, used only by old entry points and their tests. Native kernels
// declare independent operands with make_operand(), including repeated labels.
inline const auto value = make_operand("value", 0);
inline const auto auxiliary = make_operand("auxiliary", 1);
inline const auto accumulator = make_operand("accumulator", 2);
inline const auto transform_table = make_operand("transform_table", 3);
inline const auto modulus_table = make_operand("modulus_table", 4);
inline const auto correction = make_operand("correction", 5);
inline const auto temporary = make_operand("temporary", 6);
inline const auto extra = make_operand("extra", 7);

struct Value {
    Operand variable;
    bool parameter = false;
};
struct DataPort {
    std::size_t operation_index = 0;
    bool load = true;
    std::optional<hpu::compiler::ResolvedDma> binding;
    // Stable logical role, independent of instruction order and object slots.
    std::string logical_binding;
    hpu::compiler::MemoryLayout layout = hpu::compiler::MemoryLayout::unspecified;
};
struct Operation {
    hpu::Instruction instruction;
    std::vector<ValueId> inputs;
    std::vector<ValueId> outputs;
    std::optional<ValueId> reuse;
    std::optional<std::size_t> tied_input;
    std::optional<PortId> port;
    // Selected arithmetic modulus belongs to the operation, not its position.
    std::optional<int> modulus_context;
};
struct OperatorProgram {
    std::vector<Value> values;
    std::vector<DataPort> ports;
    std::vector<Operation> operations;
    // Explicit fragment live-outs; used when connecting independently emitted
    // fragments. Symbols are unique identities, never physical slot numbers.
    std::map<std::size_t, ValueId> current_values;
    void append(const OperatorProgram& source, std::size_t begin = 0,
                std::size_t end = static_cast<std::size_t>(-1));
    void bind_port(PortId id, hpu::compiler::ResolvedDma binding);
    void bind_ports(const std::vector<hpu::compiler::ResolvedDma>& bindings);
    void bind_named_ports(
        const std::map<std::string, hpu::compiler::ResolvedDma>& bindings);
};

class Recording {
public:
    Recording();
    ~Recording();
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;
    OperatorProgram finish();
    void append(const OperatorProgram& program);
    // Explicit SSA access for immutable snapshots and native IR passes.
    ValueId value(const Operand& variable);
    std::optional<ValueId> existing(const Operand& variable) const;
    ValueId define(const Operand& variable);
    void emit(Operation operation);
    void name_last_port(std::string logical_binding);
    void bind_last_port(hpu::compiler::ResolvedDma binding);
private:
    Recording* previous_;
    OperatorProgram program_;
    std::optional<int> modulus_context_;
};

// External data ports and value lifetime hints. The compiler owns the actual
// DLOAD/DSTORE/PFREE and may retain a value, spill it, or reuse its DDR backing.
std::string read(Operand object, hpu::DataType type,
                 hpu::DloadFlag flag = hpu::DloadFlag::regular_bank,
                 hpu::CachePolicy cp = hpu::CachePolicy::no_allocate);
std::string read(Operand object, hpu::DataType type,
                 const std::string& logical_binding,
                 hpu::DloadFlag flag = hpu::DloadFlag::regular_bank,
                 hpu::CachePolicy cp = hpu::CachePolicy::no_allocate);
std::string input(Operand object, hpu::compiler::ResolvedDma binding,
                  hpu::DataType type,
                  hpu::DloadFlag flag = hpu::DloadFlag::regular_bank,
                  hpu::CachePolicy cp = hpu::CachePolicy::no_allocate);
// Logical writeback declaration. Release is always a backend decision.
std::string output(Operand object,
                   hpu::CachePolicy cp = hpu::CachePolicy::no_allocate);
std::string output(Operand object, const std::string& logical_binding,
                   hpu::CachePolicy cp = hpu::CachePolicy::no_allocate);
std::string output(Operand object, hpu::compiler::ResolvedDma binding,
                   hpu::CachePolicy cp = hpu::CachePolicy::no_allocate);
std::string publish(Operand object, const std::string& logical_binding);
std::string publish(Operand object, int release = 1,
                    hpu::CachePolicy cp = hpu::CachePolicy::no_allocate);
std::string discard(Operand object);
std::string add(Operand destination, Operand left, Operand right);
std::string subtract(Operand destination, Operand left, Operand right);
std::string multiply(Operand destination, Operand left, Operand right);
// PMUL mode 01: right is a read-only two-line geometric-factor template.
std::string multiply_online(Operand destination, Operand data, Operand parameters);
std::string accumulate(Operand destination, Operand left, Operand right);
std::string multiply_immediate(Operand destination, Operand left, int immediate);
std::string accumulate_immediate(Operand destination, Operand left, int immediate);
std::string ntt_stage(Operand data, Operand table, int stage, int mode = 0, int flag = 0);
std::string intt_stage(Operand data, Operand table, int stage, int mode = 0, int flag = 0);
std::string select_modulus(int context);
std::string finish_program();

// Used exclusively at historical APIs accepting physical object numbers.
Operand compatibility_operand(int slot);
std::string compatibility_name(Operand operand);

// Bind already resolved native ports to the image without an assembly baseline.
hpu::compiler::VirtualProgram bind_program(
    const OperatorProgram& program, const hpu::runtime::HpuMemImage& image);
std::string render_operator_ir(const OperatorProgram& program);

// Historical adapter: check the compatibility stream and bind its DMA recipe,
// then create storage objects from native SSA operations.
hpu::compiler::VirtualProgram bind_program(
    const OperatorProgram& program,
    const std::vector<hpu::EncodedInstruction>& baseline,
    const std::vector<hpu::compiler::ResolvedDma>& dma,
    const hpu::runtime::HpuMemImage& image);

} // namespace hpu::resource
