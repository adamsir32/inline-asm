#include "poly/cmult.hpp"

#include "hpu/compiler/operator_resources.hpp"
#include "util/mm.hpp"
#include <sstream>
#include <string>
// eval domain
std::string generate_hpu_cmult_body_asm(
    int num_q,
    bool append_psync,
    bool manage_modulus_table)
{
    std::ostringstream asm_code;

    if (num_q <= 0) {
        asm_code << "        // Invalid config: require num_q > 0\n";
        return asm_code.str();
    }

    asm_code << "        /* CMULT: (a0,a1)*(b0,b1)->(out0,out1,out2) over basis Q */\n";

    const auto POBJ_MOD_CTX = hpu::resource::make_operand("modulus_table", 4);
    const auto POBJ_A = hpu::resource::make_operand("value", 0);
    const auto POBJ_B = hpu::resource::make_operand("auxiliary", 1);
    const auto POBJ_OUT = hpu::resource::make_operand("accumulator", 2);

    if (manage_modulus_table) {
        asm_code << hpu::resource::read(POBJ_MOD_CTX, hpu::DataType::mod_ctx,
                               hpu::DloadFlag::small_bank);
    }
    for (int i = 0; i < num_q; ++i) {
        asm_code << "        /* q_" << i << " */\n";

        asm_code << hpu::resource::select_modulus(i);

        asm_code << "        /* --- CMULT out0 = a0 * b0 --- */\n";
        asm_code << hpu::resource::read(POBJ_A, hpu::DataType::poly); // a0
        asm_code << hpu::resource::read(POBJ_B, hpu::DataType::poly); // b0
        asm_code << generate_hpu_mm_body_asm(POBJ_OUT, POBJ_A, POBJ_B);
        asm_code << hpu::resource::discard(POBJ_A);
        asm_code << hpu::resource::discard(POBJ_B);
        asm_code << hpu::resource::publish(POBJ_OUT, 1); // store out0

        asm_code << "        /* --- CMULT out1 = a0 * b1 + a1 * b0 --- */\n";
        asm_code << hpu::resource::read(POBJ_A, hpu::DataType::poly); // a0
        asm_code << hpu::resource::read(POBJ_B, hpu::DataType::poly); // b1
        asm_code << generate_hpu_mm_body_asm(POBJ_OUT, POBJ_A, POBJ_B);
        asm_code << hpu::resource::discard(POBJ_A);
        asm_code << hpu::resource::discard(POBJ_B);
        // temp result is in POBJ_OUT
        // Now compute a1 * b0 and ADD
        asm_code << hpu::resource::read(POBJ_A, hpu::DataType::poly); // a1
        asm_code << hpu::resource::read(POBJ_B, hpu::DataType::poly); // b0
        asm_code << hpu::resource::accumulate(POBJ_OUT, POBJ_A, POBJ_B);
        asm_code << hpu::resource::discard(POBJ_A);
        asm_code << hpu::resource::discard(POBJ_B);
        asm_code << hpu::resource::publish(POBJ_OUT, 1); // store out1

        asm_code << "        /* --- CMULT out2 = a1 * b1 --- */\n";
        asm_code << hpu::resource::read(POBJ_A, hpu::DataType::poly); // a1
        asm_code << hpu::resource::read(POBJ_B, hpu::DataType::poly); // b1
        asm_code << generate_hpu_mm_body_asm(POBJ_OUT, POBJ_A, POBJ_B);
        asm_code << hpu::resource::discard(POBJ_A);
        asm_code << hpu::resource::discard(POBJ_B);
        asm_code << hpu::resource::publish(POBJ_OUT, 1); // store out2
    }

    if (manage_modulus_table) {
        asm_code << hpu::resource::discard(POBJ_MOD_CTX);
    }

    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }

    return asm_code.str();
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
