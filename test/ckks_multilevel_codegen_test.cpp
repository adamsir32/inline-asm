#include "operator/rns_layout.hpp"
#include "scheme/ckks/ciphertext_multiply.hpp"
#include "scheme/ckks/relinearize.hpp"
#include "scheme/ckks/rotate.hpp"
#include "util/hpu_asm.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::size_t count(const std::string& text, const std::string& token)
{
    std::size_t result = 0;
    std::size_t position = 0;
    while ((position = text.find(token, position)) != std::string::npos) {
        ++result;
        position += token.size();
    }
    return result;
}

hpu::RnsDecompositionLayout level_layout(int active_q)
{
    hpu::RnsDecompositionLayout layout;
    for (int q = 0; q < active_q; ++q) {
        layout.q_mod_ids.push_back(q);
        layout.key_digits.push_back({q});
    }
    layout.p_mod_ids = {4};
    return layout;
}

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_global_p(const std::string& program, int active_q)
{
    require(program.find(hpu::pmodld(4)) != std::string::npos,
            "lower-level KeySwitch did not use the fixed P MOD_ID");
    for (int dropped = active_q; dropped < 4; ++dropped) {
        require(program.find(hpu::pmodld(dropped)) == std::string::npos,
                "lower-level KeySwitch selected a dropped Q MOD_ID");
    }
    require(program.find("Invalid") == std::string::npos,
            "valid lower-level CKKS layout was rejected");
    require(count(program, hpu::psync()) == 1,
            "complete lower-level kernel needs one terminal psync");
}

void require_rounded_keyswitch(const std::string& program, int active_q)
{
    const auto key_start = program.find("KEYSWITCH BODY:");
    const auto key_end = program.find("Step 6: Add base component", key_start);
    require(key_start != std::string::npos && key_end != std::string::npos,
            "CKKS kernel omitted KeySwitch");
    const auto keyswitch = program.substr(key_start, key_end - key_start);
    for (int component = 0; component < 2; ++component) {
        const auto start = keyswitch.find("component " + std::to_string(component)
            + " stage-1: add floor(P/2) in Q union P");
        require(start != std::string::npos, "CKKS KeySwitch omitted half-P rounding");
        const auto end = keyswitch.find("MODDOWN stage-1: BConv P -> Q", start);
        require(end != std::string::npos, "rounded KeySwitch omitted P-to-Q ModDown");
        const auto rounding = keyswitch.substr(start, end - start);
        require(count(rounding, hpu::padd(0, 0, 1))
                    == static_cast<std::size_t>(active_q + 1),
                "CKKS KeySwitch must round every Q/P limb of both components");
        for (int context = 0; context <= 4; ++context) {
            require(count(rounding, hpu::pmodld(context))
                        == static_cast<std::size_t>(context < active_q || context == 4),
                    "CKKS rounding lost fixed P or selected a dropped Q");
        }
    }
}

} // namespace

int main()
{
    try {
        constexpr int degree = 128;
        constexpr std::size_t log_degree = 7;
        constexpr std::uint32_t galois_element = 3;

        const auto q4 = level_layout(4);
        const auto q3 = level_layout(3);
        const auto q2 = level_layout(2);
        require(hpu::is_valid_rns_decomposition_layout(degree, q4)
                    && hpu::is_valid_rns_decomposition_layout(degree, q3)
                    && hpu::is_valid_rns_decomposition_layout(degree, q2),
                "SEAL-style singleton digit layouts are invalid");

        const auto multiply_q4 =
            hpu::scheme::ckks::generate_ciphertext_multiply_body_asm(
                degree, q4, true);
        require_global_p(multiply_q4, 4);

        const auto rotate_q3 = hpu::scheme::ckks::generate_rotate_body_asm(
            degree, q3, galois_element, true);
        require_global_p(rotate_q3, 3);
        require(count(rotate_q3, "\"pntt ") == (3 * 4 + 2 * 3) * log_degree,
                "Q3 Rotate forward-transform count is wrong");
        require(count(rotate_q3, "\"pintt ") == (2 * 3 + 2 * 4) * log_degree,
                "Q3 Rotate inverse-transform count is wrong");

        const auto relinearize_q3 =
            hpu::scheme::ckks::generate_relinearize_ntt_body_asm(
                degree, q3, true);
        require_global_p(relinearize_q3, 3);
        require(count(relinearize_q3, "\"pntt ")
                    == (3 * 4 + 2 * 3) * log_degree,
                "Q3 Relinearize forward-transform count is wrong");
        require(count(relinearize_q3, "\"pintt ")
                    == (3 * 3 + 2 * 4) * log_degree,
                "Q3 Relinearize inverse-transform count is wrong");

        const auto multiply_q3 =
            hpu::scheme::ckks::generate_ciphertext_multiply_body_asm(
                degree, q3, true);
        require_global_p(multiply_q3, 3);
        require(count(multiply_q3, "\"pntt ")
                    == (3 * 4 + 2 * 2) * log_degree,
                "Q3 Multiply forward-transform count is wrong");
        require(count(multiply_q3, "\"pintt ")
                    == (3 * 3 + 2 * 4) * log_degree,
                "Q3 Multiply inverse-transform count is wrong");

        const auto rotate_q2 = hpu::scheme::ckks::generate_rotate_body_asm(
            degree, q2, galois_element, true);
        require_global_p(rotate_q2, 2);

        // N=4096 matches the AM parameter-regression fixtures. Check raw
        // Rotate, Conjugate, Relinearize and fused Multiply at active Q levels.
        for (int test_degree : {128, 4096}) {
            for (int active_q : {4, 3, 2, 1}) {
                const auto layout = level_layout(active_q);
                require_rounded_keyswitch(
                    hpu::scheme::ckks::generate_rotate_steps_body_asm(
                        test_degree, layout, 1, true), active_q);
                require_rounded_keyswitch(
                    hpu::scheme::ckks::generate_conjugate_body_asm(
                        test_degree, layout, true), active_q);
                require_rounded_keyswitch(
                    hpu::scheme::ckks::generate_relinearize_ntt_body_asm(
                        test_degree, layout, true), active_q);
                if (active_q >= 2) {
                    require_rounded_keyswitch(
                        hpu::scheme::ckks::generate_ciphertext_multiply_body_asm(
                            test_degree, layout, true), active_q);
                }
            }
        }

        const auto generic_layout = hpu::make_contiguous_rns_decomposition_layout(4, 2, 2);
        const auto generic_multiply = hpu::scheme::ckks::generate_ciphertext_multiply_body_asm(
            degree, generic_layout, true);
        require(generic_multiply.find("Invalid") == std::string::npos
                    && generic_multiply.find("ROUNDED SINGLE-P KEYSWITCH") == std::string::npos
                    && generic_multiply.find("Step 5: ModDown for both parts") != std::string::npos,
                "generic multi-P Multiply lost its existing KeySwitch contract");

        std::cout << "CKKS Q4->Q3->Q2 stable-MOD_ID codegen tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CKKS multilevel codegen test failed: " << error.what() << '\n';
        return 1;
    }
}
