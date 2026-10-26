#include "poly/moddown.hpp"

#include "operator/rns_layout.hpp"
#include "util/bconv.hpp"
#include "hpu/compiler/operator_resources.hpp"
#include "util/validation.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool valid_moddown_config(int num_q, int num_p)
{
    return num_p > 0 && hpu::has_mod_context_capacity(num_q, num_p);
}

} // namespace

std::string generate_hpu_moddown_contexts_body_asm(
    const std::vector<int>& q_contexts,
    const std::vector<int>& p_contexts,
    bool append_psync,
    bool manage_modulus_table,
    const std::string& port_prefix)
{
    std::ostringstream asm_code;
    if (!hpu::valid_mod_id_list(q_contexts)
        || !hpu::valid_mod_id_list(p_contexts)) {
        asm_code << "        // Invalid explicit ModDown context layout\n";
        return asm_code.str();
    }
    for (int p : p_contexts) {
        if (std::find(q_contexts.begin(), q_contexts.end(), p)
            != q_contexts.end()) {
            asm_code << "        // Invalid overlapping Q/P ModDown contexts\n";
            return asm_code.str();
        }
    }

    const auto POBJ_MOD_CTX = hpu::resource::make_operand("modulus_table", 4);
    const auto POBJ_Q = hpu::resource::make_operand("value", 0);
    const auto POBJ_CORR = hpu::resource::make_operand("auxiliary", 1);
    const auto POBJ_P_INV = hpu::resource::make_operand("accumulator", 2);

    asm_code << "        /* MODDOWN stage-1: BConv P -> Q (fixed P -> active Q) */\n";
    asm_code << generate_hpu_bconv_contexts_body_asm(
        p_contexts, q_contexts, false, manage_modulus_table,
        port_prefix + "/bconv");
    if (manage_modulus_table) {
        asm_code << "        // dload the runtime-relocated complete modulus table\n";
        asm_code << hpu::resource::read(
            POBJ_MOD_CTX, hpu::DataType::mod_ctx,
            port_prefix + "/modulus_table",
            hpu::DloadFlag::small_bank);
    }
    asm_code << "        /* MODDOWN stage-2: q <- q - correction (mod q_i) */\n";
    for (int q : q_contexts) {
        asm_code << "        /* q MOD_ID " << q << " */\n";
        asm_code << hpu::resource::select_modulus(q);
        const auto context = std::to_string(q);
        asm_code << hpu::resource::read(POBJ_Q, hpu::DataType::poly,
                                      port_prefix + "/input/mod" + context);
        asm_code << hpu::resource::read(POBJ_CORR, hpu::DataType::poly,
                                      port_prefix + "/correction/mod" + context);
        asm_code << hpu::resource::read(POBJ_P_INV, hpu::DataType::poly,
                                      port_prefix + "/inverse/mod" + context);
        asm_code << hpu::resource::subtract(POBJ_Q, POBJ_Q, POBJ_CORR);
        asm_code << hpu::resource::multiply(POBJ_Q, POBJ_Q, POBJ_P_INV);
        asm_code << hpu::resource::output(POBJ_Q,
                                        port_prefix + "/output/mod" + context);
    }
    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }
    return asm_code.str();
}
// Coefficient domain.
std::string generate_hpu_moddown_body_asm(
    int num_q,
    int num_p,
    bool append_psync,
    bool manage_modulus_table,
    const std::string& port_prefix)
{
    std::ostringstream asm_code;

    if (!valid_moddown_config(num_q, num_p)) {
        asm_code << "        // Invalid config: require positive bases within the 64-entry software MOD_ID ABI\n";
        return asm_code.str();
    }

    std::vector<int> q_contexts;
    std::vector<int> p_contexts;
    for (int i = 0; i < num_q; ++i) {
        q_contexts.push_back(i);
    }
    for (int i = 0; i < num_p; ++i) {
        p_contexts.push_back(num_q + i);
    }
    return generate_hpu_moddown_contexts_body_asm(
        q_contexts, p_contexts, append_psync, manage_modulus_table, port_prefix);
}

std::string generate_hpu_moddown_asm(
    int num_q,
    int num_p,
    bool append_psync)
{
    std::ostringstream asm_code;
    asm_code << "void hpu_moddown_Q" << num_q << "_P" << num_p << "(void) {\n";

    if (!valid_moddown_config(num_q, num_p)) {
        asm_code << "    // Invalid config: require positive bases within the 64-entry software MOD_ID ABI\n";
        asm_code << "}\n";
        return asm_code.str();
    }

    asm_code << "    __asm__ volatile(\n";
    asm_code << generate_hpu_moddown_body_asm(
        num_q,
        num_p,
        append_psync);

    asm_code << "        : \n";
    asm_code << "        : \n";
    asm_code << "        : \"memory\"\n";
    asm_code << "    );\n";
    asm_code << "}\n";

    return asm_code.str();
}
