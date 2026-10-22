#include "scheme/ckks/ciphertext_multiply.hpp"
#include "scheme/ckks/basic_arithmetic.hpp"

#include "assembler.hpp"
#include "util/hpu_asm.hpp"
#include "util/ntt.hpp"

#include <cstddef>
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

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void check_transform_cache_policy()
{
    // Exercise the complete legacy entry points as well as the transform
    // bodies used by every scheme. Only transform tables request LLC allocation.
    for (const auto& source : {generate_hpu_ntt_asm(8192, 0, 3, 4),
                               generate_hpu_intt_asm(8192, 0, 3, 4)}) {
        std::size_t table_loads = 0;
        std::size_t stage_loads = 0;
        for (const auto& encoded : hpu::assemble_source(source)) {
            const auto& instruction = encoded.instruction;
            if (instruction.mnemonic == hpu::Mnemonic::kDload) {
                const bool is_table = instruction.obj_id == 3;
                require(instruction.cp == (is_table ? 1 : 0),
                        "transform DMA did not apply twiddle-only LLC allocation");
                require(((encoded.word >> 13U) & 1U) == instruction.cp,
                        "transform CP hint was not encoded in bit 13");
                if (is_table) {
                    ++table_loads;
                    require(instruction.type == 0,
                            "twiddle load incorrectly selected the modulus table");
                    if (instruction.dma_flag == 0x20) ++stage_loads;
                    else require(instruction.dma_flag == 0,
                                 "pre/post table changed SRAM placement");
                }
            } else if (instruction.mnemonic == hpu::Mnemonic::kDstore) {
                require(instruction.cp == 0,
                        "transform result store requested LLC allocation");
            }
        }
        require(table_loads == 14 && stage_loads == 13,
                "forward/inverse transform did not cache both stage and pre/post tables");
    }
}

} // namespace

int main()
{
    try {
        check_transform_cache_policy();
        constexpr int degree = 128;
        constexpr int num_q = 4;
        constexpr int num_p = 1;
        constexpr int dnum = 4;
        constexpr std::size_t log_degree = 7;

        const std::string program =
            hpu::scheme::ckks::generate_ciphertext_multiply_body_asm(
                degree, num_q, num_p, dnum, true);

        require(
            count(program, hpu::dload(
                4, hpu::DataType::mod_ctx, hpu::DloadFlag::small_bank)) == 1,
            "application must load the complete modulus table exactly once");
        require(count(program, hpu::pfree(4)) == 1,
                "application must release the modulus table exactly once");
        require(count(program, hpu::psync()) == 1,
                "application must contain exactly one terminal psync");
        require(program.rfind(hpu::psync()) + hpu::psync().size() == program.size(),
                "psync must be the final instruction");
        const std::size_t last_store = program.rfind("\"dstore ");
        const std::size_t table_release = program.rfind(hpu::pfree(4));
        const std::size_t synchronization = program.rfind(hpu::psync());
        require(last_store != std::string::npos
                    && last_store < table_release
                    && table_release < synchronization,
                "final result must be dstore'd before table release and psync");
        require(program.find("no input NTT is emitted") != std::string::npos,
                "application does not declare the canonical NTT input contract");
        const std::size_t key_switch_ntt =
            program.find("Step 2: NTT on Q and P bases");
        require(key_switch_ntt != std::string::npos
                    && program.find("\"pntt ") >= key_switch_ntt,
                "a forward NTT was emitted before KeySwitch");

        // KeySwitch requires dnum transforms over Q union P. The only other
        // forward transforms are the two post-rescale outputs over Q\{q_last}.
        const std::size_t expected_pntt =
            (dnum * (num_q + num_p) + 2 * (num_q - 1)) * log_degree;
        // Three tensor components cross to coefficients, then the two Q/P
        // KeySwitch accumulators cross to coefficients before ModDown.
        const std::size_t expected_pintt =
            (3 * num_q + 2 * (num_q + num_p)) * log_degree;
        require(count(program, "\"pntt ") == expected_pntt,
                "application emitted a redundant or missing forward NTT");
        require(count(program, "\"pintt ") == expected_pintt,
                "application emitted a redundant or missing inverse NTT");

        // A larger application owns the small-bank table and final psync.
        // Nested bodies must be composable without reloading/releasing it.
        std::string polynomial_program = hpu::dload(
            4, hpu::DataType::mod_ctx, hpu::DloadFlag::small_bank);
        polynomial_program +=
            hpu::scheme::ckks::generate_ciphertext_multiply_body_asm(
                degree, num_q, num_p, dnum, false, false);
        polynomial_program += hpu::scheme::ckks::generate_add_plain_body_asm(
            num_q - 1, false, false);
        polynomial_program += hpu::pfree(4);
        polynomial_program += hpu::psync();
        require(
            count(polynomial_program, hpu::dload(
                4, hpu::DataType::mod_ctx,
                hpu::DloadFlag::small_bank)) == 1
                && count(polynomial_program, hpu::pfree(4)) == 1
                && count(polynomial_program, hpu::psync()) == 1,
            "composed polynomial program duplicated application-lifetime state");

        std::cout << "CKKS application codegen lifecycle/transform tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CKKS application codegen test failed: " << error.what() << '\n';
        return 1;
    }
}
