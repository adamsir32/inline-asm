#pragma once

#include <string>

std::string generate_hpu_ciphertext_multiply_body_asm(
    int N,
    int num_q,
    int num_p,
    int dnum,
    bool append_psync = false,
    const std::string& port_prefix = "multiply");

std::string generate_hpu_ciphertext_multiply_asm(
    int N,
    int num_q,
    int num_p,
    int dnum,
    bool append_psync = true);
