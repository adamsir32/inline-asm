#include "scheme/bfv/basic_arithmetic.hpp"

#include "hpu/compiler/operator_resources.hpp"
#include "util/ntt.hpp"
#include "util/validation.hpp"

#include <sstream>

namespace hpu::scheme::bfv {
namespace {

const auto kTwiddleObject = hpu::resource::make_operand("transform_table", 3);
const auto kLeftObject = hpu::resource::make_operand("value", 0);
const auto kRightObject = hpu::resource::make_operand("auxiliary", 1);
const auto kOutputObject = hpu::resource::make_operand("accumulator", 2);
const auto kModulusTableObject = hpu::resource::make_operand("modulus_table", 4);

bool valid_config(int num_q)
{
    return num_q > 0 && hpu::has_mod_context_capacity(num_q);
}

std::string generate_ciphertext_binary_body(int num_q, bool subtract, bool append_psync,
                                            bool manage_modulus_table)
{
    std::ostringstream asm_code;
    if (!valid_config(num_q)) {
        asm_code << "        /* Invalid BFV ciphertext binary config */\n";
        return asm_code.str();
    }
    asm_code << "        /* BFV " << (subtract ? "SUB" : "ADD")
             << ": coefficient-domain Q ciphertext operation */\n";
    if (manage_modulus_table) {
        asm_code << hpu::resource::read(kModulusTableObject, hpu::DataType::mod_ctx,
                               "constants/modulus_table", hpu::DloadFlag::small_bank);
    }
    for (int component = 0; component < 2; ++component) {
        for (int basis = 0; basis < num_q; ++basis) {
            const auto prefix = "pointwise/mod" + std::to_string(basis) + "/";
            const auto limb = "c" + std::to_string(component);
            asm_code << "        /* component_" << component << ", q_" << basis << " */\n";
            asm_code << hpu::resource::select_modulus(basis);
            asm_code << hpu::resource::read(kLeftObject, hpu::DataType::poly, prefix + "left/" + limb);
            asm_code << hpu::resource::read(kRightObject, hpu::DataType::poly, prefix + "right/" + limb);
            asm_code << (subtract ? hpu::resource::subtract(kOutputObject, kLeftObject, kRightObject)
                                  : hpu::resource::add(kOutputObject, kLeftObject, kRightObject));
            asm_code << hpu::resource::output(kOutputObject, prefix + "output/" + limb);
        }
    }
    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }
    return asm_code.str();
}

std::string generate_plain_binary_body(int num_q, bool subtract, bool append_psync,
                                       bool manage_modulus_table)
{
    std::ostringstream asm_code;
    if (!valid_config(num_q)) {
        asm_code << "        /* Invalid BFV plaintext binary config */\n";
        return asm_code.str();
    }
    asm_code << "        /* BFV " << (subtract ? "SUB_PLAIN" : "ADD_PLAIN")
             << ": update coefficient-domain c0 with prepared Delta*m; copy c1 */\n";
    if (manage_modulus_table) {
        asm_code << hpu::resource::read(kModulusTableObject, hpu::DataType::mod_ctx,
                               "constants/modulus_table", hpu::DloadFlag::small_bank);
    }
    for (int basis = 0; basis < num_q; ++basis) {
        const auto prefix = "pointwise/mod" + std::to_string(basis) + "/";
        asm_code << "        /* q_" << basis << ": out0=c0 op prepared_plaintext */\n";
        asm_code << hpu::resource::select_modulus(basis);
        asm_code << hpu::resource::read(kLeftObject, hpu::DataType::poly, prefix + "left/c0");
        asm_code << hpu::resource::read(kRightObject, hpu::DataType::poly, prefix + "plaintext");
        asm_code << (subtract ? hpu::resource::subtract(kOutputObject, kLeftObject, kRightObject)
                              : hpu::resource::add(kOutputObject, kLeftObject, kRightObject));
        asm_code << hpu::resource::output(kOutputObject, prefix + "output/c0");

        asm_code << "        /* q_" << basis << ": out1=c1 */\n";
        asm_code << hpu::resource::read(kLeftObject, hpu::DataType::poly, prefix + "left/c1");
        asm_code << hpu::resource::output(kLeftObject, prefix + "output/c1");
    }
    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }
    return asm_code.str();
}

} // namespace

std::string generate_add_body_asm(int num_q, bool append_psync, bool manage_modulus_table)
{
    return generate_ciphertext_binary_body(num_q, false, append_psync, manage_modulus_table);
}

std::string generate_subtract_body_asm(int num_q, bool append_psync, bool manage_modulus_table)
{
    return generate_ciphertext_binary_body(num_q, true, append_psync, manage_modulus_table);
}

std::string generate_add_plain_body_asm(int num_q, bool append_psync, bool manage_modulus_table)
{
    return generate_plain_binary_body(num_q, false, append_psync, manage_modulus_table);
}

std::string generate_subtract_plain_body_asm(int num_q, bool append_psync,
                                             bool manage_modulus_table)
{
    return generate_plain_binary_body(num_q, true, append_psync, manage_modulus_table);
}

std::string generate_multiply_plain_body_asm(int N, int num_q, bool append_psync,
                                             bool manage_modulus_table)
{
    std::ostringstream asm_code;
    if (!valid_config(num_q) || !hpu::is_valid_ntt_size(N)) {
        asm_code << "        /* Invalid BFV MultiplyPlain config */\n";
        return asm_code.str();
    }
    asm_code << "        /* BFV MULTIPLY_PLAIN: coefficient ciphertext -> canonical HPU "
                "NTT -> pointwise multiply -> coefficient output */\n";
    if (manage_modulus_table) {
        asm_code << hpu::resource::read(kModulusTableObject, hpu::DataType::mod_ctx,
                               "constants/modulus_table", hpu::DloadFlag::small_bank);
    }
    for (int component = 0; component < 2; ++component) {
        for (int basis = 0; basis < num_q; ++basis) {
            const auto prefix = "pointwise/mod" + std::to_string(basis) + "/";
            const auto limb = "c" + std::to_string(component);
            asm_code << "        /* component_" << component << ", q_" << basis << " */\n";
            asm_code << hpu::resource::select_modulus(basis);
            asm_code << hpu::resource::read(kLeftObject, hpu::DataType::poly, prefix + "left/" + limb);
            asm_code << ::generate_hpu_ntt_body_asm(N, kLeftObject, kTwiddleObject, false,
                prefix + "forward/" + limb + "/table");
            asm_code << hpu::resource::read(kRightObject, hpu::DataType::poly, prefix + "plaintext/" + limb);
            asm_code << hpu::resource::multiply(kLeftObject, kLeftObject, kRightObject);
            asm_code << ::generate_hpu_intt_body_asm(N, kLeftObject, kTwiddleObject, false,
                prefix + "inverse/" + limb + "/table");
            asm_code << hpu::resource::output(kLeftObject, prefix + "output/" + limb);
        }
    }
    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }
    return asm_code.str();
}

std::string generate_negate_body_asm(int num_q, bool append_psync, bool manage_modulus_table)
{
    std::ostringstream asm_code;
    if (!valid_config(num_q)) {
        asm_code << "        /* Invalid BFV Negate config */\n";
        return asm_code.str();
    }
    asm_code << "        /* BFV NEGATE: coefficient-domain out=(c-c)-c */\n";
    if (manage_modulus_table) {
        asm_code << hpu::resource::read(kModulusTableObject, hpu::DataType::mod_ctx,
                               "constants/modulus_table", hpu::DloadFlag::small_bank);
    }
    for (int component = 0; component < 2; ++component) {
        for (int basis = 0; basis < num_q; ++basis) {
            const auto prefix = "pointwise/mod" + std::to_string(basis) + "/";
            const auto limb = "c" + std::to_string(component);
            asm_code << "        /* component_" << component << ", q_" << basis << " */\n";
            asm_code << hpu::resource::select_modulus(basis);
            asm_code << hpu::resource::read(kLeftObject, hpu::DataType::poly, prefix + "left/" + limb);
            asm_code << hpu::resource::subtract(kOutputObject, kLeftObject, kLeftObject);
            asm_code << hpu::resource::subtract(kOutputObject, kOutputObject, kLeftObject);
            asm_code << hpu::resource::output(kOutputObject, prefix + "output/" + limb);
        }
    }
    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }
    return asm_code.str();
}

} // namespace hpu::scheme::bfv
