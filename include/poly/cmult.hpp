#pragma once

#include <string>
#include <vector>

// Common logical tensor builder for CKKS/BGV Q and BFV Q/Bsk bases.
// Named ports bind component identities independently of generated DMA slots.
std::string generate_hpu_cmult_contexts_body_asm(
    const std::vector<int>& contexts,
    const std::string& label,
    bool append_psync = false,
    bool manage_modulus_table = true);

std::string generate_hpu_cmult_body_asm(
    int num_q,
    bool append_psync = false,
    bool manage_modulus_table = true);

std::string generate_hpu_cmult_asm(
    int num_q,
    bool append_psync = true);

// std::string generate_hpu_cmult_ntt_asm(
//     int N,
//     int num_q,
//     bool append_psync = false);
