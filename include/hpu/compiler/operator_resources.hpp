#pragma once

#include "hpu/compiler/resource_ir.hpp"
#include "util/hpu_asm.hpp"


namespace hpu::resource {

// Algorithm roles, not physical object IDs. The compatibility renderer alone
// maps these roles to the historical assembly convention.
enum class Role { value, auxiliary, accumulator, transform_table,
                  modulus_table, correction, temporary, extra };
struct Operand { Role role; };
inline constexpr Operand value{Role::value};
inline constexpr Operand auxiliary{Role::auxiliary};
inline constexpr Operand accumulator{Role::accumulator};
inline constexpr Operand transform_table{Role::transform_table};
inline constexpr Operand modulus_table{Role::modulus_table};
inline constexpr Operand correction{Role::correction};
inline constexpr Operand temporary{Role::temporary};
inline constexpr Operand extra{Role::extra};

inline bool operator==(Operand a, Operand b) { return a.role == b.role; }
inline bool operator!=(Operand a, Operand b) { return !(a == b); }

struct Operation {
    hpu::Instruction instruction;
    std::vector<Operand> inputs;
    std::vector<Operand> outputs;
};
struct OperatorProgram {
    std::vector<Operation> operations;
    void append(const OperatorProgram& source, std::size_t begin = 0,
                std::size_t end = static_cast<std::size_t>(-1));
};
void record(Operation operation);

// Scoped typed emission supports nested kernel calls and independent threads.
// No assembly parser or encoded physical operand is used to construct this IR.
class Recording {
public:
    Recording();
    ~Recording();
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;
    OperatorProgram finish();
private:
    Recording* previous_;
    OperatorProgram program_;
    friend void record(Operation operation);
};

// External data ports and value lifetime hints. The compiler owns the actual
// DLOAD/DSTORE/PFREE and may retain a value, spill it, or reuse its DDR backing.
std::string read(Operand object, hpu::DataType type,
                 hpu::DloadFlag flag = hpu::DloadFlag::regular_bank);
std::string publish(Operand object, int release = 1);
std::string discard(Operand object);
std::string add(Operand destination, Operand left, Operand right);
std::string subtract(Operand destination, Operand left, Operand right);
std::string multiply(Operand destination, Operand left, Operand right);
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

// Bind symbolic ports to the image. Validate the compatibility stream as a
// baseline, then create virtual objects directly from the operator operations.
hpu::compiler::VirtualProgram bind_program(
    const OperatorProgram& program,
    const std::vector<hpu::EncodedInstruction>& baseline,
    const std::vector<hpu::compiler::ResolvedDma>& dma,
    const hpu::runtime::HpuMemImage& image);

} // namespace hpu::resource
