#include "scheme/bgv/keyswitch.hpp"

#include "hpu/compiler/operator_resources.hpp"
#include "util/ntt.hpp"

#include <algorithm>
#include <sstream>
#include <vector>

namespace hpu::scheme::bgv {
namespace {

const auto kValue = hpu::resource::make_operand("value", 0);
const auto kAuxiliary = hpu::resource::make_operand("auxiliary", 1);
const auto kAccumulator = hpu::resource::make_operand("accumulator", 2);
const auto kTwiddle = hpu::resource::make_operand("transform_table", 3);
const auto kModulusTable = hpu::resource::make_operand("modulus_table", 4);

bool valid_config(int N, const hpu::RnsDecompositionLayout& layout, int t_id)
{
    return hpu::is_seal_single_p_rns_decomposition_layout(N, layout) &&
           t_id >= 0 && t_id < hpu::kMaxModContexts &&
           std::find(layout.q_mod_ids.begin(), layout.q_mod_ids.end(), t_id) ==
               layout.q_mod_ids.end() &&
           t_id != layout.p_mod_ids.front();
}

} // namespace

std::string generate_keyswitch_ntt_body_asm(
    int N, const hpu::RnsDecompositionLayout& layout, int plaintext_mod_id,
    bool append_psync, bool manage_modulus_table)
{
    if (!valid_config(N, layout, plaintext_mod_id)) {
        return "        /* Invalid SEAL-facing BGV NTT KeySwitch layout */\n";
    }
    const int p_id = layout.p_mod_ids.front();
    std::vector<int> target_ids = layout.q_mod_ids;
    target_ids.push_back(p_id);
    const auto limb = [](const std::string& prefix, int q) { return prefix + "/mod" + std::to_string(q); };
    const auto table = [&](int q, bool inverse) {
        return limb("constants/twiddle/canonical", q) + (inverse ? "/intt" : "/ntt");
    };
    const auto acc = [&](int c, int q) {
        return limb("workspace/bgv_keyswitch/c" + std::to_string(c) + "/acc", q);
    };
    std::ostringstream asm_code;
    asm_code << "        /* BGV KEYSWITCH NTT: singleton Q digits, mod-t special-P ModDown */\n";
    asm_code << "        /* Two canonical NTT base limbs plus one switching component -> two NTT outputs. */\n";
    if (manage_modulus_table) {
        asm_code << hpu::resource::read(kModulusTable, hpu::DataType::mod_ctx, "constants/modulus_table",
                               hpu::DloadFlag::small_bank);
    }

    for (std::size_t digit = 0; digit < layout.q_mod_ids.size(); ++digit) {
        const int source_id = layout.q_mod_ids[digit];
        asm_code << "        /* digit " << digit << ": switching limb NTT -> coefficient scratch */\n";
        asm_code << hpu::resource::select_modulus(source_id);
        asm_code << "        // dload switching component, q_j canonical HPU NTT\n";
        asm_code << hpu::resource::read(kValue, hpu::DataType::poly, limb("input/c2", source_id));
        asm_code << generate_hpu_intt_body_asm(N, kValue, kTwiddle, false, table(source_id, true));
        asm_code << "        // dstore switching q_j coefficient scratch\n";
        asm_code << hpu::resource::output(kValue, limb("workspace/bgv_keyswitch/switch_coeff", static_cast<int>(digit)));

        for (int target_id : target_ids) {
            asm_code << "        /* digit " << digit << ", target MOD_ID " << target_id << " */\n";
            asm_code << hpu::resource::select_modulus(target_id);
            if (target_id == source_id) {
                asm_code << "        // same modulus: dload original canonical NTT limb\n";
                asm_code << hpu::resource::read(kValue, hpu::DataType::poly, limb("input/c2", source_id));
            } else {
                asm_code << "        // cross-base dload q_j coefficients; canonicalize before PNTT\n";
                asm_code << hpu::resource::read(kValue, hpu::DataType::poly, limb("workspace/bgv_keyswitch/switch_coeff", static_cast<int>(digit)));
                asm_code << hpu::resource::multiply_immediate(kValue, kValue, 1);
                asm_code << generate_hpu_ntt_body_asm(N, kValue, kTwiddle, false, table(target_id, false));
            }
            for (int key_component = 0; key_component < 2; ++key_component) {
                asm_code << "        /* evaluation-key component " << key_component << " */\n";
                asm_code << "        // dload key digit at target MOD_ID\n";
                asm_code << hpu::resource::read(kAuxiliary, hpu::DataType::poly, limb("keys/digit" + std::to_string(digit) + "/c" + std::to_string(key_component), target_id));
                if (digit == 0) {
                    asm_code << hpu::resource::multiply(kAccumulator, kValue, kAuxiliary);
                } else {
                    asm_code << "        // dload prior canonical NTT accumulator\n";
                    asm_code << hpu::resource::read(kAccumulator, hpu::DataType::poly, acc(key_component, target_id));
                    asm_code << hpu::resource::accumulate(kAccumulator, kValue, kAuxiliary);
                }
                asm_code << "        // dstore canonical NTT accumulator\n";
                asm_code << hpu::resource::output(kAccumulator, acc(key_component, target_id));
            }
        }
    }

    for (int key_component = 0; key_component < 2; ++key_component) {
        asm_code << "        /* key component " << key_component
                 << ": special-P NTT accumulator -> coefficient correction */\n";
        asm_code << hpu::resource::select_modulus(p_id);
        asm_code << "        // dload special-P NTT accumulator\n";
        asm_code << hpu::resource::read(kValue, hpu::DataType::poly, acc(key_component, p_id));
        asm_code << generate_hpu_intt_body_asm(N, kValue, kTwiddle, false, table(p_id, true));
        asm_code << "        // dstore special-P coefficient scratch\n";
        asm_code << hpu::resource::output(kValue, "workspace/bgv_keyswitch/c" + std::to_string(key_component) + "/p_coeff");

        asm_code << "        /* u_t = -c_P * P^-1 mod t; wide PMUL input is legal */\n";
        asm_code << hpu::resource::select_modulus(plaintext_mod_id);
        asm_code << "        // dload c_P coefficients and prepared -P^-1 mod t\n";
        asm_code << hpu::resource::read(kValue, hpu::DataType::poly, "workspace/bgv_keyswitch/c" + std::to_string(key_component) + "/p_coeff");
        asm_code << hpu::resource::read(kAuxiliary, hpu::DataType::poly, "constants/bgv_keyswitch/neg_inv_t");
        asm_code << hpu::resource::multiply(kValue, kValue, kAuxiliary);
        asm_code << "        // dstore canonical u_t coefficient scratch\n";
        asm_code << hpu::resource::output(kValue, "workspace/bgv_keyswitch/c" + std::to_string(key_component) + "/u_t");

        for (int q_id : layout.q_mod_ids) {
            asm_code << "        /* BGV P ModDown, key component " << key_component
                     << ", q MOD_ID " << q_id << " */\n";
            asm_code << hpu::resource::select_modulus(q_id);
            asm_code << "        // dload u_t and prepared P mod q_i\n";
            asm_code << hpu::resource::read(kValue, hpu::DataType::poly, "workspace/bgv_keyswitch/c" + std::to_string(key_component) + "/u_t");
            asm_code << hpu::resource::read(kAuxiliary, hpu::DataType::poly, limb("constants/bgv_keyswitch/p_mod", q_id));
            asm_code << hpu::resource::multiply(kValue, kValue, kAuxiliary);
            asm_code << "        // dload c_P coefficients and canonicalize before PADD\n";
            asm_code << hpu::resource::read(kAuxiliary, hpu::DataType::poly, "workspace/bgv_keyswitch/c" + std::to_string(key_component) + "/p_coeff");
            asm_code << hpu::resource::multiply_immediate(kAuxiliary, kAuxiliary, 1);
            asm_code << hpu::resource::add(kValue, kValue, kAuxiliary);
            asm_code << generate_hpu_ntt_body_asm(N, kValue, kTwiddle, false, table(q_id, false));
            asm_code << "        // dload Q NTT accumulator and subtract correction NTT\n";
            asm_code << hpu::resource::read(kAuxiliary, hpu::DataType::poly, acc(key_component, q_id));
            asm_code << hpu::resource::subtract(kAuxiliary, kAuxiliary, kValue);
            asm_code << "        // dload prepared P^-1 mod q_i\n";
            asm_code << hpu::resource::read(kValue, hpu::DataType::poly, limb("constants/bgv_keyswitch/p_inv", q_id));
            asm_code << hpu::resource::multiply(kAuxiliary, kAuxiliary, kValue);
            asm_code << "        // dload base component in canonical HPU NTT and add\n";
            asm_code << hpu::resource::read(kValue, hpu::DataType::poly, limb("input/c" + std::to_string(key_component), q_id));
            asm_code << hpu::resource::add(kAuxiliary, kAuxiliary, kValue);
            asm_code << "        // dstore final canonical HPU NTT output\n";
            asm_code << hpu::resource::output(kAuxiliary, limb("output/c" + std::to_string(key_component), q_id));
        }
    }

    if (append_psync) {
        asm_code << hpu::resource::finish_program();
    }
    return asm_code.str();
}

} // namespace hpu::scheme::bgv
