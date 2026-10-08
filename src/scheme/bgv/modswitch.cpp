#include "scheme/bgv/modswitch.hpp"

#include "util/bconv.hpp"
#include "hpu/compiler/operator_resources.hpp"
#include "util/ntt.hpp"
#include "util/validation.hpp"

#include <sstream>
#include <stdexcept>
#include <vector>

namespace hpu::scheme::bgv {
namespace {

bool valid_config(int num_q, int num_p, int num_components)
{
    return num_q >= 2 && num_components > 0
        && hpu::has_mod_context_capacity(num_q, num_p, 1);
}

bool valid_ntt_layout(int N, const NttModSwitchLayout& layout, int num_components)
{
    if (!hpu::is_valid_ntt_size(N) || num_components <= 0 ||
        layout.q_mod_ids.size() < 2 ||
        layout.q_mod_ids.size() >= static_cast<std::size_t>(hpu::kMaxModContexts) ||
        layout.plaintext_mod_id < 0 ||
        layout.plaintext_mod_id >= hpu::kMaxModContexts) {
        return false;
    }
    std::vector<bool> seen(hpu::kMaxModContexts);
    seen[static_cast<std::size_t>(layout.plaintext_mod_id)] = true;
    for (int context : layout.q_mod_ids) {
        if (context < 0 || context >= hpu::kMaxModContexts ||
            seen[static_cast<std::size_t>(context)]) {
            return false;
        }
        seen[static_cast<std::size_t>(context)] = true;
    }
    return true;
}

std::uint64_t inverse_mod(std::uint64_t value, std::uint64_t modulus)
{
    using I128 = __int128;
    I128 t = 0;
    I128 new_t = 1;
    I128 r = static_cast<I128>(modulus);
    I128 new_r = static_cast<I128>(value % modulus);
    while (new_r != 0) {
        const I128 quotient = r / new_r;
        const I128 old_t = t;
        t = new_t;
        new_t = old_t - quotient * new_t;
        const I128 old_r = r;
        r = new_r;
        new_r = old_r - quotient * new_r;
    }
    if (r != 1) {
        throw std::invalid_argument("BGV q_last is not invertible modulo t");
    }
    t %= static_cast<I128>(modulus);
    if (t < 0) {
        t += static_cast<I128>(modulus);
    }
    return static_cast<std::uint64_t>(t);
}

} // namespace

std::string generate_modswitch_body_asm(
    int num_q,
    int num_p,
    int num_components,
    bool append_psync)
{
    std::ostringstream asm_code;
    if (!valid_config(num_q, num_p, num_components)) {
        asm_code << "        // Invalid BGV ModSwitch config: require num_q >= 2, components > 0, and Q|P|t within 64 contexts\n";
        return asm_code.str();
    }

    const int dropped_context = num_q - 1;
    const int t_context = num_q + num_p;
    std::vector<int> retained_contexts;
    retained_contexts.reserve(static_cast<std::size_t>(num_q - 1));
    for (int i = 0; i < dropped_context; ++i) {
        retained_contexts.push_back(i);
    }

    const auto POBJ_VALUE = hpu::resource::value;
    const auto POBJ_CORRECTION = hpu::resource::auxiliary;
    const auto POBJ_U = hpu::resource::accumulator;
    const auto POBJ_SCALAR = hpu::resource::transform_table;
    const auto POBJ_MOD_CTX = hpu::resource::modulus_table;

    asm_code << "        /* BGV MODSWITCH: coefficient/Q -> coefficient/Q_without_last */\n";
    asm_code << "        /* MOD_ID layout: Q[0.." << dropped_context << "], P["
             << num_q << ".." << (num_q + num_p - 1) << "], t=" << t_context << ". */\n";
    asm_code << "        /* Formula: u=-c_last*q_last^-1 mod t; c_i'=(c_i-c_last-q_last*u)*q_last^-1 mod q_i. */\n";

    for (int component = 0; component < num_components; ++component) {
        asm_code << "        /* BGV MODSWITCH component " << component << " */\n";

        asm_code << "        /* stage-1: exact single-source BConv q_last -> t */\n";
        asm_code << ::generate_hpu_bconv_contexts_body_asm(
            {dropped_context}, {t_context}, false);

        asm_code << "        /* stage-2: u = -c_last * q_last^-1 mod t */\n";
        asm_code << hpu::resource::read(
            POBJ_MOD_CTX, hpu::DataType::mod_ctx, hpu::DloadFlag::small_bank);
        asm_code << hpu::resource::select_modulus(t_context);
        asm_code << "        // dload zero, BConv(c_last mod t), and q_last^-1 mod t\n";
        asm_code << hpu::resource::read(POBJ_VALUE, hpu::DataType::poly);
        asm_code << hpu::resource::read(POBJ_CORRECTION, hpu::DataType::poly);
        asm_code << hpu::resource::read(POBJ_SCALAR, hpu::DataType::poly);
        asm_code << hpu::resource::subtract(POBJ_VALUE, POBJ_VALUE, POBJ_CORRECTION);
        asm_code << hpu::resource::discard(POBJ_CORRECTION);
        asm_code << hpu::resource::multiply(POBJ_VALUE, POBJ_VALUE, POBJ_SCALAR);
        asm_code << hpu::resource::discard(POBJ_SCALAR);
        asm_code << hpu::resource::publish(POBJ_VALUE, 1);
        asm_code << hpu::resource::discard(POBJ_MOD_CTX);

        asm_code << "        /* stage-3a: exact single-source BConv c_last: q_last -> Q' */\n";
        asm_code << ::generate_hpu_bconv_contexts_body_asm(
            {dropped_context}, retained_contexts, false);
        asm_code << "        /* stage-3b: exact single-source BConv u: t -> Q' */\n";
        asm_code << ::generate_hpu_bconv_contexts_body_asm(
            {t_context}, retained_contexts, false);

        asm_code << "        /* stage-4: subtract correction and divide by q_last in Q' */\n";
        asm_code << hpu::resource::read(
            POBJ_MOD_CTX, hpu::DataType::mod_ctx, hpu::DloadFlag::small_bank);
        for (int i = 0; i < dropped_context; ++i) {
            asm_code << "        /* component " << component << ", q_" << i << " */\n";
            asm_code << hpu::resource::select_modulus(i);
            asm_code << "        // dload c_i, c_last mod q_i, u mod q_i, q_last mod q_i\n";
            asm_code << hpu::resource::read(POBJ_VALUE, hpu::DataType::poly);
            asm_code << hpu::resource::read(POBJ_CORRECTION, hpu::DataType::poly);
            asm_code << hpu::resource::read(POBJ_U, hpu::DataType::poly);
            asm_code << hpu::resource::read(POBJ_SCALAR, hpu::DataType::poly);
            asm_code << hpu::resource::multiply(POBJ_U, POBJ_U, POBJ_SCALAR);
            asm_code << hpu::resource::discard(POBJ_SCALAR);
            asm_code << hpu::resource::add(POBJ_CORRECTION, POBJ_CORRECTION, POBJ_U);
            asm_code << hpu::resource::discard(POBJ_U);
            asm_code << hpu::resource::subtract(POBJ_VALUE, POBJ_VALUE, POBJ_CORRECTION);
            asm_code << hpu::resource::discard(POBJ_CORRECTION);
            asm_code << "        // dload q_last^-1 mod q_i and write c_i'\n";
            asm_code << hpu::resource::read(POBJ_SCALAR, hpu::DataType::poly);
            asm_code << hpu::resource::multiply(POBJ_VALUE, POBJ_VALUE, POBJ_SCALAR);
            asm_code << hpu::resource::discard(POBJ_SCALAR);
            asm_code << hpu::resource::publish(POBJ_VALUE, 1);
        }
        asm_code << hpu::resource::discard(POBJ_MOD_CTX);
    }

    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }
    return asm_code.str();
}

std::string generate_modswitch_asm(
    int num_q,
    int num_p,
    int num_components,
    bool append_psync)
{
    std::ostringstream asm_code;
    asm_code << "void hpu_bgv_modswitch_Q" << num_q << "_P" << num_p
             << "_C" << num_components << "(void) {\n";
    if (!valid_config(num_q, num_p, num_components)) {
        asm_code << "    // Invalid BGV ModSwitch config\n}\n";
        return asm_code.str();
    }

    asm_code << "    __asm__ volatile(\n";
    asm_code << generate_modswitch_body_asm(
        num_q, num_p, num_components, append_psync);
    asm_code << "        : \n"
             << "        : \n"
             << "        : \"memory\"\n"
             << "    );\n"
             << "}\n";
    return asm_code.str();
}

std::uint64_t modswitch_correction_factor(
    std::uint64_t factor,
    std::uint64_t q_last,
    std::uint64_t plaintext_modulus)
{
    if (plaintext_modulus < 2) {
        throw std::invalid_argument("BGV plaintext modulus must be at least 2");
    }
    const std::uint64_t inverse = inverse_mod(q_last, plaintext_modulus);
    return static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(factor % plaintext_modulus) * inverse)
        % plaintext_modulus);
}

NttModSwitchConstants prepare_ntt_modswitch_constants(
    const std::vector<std::uint32_t>& q_moduli,
    std::uint32_t plaintext_modulus)
{
    if (q_moduli.size() < 2 || plaintext_modulus < 65537) {
        throw std::invalid_argument("BGV NTT ModSwitch requires at least two Q limbs and an HPU plaintext modulus");
    }
    const std::uint32_t q_last = q_moduli.back();
    if (q_last < 65537) {
        throw std::invalid_argument("BGV NTT ModSwitch q_last is outside the HPU modulus range");
    }
    NttModSwitchConstants result;
    const auto inverse_t = inverse_mod(q_last, plaintext_modulus);
    result.neg_q_last_inverse_mod_t = static_cast<std::uint32_t>(
        plaintext_modulus - inverse_t);
    result.q_last_mod_q.reserve(q_moduli.size() - 1);
    result.q_last_inverse_mod_q.reserve(q_moduli.size() - 1);
    for (std::size_t index = 0; index + 1 < q_moduli.size(); ++index) {
        const std::uint32_t q = q_moduli[index];
        if (q < 65537) {
            throw std::invalid_argument("BGV NTT ModSwitch Q modulus is outside the HPU range");
        }
        result.q_last_mod_q.push_back(q_last % q);
        result.q_last_inverse_mod_q.push_back(
            static_cast<std::uint32_t>(inverse_mod(q_last, q)));
    }
    return result;
}

std::string generate_modswitch_ntt_body_asm(
    int N,
    const NttModSwitchLayout& layout,
    int num_components,
    bool append_psync,
    bool manage_modulus_table)
{
    if (!valid_ntt_layout(N, layout, num_components)) {
        return "        /* Invalid SEAL-facing BGV NTT ModSwitch layout */\n";
    }

    constexpr auto value = hpu::resource::value;
    constexpr auto auxiliary = hpu::resource::auxiliary;
    constexpr auto twiddle = hpu::resource::transform_table;
    constexpr auto modulus_table = hpu::resource::modulus_table;
    const int dropped_context = layout.q_mod_ids.back();
    std::ostringstream asm_code;
    asm_code << "        /* BGV MODSWITCH NTT: drop q_last; retain canonical HPU NTT output */\n";
    asm_code << "        /* DMA constants: -q_last^-1 mod t, q_last mod q_i, q_last^-1 mod q_i. */\n";
    if (manage_modulus_table) {
        asm_code << hpu::resource::read(modulus_table, hpu::DataType::mod_ctx,
                               hpu::DloadFlag::small_bank);
    }

    for (int component = 0; component < num_components; ++component) {
        asm_code << "        /* component " << component << ": only dropped NTT limb -> coefficients */\n";
        asm_code << hpu::resource::select_modulus(dropped_context);
        asm_code << "        // dload c_last in canonical HPU NTT layout\n";
        asm_code << hpu::resource::read(value, hpu::DataType::poly);
        asm_code << generate_hpu_intt_body_asm(N, value, twiddle, false);
        asm_code << "        // dstore c_last coefficient scratch (bit-reversed physical order)\n";
        asm_code << hpu::resource::publish(value, 1);

        asm_code << "        /* u_t = -c_last * q_last^-1 mod t; wide PMUL input is legal */\n";
        asm_code << hpu::resource::select_modulus(layout.plaintext_mod_id);
        asm_code << "        // dload c_last coefficient scratch, then prepared -q_last^-1 mod t\n";
        asm_code << hpu::resource::read(value, hpu::DataType::poly);
        asm_code << hpu::resource::read(auxiliary, hpu::DataType::poly);
        asm_code << hpu::resource::multiply(value, value, auxiliary);
        asm_code << hpu::resource::discard(auxiliary);
        asm_code << "        // dstore canonical u_t coefficient scratch\n";
        asm_code << hpu::resource::publish(value, 1);

        for (std::size_t index = 0; index + 1 < layout.q_mod_ids.size(); ++index) {
            const int q_context = layout.q_mod_ids[index];
            asm_code << "        /* component " << component << ", retained q MOD_ID "
                     << q_context << ": coefficient correction -> NTT */\n";
            asm_code << hpu::resource::select_modulus(q_context);
            asm_code << "        // dload u_t and prepared q_last mod q_i; PMUL reduces u_t under q_i\n";
            asm_code << hpu::resource::read(value, hpu::DataType::poly);
            asm_code << hpu::resource::read(auxiliary, hpu::DataType::poly);
            asm_code << hpu::resource::multiply(value, value, auxiliary);
            asm_code << hpu::resource::discard(auxiliary);
            asm_code << "        // dload c_last coefficients and canonicalize under q_i before PADD\n";
            asm_code << hpu::resource::read(auxiliary, hpu::DataType::poly);
            asm_code << hpu::resource::multiply_immediate(auxiliary, auxiliary, 1);
            asm_code << hpu::resource::add(value, value, auxiliary);
            asm_code << hpu::resource::discard(auxiliary);
            asm_code << generate_hpu_ntt_body_asm(N, value, twiddle, false);
            asm_code << "        // dload original retained c_i in canonical HPU NTT layout\n";
            asm_code << hpu::resource::read(auxiliary, hpu::DataType::poly);
            asm_code << hpu::resource::subtract(auxiliary, auxiliary, value);
            asm_code << hpu::resource::discard(value);
            asm_code << "        // dload prepared q_last^-1 mod q_i and store output NTT limb\n";
            asm_code << hpu::resource::read(value, hpu::DataType::poly);
            asm_code << hpu::resource::multiply(auxiliary, auxiliary, value);
            asm_code << hpu::resource::discard(value);
            asm_code << hpu::resource::publish(auxiliary, 1);
        }
    }

    if (manage_modulus_table) {
        asm_code << hpu::resource::discard(modulus_table);
    }
    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }
    return asm_code.str();
}

} // namespace hpu::scheme::bgv
