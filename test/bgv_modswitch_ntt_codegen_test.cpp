#include "assembler.hpp"
#include "executable.hpp"
#include "hpu/compiler/operator_resources.hpp"
#include "scheme/bgv/modswitch.hpp"
#include "util/hpu_asm.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

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

void check_layout(const std::vector<int>& q_contexts, int t_context)
{
    constexpr int degree = 128;
    constexpr int components = 2;
    constexpr std::size_t stages = 7;
    hpu::scheme::bgv::NttModSwitchLayout layout{q_contexts, t_context};
    const auto body = hpu::scheme::bgv::generate_modswitch_ntt_body_asm(
        degree, layout, components, true, true);
    require(body.find("Invalid") == std::string::npos,
            "valid BGV NTT ModSwitch layout was rejected");
    require(count(body, hpu::psync()) == 1 &&
                body.rfind(hpu::psync()) + hpu::psync().size() == body.size(),
            "complete BGV NTT ModSwitch needs one terminal psync");
    require(count(body, hpu::dload(
                4, hpu::DataType::mod_ctx,
                hpu::DloadFlag::small_bank)) == 1,
            "BGV NTT ModSwitch reloaded the application modulus table");
    const auto encoded = hpu::assemble_source(body);
    require(!encoded.empty(), "BGV NTT ModSwitch did not encode any instructions");
    hpu::validate_executable_program(encoded);
    for (const auto& instruction : encoded) {
        const std::uint32_t kind = (instruction.word & 0x7fU) == 0x2bU ? 1U : 0U;
        require((instruction.command26 >> 25U) == kind,
                "BGV NTT ModSwitch 32-bit instruction/precode kind mismatch");
    }
    require(count(body, hpu::pmodld(q_contexts.back())) == components &&
                count(body, hpu::pmodld(t_context)) == components,
            "dropped Q or fixed plaintext MOD_ID was not selected once per component");
    require(count(body, "\"pintt ") == components * stages,
            "BGV NTT ModSwitch must inverse-transform only q_last");
    require(count(body, "\"pntt ")
                == components * (q_contexts.size() - 1) * stages,
            "BGV NTT ModSwitch must forward-transform every correction limb");
    require(count(body, hpu::pmul_imm(1, 1, 1))
                == components * (q_contexts.size() - 1),
            "wide c_last was not canonicalized before PADD");

    const auto normalization = body.find(hpu::pmul_imm(1, 1, 1));
    const auto coefficient_add = body.find(hpu::padd(0, 0, 1));
    const auto correction_ntt = body.find("\"pntt ");
    const auto ntt_subtract = body.find(hpu::psub(1, 1, 0));
    require(normalization < coefficient_add &&
                coefficient_add < correction_ntt &&
                correction_ntt < ntt_subtract,
            "BGV correction was not normalized, transformed, and subtracted in order");

    const auto nested = hpu::scheme::bgv::generate_modswitch_ntt_body_asm(
        degree, layout, components, false, false);
    require(nested.find(hpu::dload(
                4, hpu::DataType::mod_ctx,
                hpu::DloadFlag::small_bank)) == std::string::npos &&
                nested.find(hpu::pfree(4)) == std::string::npos &&
                nested.find(hpu::psync()) == std::string::npos,
            "nested BGV NTT ModSwitch duplicated application-owned state");
}

void check_coefficient_port_names()
{
    hpu::resource::Recording recording;
    hpu::scheme::bgv::generate_modswitch_body_asm(3, 1, 2, true, "custom_drop");
    const auto program = recording.finish();
    require(!program.ports.empty(), "coefficient ModSwitch produced no logical data ports");
    bool conversion = false;
    bool output = false;
    for (const auto& port : program.ports) {
        require(port.logical_binding.find("custom_drop/c") == 0,
                "coefficient ModSwitch has an unnamed or unscoped data port");
        conversion = conversion || port.logical_binding.find("/last_to_t/") != std::string::npos;
        output = output || port.logical_binding == "custom_drop/c1/output/mod1";
    }
    require(conversion && output,
            "coefficient ModSwitch lost nested conversion or component output identities");
}

} // namespace

int main()
{
    try {
        const std::vector<std::uint32_t> q{
            2013265921U, 1811939329U, 469762049U};
        constexpr std::uint32_t t = 65537;
        const auto constants =
            hpu::scheme::bgv::prepare_ntt_modswitch_constants(q, t);
        require(constants.q_last_mod_q.size() == 2 &&
                    constants.q_last_inverse_mod_q.size() == 2,
                "BGV NTT ModSwitch constants have the wrong Q shape");
        require((static_cast<std::uint64_t>(q.back() % t) *
                     constants.neg_q_last_inverse_mod_t) % t == t - 1,
                "BGV negative inverse modulo t is wrong");
        for (std::size_t index = 0; index + 1 < q.size(); ++index) {
            require(constants.q_last_mod_q[index] == q.back() % q[index] &&
                        (static_cast<std::uint64_t>(constants.q_last_mod_q[index]) *
                         constants.q_last_inverse_mod_q[index]) % q[index] == 1,
                    "BGV Q correction constants are wrong");
        }
        check_layout({0, 1, 2}, 5);
        check_layout({0, 1}, 5);
        check_coefficient_port_names();
        const auto production = hpu::scheme::bgv::generate_modswitch_ntt_body_asm(
            65536, {{0, 1, 2}, 5}, 2);
        require(count(production, "\"pintt ") == 2 * 16 &&
                    count(production, "\"pntt ") == 2 * 2 * 16,
                "N=65536 BGV NTT ModSwitch transform shape is wrong");

        const auto invalid = hpu::scheme::bgv::generate_modswitch_ntt_body_asm(
            128, {{0, 1}, 1}, 2);
        require(invalid.find("Invalid SEAL-facing BGV") != std::string::npos,
                "overlapping Q/t MOD_ID was accepted");
        std::cout << "BGV NTT ModSwitch codegen tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BGV NTT ModSwitch codegen test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
