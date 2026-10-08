#pragma once

#include <string>
#include "hpu/compiler/operator_resources.hpp"

std::string generate_hpu_ntt_body_asm(
        int N,
        int obj_poly,
        int twiddle_obj,
        bool append_psync = false);

std::string generate_hpu_intt_body_asm(
        int N,
        int obj_poly,
        int twiddle_obj,
        bool append_psync = false);

std::string generate_hpu_ntt_asm(
        int N,
        int obj_poly,
        int twiddle_obj,
        int mod_ctx_obj,
        bool append_psync = true);

std::string generate_hpu_intt_asm(
        int N,
        int obj_poly,
        int twiddle_obj,
        int mod_ctx_obj,
        bool append_psync = true);

// Managed operator entry points use symbolic operands.
std::string generate_hpu_ntt_body_asm(int N, hpu::resource::Operand data,
    hpu::resource::Operand table, bool append_psync = false);
std::string generate_hpu_intt_body_asm(int N, hpu::resource::Operand data,
    hpu::resource::Operand table, bool append_psync = false);
