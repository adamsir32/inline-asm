#pragma once

#include <string>
#include "hpu/compiler/operator_resources.hpp"

std::string generate_hpu_mm_body_asm(
    int obj_a,
    int obj_b,
    int obj_c,
    bool append_psync = false);

std::string generate_hpu_mm_asm(
    int obj_a,
    int obj_b,
    int obj_c,
    int mod_ctx_obj,
    bool append_psync = true);

std::string generate_hpu_mm_body_asm(hpu::resource::Operand destination,
    hpu::resource::Operand left, hpu::resource::Operand right,
    bool append_psync = false);
