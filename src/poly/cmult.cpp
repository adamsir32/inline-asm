#include "poly/cmult.hpp"

#include "hpu/compiler/operator_resources.hpp"
#include "util/mm.hpp"
#include <sstream>
#include <string>
// eval domain
std::string generate_hpu_cmult_contexts_body_asm(
    const std::vector<int>& contexts,
    const std::string& label,
    bool append_psync,
    bool manage_modulus_table)
{
    std::ostringstream asm_code;
    if (contexts.empty()) {
        asm_code << "        // Invalid config: require at least one modulus context\n";
        return asm_code.str();
    }
    asm_code << "        /* " << label
             << ": (a0,a1)*(b0,b1)->(out0,out1,out2) */\n";

    // Independent source identities are essential: the scheduler owns their
    // physical residency and can keep each input until its final consumer.
    const auto modulus = hpu::resource::make_operand("modulus_table", 4);
    const auto a0 = hpu::resource::make_operand("left/c0", 0);
    const auto b0 = hpu::resource::make_operand("right/c0", 1);
    const auto a1 = hpu::resource::make_operand("left/c1", 5);
    const auto b1 = hpu::resource::make_operand("right/c1", 6);
    const auto out0 = hpu::resource::make_operand("tensor/c0", 2);
    const auto out1 = hpu::resource::make_operand("tensor/c1", 7);
    const auto out2 = hpu::resource::make_operand("tensor/c2", 8);
    if (manage_modulus_table) {
        asm_code << hpu::resource::read(modulus, hpu::DataType::mod_ctx, "constants/modulus_table",
                                      hpu::DloadFlag::small_bank);
    }
    for (int context : contexts) {
        const auto prefix = "tensor/mod" + std::to_string(context) + "/";
        asm_code << hpu::resource::select_modulus(context);
        asm_code << hpu::resource::read(a0, hpu::DataType::poly, prefix + "left/c0");
        asm_code << hpu::resource::read(b0, hpu::DataType::poly, prefix + "right/c0");
        asm_code << hpu::resource::read(a1, hpu::DataType::poly, prefix + "left/c1");
        asm_code << hpu::resource::read(b1, hpu::DataType::poly, prefix + "right/c1");
        asm_code << hpu::resource::multiply(out0, a0, b0);
        asm_code << hpu::resource::output(out0, prefix + "output/c0");
        asm_code << hpu::resource::multiply(out1, a0, b1);
        asm_code << hpu::resource::accumulate(out1, a1, b0);
        asm_code << hpu::resource::output(out1, prefix + "output/c1");
        asm_code << hpu::resource::multiply(out2, a1, b1);
        asm_code << hpu::resource::output(out2, prefix + "output/c2");
    }
    if (append_psync) asm_code << hpu::resource::finish_program();
    return asm_code.str();
}

std::string generate_hpu_cmult_body_asm(
    int num_q,
    bool append_psync,
    bool manage_modulus_table)
{
    if (num_q <= 0) return "        // Invalid config: require num_q > 0\n";
    std::vector<int> contexts;
    for (int i = 0; i < num_q; ++i) contexts.push_back(i);
    return generate_hpu_cmult_contexts_body_asm(
        contexts, "CMULT", append_psync, manage_modulus_table);
}

std::string generate_hpu_cmult_asm(
    int num_q,
    bool append_psync)
{
    std::ostringstream asm_code;
    asm_code << "void hpu_cmult_Q" << num_q << "(void) {\n";

    if (num_q <= 0) {
        asm_code << "    // Invalid config: require num_q > 0\n";
        asm_code << "}\n";
        return asm_code.str();
    }

    asm_code << "    __asm__ volatile(\n";
    asm_code << generate_hpu_cmult_body_asm(num_q, append_psync);

    asm_code << "        : \n";
    asm_code << "        : \n";
    asm_code << "        : \"memory\"\n";
    asm_code << "    );\n";
    asm_code << "}\n";

    return asm_code.str();
}
