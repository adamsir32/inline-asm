#include "util/ntt.hpp"
#include "hpu/compiler/operator_resources.hpp"
#include "util/validation.hpp"

#include <cmath>
#include <sstream>
#include <string>

std::string generate_hpu_ntt_body_asm(
    int N,
    hpu::resource::Operand obj_poly,
    hpu::resource::Operand twiddle_obj,
    bool append_psync)
{
    return generate_hpu_ntt_body_asm(N, obj_poly, twiddle_obj, append_psync,
        "forward/v" + std::to_string(obj_poly.symbol));
}

std::string generate_hpu_ntt_body_asm(
    int N,
    hpu::resource::Operand obj_poly,
    hpu::resource::Operand twiddle_obj,
    bool append_psync,
    const std::string& table_port_prefix)
{
    std::ostringstream asm_code;

    if (!hpu::is_valid_ntt_size(N)) {
        asm_code << "        // Invalid config: require power-of-two 128 <= N <= 65536\n";
        return asm_code.str();
    }

    const int logN = static_cast<int>(std::log2(static_cast<double>(N)));

    auto data_obj = obj_poly;

    asm_code << "        // modulus table is loaded by the enclosing complete program\n";
    // 假设调用方已经加载过密文模数
    // Negacyclic NTT = pointwise pre-twist followed by a cyclic NTT.
    asm_code << "\n        // Negacyclic pre-twist: explicit PMUL by psi^i\n";
    asm_code << hpu::resource::read(
        twiddle_obj, hpu::DataType::poly,
        table_port_prefix + "/pre_twist",
        hpu::DloadFlag::regular_bank, hpu::CachePolicy::allocate);
    asm_code << hpu::resource::multiply(data_obj, data_obj, twiddle_obj);

    // obj_poly 是稳定的逻辑对象；控制器为每个 stage 执行物理 out-of-place，
    // 完成后提交新 base。stage 配对由 stream_ctrl 地址与 lane 调度完成。
    for (int stage = 0; stage < logN; ++stage) {
        asm_code << "\n        // ==========================================\n";
        asm_code << "        // Stage " << stage << " (Stage-level pntt)\n";
        asm_code << "        // ==========================================\n";
        asm_code << hpu::resource::read(
            twiddle_obj, hpu::DataType::twiddle_parameters,
            table_port_prefix + "/stage" + std::to_string(stage),
            hpu::DloadFlag::small_bank, hpu::CachePolicy::allocate);
        asm_code << hpu::resource::ntt_stage(data_obj, twiddle_obj, stage, 0);
    }

    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }

    asm_code << "\n        // Final result object slot: " << hpu::resource::compatibility_name(data_obj) << "\n";
    return asm_code.str();
}

std::string generate_hpu_intt_body_asm(
    int N,
    hpu::resource::Operand obj_poly,
    hpu::resource::Operand twiddle_obj,
    bool append_psync)
{
    return generate_hpu_intt_body_asm(N, obj_poly, twiddle_obj, append_psync,
        "inverse/v" + std::to_string(obj_poly.symbol));
}

std::string generate_hpu_intt_body_asm(
    int N,
    hpu::resource::Operand obj_poly,
    hpu::resource::Operand twiddle_obj,
    bool append_psync,
    const std::string& table_port_prefix)
{
    std::ostringstream asm_code;

    if (!hpu::is_valid_ntt_size(N)) {
        asm_code << "        // Invalid config: require power-of-two 128 <= N <= 65536\n";
        return asm_code.str();
    }

    const int logN = static_cast<int>(std::log2(static_cast<double>(N)));

    auto data_obj = obj_poly;

    asm_code << "        // modulus table is loaded by the enclosing complete program\n";
    // 假设调用方已经加载过密文模数

    // obj_poly 是稳定的逻辑对象；控制器为每个 stage 执行物理 out-of-place，
    // 完成后提交新 base。stage 配对由 stream_ctrl 地址与 lane 调度完成。
    for (int stage = 0; stage < logN; ++stage) {
        asm_code << "\n        // ==========================================\n";
        asm_code << "        // Stage " << stage << " (Stage-level pintt)\n";
        asm_code << "        // ==========================================\n";
        asm_code << hpu::resource::read(
            twiddle_obj, hpu::DataType::twiddle_parameters,
            table_port_prefix + "/stage" + std::to_string(stage),
            hpu::DloadFlag::small_bank, hpu::CachePolicy::allocate);
        asm_code << hpu::resource::intt_stage(data_obj, twiddle_obj, stage, 0);
    }

    // The PE butterfly does not implicitly normalize or apply the inverse twist.
    asm_code << "\n        // INTT normalize and inverse-twist: explicit PMUL by N^-1 * psi^-i\n";
    asm_code << hpu::resource::read(
        twiddle_obj, hpu::DataType::poly,
        table_port_prefix + "/post_untwist_scale",
        hpu::DloadFlag::regular_bank, hpu::CachePolicy::allocate);
    asm_code << hpu::resource::multiply(data_obj, data_obj, twiddle_obj);

    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }

    asm_code << "\n        // Final result object slot: " << hpu::resource::compatibility_name(data_obj) << "\n";
    return asm_code.str();
}

std::string generate_hpu_ntt_body_asm(int N, int obj_poly, int twiddle_obj, bool append_psync)
{
    return generate_hpu_ntt_body_asm(N,
        hpu::resource::compatibility_operand(obj_poly),
        hpu::resource::compatibility_operand(twiddle_obj), append_psync);
}

std::string generate_hpu_intt_body_asm(int N, int obj_poly, int twiddle_obj, bool append_psync)
{
    return generate_hpu_intt_body_asm(N,
        hpu::resource::compatibility_operand(obj_poly),
        hpu::resource::compatibility_operand(twiddle_obj), append_psync);
}

std::string generate_hpu_ntt_asm(
    int N,
    int obj_poly,
    int twiddle_obj,
    int mod_ctx_obj,
    bool append_psync)
{
    std::ostringstream asm_code;

    asm_code << "void hpu_ntt_N" << N << "(void) {\n";

    if (!hpu::is_valid_ntt_size(N)) {
        asm_code << "    // Invalid config: require power-of-two 128 <= N <= 65536\n";
        asm_code << "}\n";
        return asm_code.str();
    }

    asm_code << "    __asm__ volatile(\n";
    asm_code << hpu::resource::read(hpu::resource::compatibility_operand(mod_ctx_obj), hpu::DataType::mod_ctx, "constants/modulus_table",
                           hpu::DloadFlag::small_bank);
    asm_code << hpu::resource::select_modulus(0);
    asm_code << hpu::resource::read(hpu::resource::compatibility_operand(obj_poly), hpu::DataType::poly, "input");
    asm_code << generate_hpu_ntt_body_asm(
        N,
        obj_poly,
        twiddle_obj,
        false);
    asm_code << hpu::resource::output(hpu::resource::compatibility_operand(obj_poly), "output");
    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }
    asm_code << "\n        // 结束\n";
    asm_code << "        : \n";
    asm_code << "        : \n";
    asm_code << "        : \"memory\"\n";
    asm_code << "    );\n";
    asm_code << "}\n";
    
    return asm_code.str();
}

std::string generate_hpu_intt_asm(
    int N,
    int obj_poly,
    int twiddle_obj,
    int mod_ctx_obj,
    bool append_psync)
{
    std::ostringstream asm_code;

    asm_code << "void hpu_intt_N" << N << "(void) {\n";

    if (!hpu::is_valid_ntt_size(N)) {
        asm_code << "    // Invalid config: require power-of-two 128 <= N <= 65536\n";
        asm_code << "}\n";
        return asm_code.str();
    }

    asm_code << "    __asm__ volatile(\n";
    asm_code << hpu::resource::read(hpu::resource::compatibility_operand(mod_ctx_obj), hpu::DataType::mod_ctx, "constants/modulus_table",
                           hpu::DloadFlag::small_bank);
    asm_code << hpu::resource::select_modulus(0);
    asm_code << hpu::resource::read(hpu::resource::compatibility_operand(obj_poly), hpu::DataType::poly, "input");
    asm_code << generate_hpu_intt_body_asm(
        N,
        obj_poly,
        twiddle_obj,
        false);
    asm_code << hpu::resource::output(hpu::resource::compatibility_operand(obj_poly), "output");
    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }
    asm_code << "\n        // 结束\n";
    asm_code << "        : \n";
    asm_code << "        : \n";
    asm_code << "        : \"memory\"\n";
    asm_code << "    );\n";
    asm_code << "}\n";
    
    return asm_code.str();
}
