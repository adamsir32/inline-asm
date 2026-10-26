#pragma once

#include "operator/rns_layout.hpp"

#include <string>

// Complete two-component key switch:
// KeySwitch(base, switching_component, evaluation_key) -> (base + ks0, ks1).
std::string generate_hpu_keyswitch_body_asm(
    int N,
    const hpu::RnsDecompositionLayout& layout,
    bool append_psync = false,
    bool manage_modulus_table = true,
    const std::string& port_prefix = "keyswitch");

// SEAL singleton-digit KeySwitch with rounded single-P ModDown. The core
// accepts coefficient-domain inputs and returns coefficient-domain outputs;
// CKKS wraps it with its input/output NTT transforms. Half-P residues must be
// bound for every Q/P context, and the accumulator objects must be mutable.
std::string generate_hpu_rounded_single_p_keyswitch_body_asm(
    int N,
    const hpu::RnsDecompositionLayout& layout,
    bool append_psync = false,
    bool manage_modulus_table = true,
    const std::string& port_prefix = "keyswitch");

// SEAL BFV KeySwitch specialization. Inputs and outputs are in coefficient
// form. Evaluation-key products accumulate in NTT form, cross back through
// INTT, and use rounded single-P ModDown before the base component is added.
std::string generate_hpu_bfv_keyswitch_body_asm(
    int N,
    const hpu::RnsDecompositionLayout& layout,
    bool append_psync = false,
    bool manage_modulus_table = true,
    const std::string& port_prefix = "keyswitch");

std::string generate_hpu_keyswitch_body_asm(
    int N,
    int num_q,
    int num_p,
    int dnum,
    bool append_psync = false,
    bool manage_modulus_table = true,
    const std::string& port_prefix = "keyswitch");

std::string generate_hpu_keyswitch_asm(
    int N,
    int num_q,
    int num_p,
    int dnum,
    bool append_psync = true);
