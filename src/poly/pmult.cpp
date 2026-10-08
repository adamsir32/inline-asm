#include "poly/pmult.hpp"

#include "hpu/compiler/operator_resources.hpp"
#include "util/mm.hpp"
#include <sstream>
#include <string>

std::string generate_hpu_pmult_body_asm(
    int num_q,
    bool append_psync,
    bool manage_modulus_table)
{
    std::ostringstream asm_code;

    if (num_q <= 0) {
        asm_code << "        // Invalid config: require num_q > 0\n";
        return asm_code.str();
    }

    asm_code << "        /* PMULT: (ct0, ct1) * pt over RNS basis Q */\n";

    const auto POBJ_MOD_CTX = hpu::resource::modulus_table;
    const auto POBJ_CT = hpu::resource::value;
    const auto POBJ_PT = hpu::resource::auxiliary;
    const auto POBJ_OUT = hpu::resource::accumulator;

    if (manage_modulus_table) {
        asm_code << hpu::resource::read(POBJ_MOD_CTX, hpu::DataType::mod_ctx,
                               hpu::DloadFlag::small_bank);
    }
    for (int i = 0; i < num_q; ++i) {
        asm_code << "        /* q_" << i << " */\n";
        asm_code << hpu::resource::select_modulus(i);

        asm_code << hpu::resource::read(POBJ_CT, hpu::DataType::poly);
        asm_code << hpu::resource::read(POBJ_PT, hpu::DataType::poly);
        asm_code << generate_hpu_mm_body_asm(POBJ_OUT, POBJ_CT, POBJ_PT);
        asm_code << hpu::resource::discard(POBJ_CT);
        asm_code << hpu::resource::publish(POBJ_OUT, 1);

        asm_code << hpu::resource::read(POBJ_CT, hpu::DataType::poly);
        asm_code << generate_hpu_mm_body_asm(POBJ_OUT, POBJ_CT, POBJ_PT);
        asm_code << hpu::resource::discard(POBJ_CT);
        asm_code << hpu::resource::discard(POBJ_PT);
        asm_code << hpu::resource::publish(POBJ_OUT, 1);
    }

    if (manage_modulus_table) {
        asm_code << hpu::resource::discard(POBJ_MOD_CTX);
    }

    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }

    return asm_code.str();
}

std::string generate_hpu_pmult_asm(
    int num_q,
    bool append_psync)
{
    std::ostringstream asm_code;
    asm_code << "void hpu_pmult_Q" << num_q << "(void) {\n";

    if (num_q <= 0) {
        asm_code << "    // Invalid config: require num_q > 0\n";
        asm_code << "}\n";
        return asm_code.str();
    }

    asm_code << "    __asm__ volatile(\n";
    asm_code << generate_hpu_pmult_body_asm(num_q, append_psync);

    asm_code << "        : \n";
    asm_code << "        : \n";
    asm_code << "        : \"memory\"\n";
    asm_code << "    );\n";
    asm_code << "}\n";

    return asm_code.str();
}
