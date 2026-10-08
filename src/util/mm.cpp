#include "util/mm.hpp"
#include "hpu/compiler/operator_resources.hpp"

#include <sstream>
#include <string>

std::string generate_hpu_mm_body_asm(
    hpu::resource::Operand obj_a,
    hpu::resource::Operand obj_b,
    hpu::resource::Operand obj_c,
    bool append_psync)
{
    std::ostringstream asm_code;
    asm_code << hpu::resource::multiply(obj_a, obj_b, obj_c);
    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }
    return asm_code.str();
}

std::string generate_hpu_mm_body_asm(int obj_a, int obj_b, int obj_c, bool append_psync)
{
    return generate_hpu_mm_body_asm(hpu::resource::compatibility_operand(obj_a),
        hpu::resource::compatibility_operand(obj_b),
        hpu::resource::compatibility_operand(obj_c), append_psync);
}

std::string generate_hpu_mm_asm(
    int obj_a,
    int obj_b,
    int obj_c,
    int mod_ctx_obj,
    bool append_psync)
{
    std::ostringstream asm_code;
    asm_code << "void hpu_mm_complete(void) {\n";

    asm_code << "    __asm__ volatile(\n";
    asm_code << hpu::resource::read(hpu::resource::compatibility_operand(mod_ctx_obj), hpu::DataType::mod_ctx,
                           hpu::DloadFlag::small_bank);
    asm_code << hpu::resource::select_modulus(0);
    asm_code << hpu::resource::read(hpu::resource::compatibility_operand(obj_b), hpu::DataType::poly);
    asm_code << hpu::resource::read(hpu::resource::compatibility_operand(obj_c), hpu::DataType::poly);
    asm_code << generate_hpu_mm_body_asm(obj_a, obj_b, obj_c);
    if (obj_b != obj_a) {
        asm_code << hpu::resource::discard(hpu::resource::compatibility_operand(obj_b));
    }
    if (obj_c != obj_a && obj_c != obj_b) {
        asm_code << hpu::resource::discard(hpu::resource::compatibility_operand(obj_c));
    }
    asm_code << hpu::resource::publish(hpu::resource::compatibility_operand(obj_a), 1);
    asm_code << hpu::resource::discard(hpu::resource::compatibility_operand(mod_ctx_obj));

    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }

    asm_code << "        : \n";
    asm_code << "        : \n";
    asm_code << "        : \"memory\"\n";
    asm_code << "    );\n";
    asm_code << "}\n";

    return asm_code.str();
}
