#include "hpu/model/instruction_executor.hpp"
#include "hpu/seal/ckks_context.hpp"
#include "hpu/seal/operation_codegen.hpp"
#include "hpu/seal/operation_plan.hpp"
#include "hpu/seal/operation_relocation.hpp"
#include "hpu/seal/operation_runtime.hpp"
#include "assembler.hpp"
#include "operator/rounded_drop_last.hpp"
#include "poly/moddown.hpp"
#include "scheme/ckks/galois.hpp"
#include "util/hpu_asm.hpp"

#include <seal/seal.h>
#include <seal/util/ntt.h>

#include <algorithm>
#include <complex>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace hpu::seal_adapter;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

// Exercise exact rounding boundaries without probabilistic ciphertext data.
// Expected results come from integer division, independently of the RNS core.
void rounded_moddown_boundaries(
    const ::seal::SEALContext& context, const CkksLevelDescriptor& level)
{
    CkksApplicationImageBuilder builder(context, 256);
    const auto table = builder.add_modulus_table();
    const auto constants = builder.add_keyswitch_constants("constants/keyswitch", level);
    auto image = builder.image();
    const auto p = level.special_moduli.front();
    const auto p_id = level.rns_layout.p_mod_ids.front();
    const std::vector<std::uint64_t> residues{0, p / 2 - 1, p / 2, p / 2 + 1, p - 1};
    const auto degree = context.first_context_data()->parms().poly_modulus_degree();
    const auto fixture = [&](bool rounded) {
        auto memory = image;
        std::vector<hpu::runtime::HpuMemSpan> input;
        std::vector<int> full = level.rns_layout.q_mod_ids;
        full.push_back(p_id);
        for (std::size_t limb = 0; limb < full.size(); ++limb) {
            const auto modulus = limb < level.q_moduli.size() ? level.q_moduli[limb] : p;
            std::vector<std::uint32_t> words(degree);
            for (std::size_t index = 0; index < degree; ++index) {
                const auto quotient = 3 + index % 7;
                words[index] = static_cast<std::uint32_t>(
                    (quotient * static_cast<std::uint64_t>(p) + residues[index % residues.size()]) % modulus);
            }
            input.push_back(memory.add("fixture/mod" + std::to_string(full[limb]), words,
                hpu::runtime::AllocationKind::workspace, false).span);
        }
        const auto allocation = [&](const std::string& suffix) {
            return memory.allocation(constants.hardware_prefix + suffix).span;
        };
        std::vector<hpu::runtime::HpuMemSpan> spans{table};
        if (rounded) {
            for (std::size_t limb = 0; limb < full.size(); ++limb) {
                spans.push_back(input[limb]);
                spans.push_back(allocation("/half/mod" + std::to_string(full[limb])));
                spans.push_back(input[limb]);
            }
        }
        spans.push_back(input.back());
        spans.push_back(allocation("/moddown/qhat_inv/mod" + std::to_string(p_id)));
        spans.push_back(allocation("/workspace/bconv/normalized0"));
        for (int q_id : level.rns_layout.q_mod_ids) {
            spans.push_back(allocation("/workspace/bconv/normalized0"));
            spans.push_back(allocation("/moddown/qhat_mod_target/target"
                + std::to_string(q_id) + "/source" + std::to_string(p_id)));
            spans.push_back(allocation("/workspace/moddown/correction/mod" + std::to_string(q_id)));
        }
        std::vector<hpu::model::InstructionMemoryExpectation> expectations;
        for (std::size_t limb = 0; limb < level.q_moduli.size(); ++limb) {
            const auto q_id = std::to_string(level.rns_layout.q_mod_ids[limb]);
            spans.push_back(input[limb]);
            spans.push_back(allocation("/workspace/moddown/correction/mod" + q_id));
            spans.push_back(allocation("/moddown/p_inverse/mod" + q_id));
            spans.push_back(input[limb]);
            std::vector<std::uint32_t> expected(degree);
            for (std::size_t index = 0; index < degree; ++index) {
                expected[index] = static_cast<std::uint32_t>(3 + index % 7
                    + (residues[index % residues.size()] > p / 2 ? 1 : 0));
            }
            expectations.push_back({"rounding/mod" + q_id, input[limb], expected});
        }
        const auto body = rounded
            ? ::generate_hpu_rounded_single_p_moddown_contexts_body_asm(
                level.rns_layout.q_mod_ids, p_id, 1, false, false)
            : ::generate_hpu_moddown_contexts_body_asm(
                level.rns_layout.q_mod_ids, {p_id}, false, false);
        const auto instructions = hpu::assemble_source(
            hpu::dload(4, hpu::DataType::mod_ctx, hpu::DloadFlag::small_bank)
            + body + hpu::pfree(4) + hpu::psync());
        const auto execution = hpu::model::execute_instructions(instructions, spans, memory);
        require(execution.report.completed(), execution.report.message);
        return hpu::model::compare_instruction_memory(execution, expectations).verified;
    };
    require(fixture(true), "rounded ModDown differs from integer rounding at the half-P boundary");
    require(!fixture(false), "half-P fixture failed to distinguish unrounded ModDown");
}

// Compute the special-P accumulator independently with SEAL's NTT primitives.
// This only measures test coverage: neither the HPU model nor its constants are
// used to choose or validate the rounded residue convention.
void require_rounding_coverage(
    const ::seal::Ciphertext& source, std::size_t source_component,
    const std::vector<::seal::PublicKey>& digits,
    const CkksLevelDescriptor& level, const ::seal::SEALContext& context)
{
    const auto data = context.get_context_data(source.parms_id());
    const auto key = context.key_context_data();
    const auto degree = source.poly_modulus_degree();
    const auto p_index = key->parms().coeff_modulus().size() - 1;
    const auto p = key->parms().coeff_modulus()[p_index].value();
    const auto& p_table = key->small_ntt_tables()[p_index];
    std::size_t lower = 0;
    std::size_t upper = 0;
    for (std::size_t component = 0; component < 2; ++component) {
        std::vector<std::uint64_t> accumulator(degree, 0);
        for (std::size_t digit = 0; digit < level.q_moduli.size(); ++digit) {
            const auto* first = source.data(source_component) + digit * degree;
            std::vector<std::uint64_t> converted(first, first + degree);
            ::seal::util::inverse_ntt_negacyclic_harvey(
                converted.data(), data->small_ntt_tables()[digit]);
            for (auto& value : converted) value %= p;
            ::seal::util::ntt_negacyclic_harvey(converted.data(), p_table);
            const auto& key_digit = digits[level.evaluation_key_digit_indices[digit]].data();
            const auto* key_words = key_digit.data(component) + p_index * degree;
            for (std::size_t index = 0; index < degree; ++index) {
                accumulator[index] = (accumulator[index]
                    + converted[index] * key_words[index]) % p;
            }
        }
        ::seal::util::inverse_ntt_negacyclic_harvey(accumulator.data(), p_table);
        for (auto value : accumulator) {
            lower += value <= p / 2;
            upper += value > p / 2;
        }
    }
    require(lower != 0 && upper != 0,
        "relinearization fixture did not exercise both special-P half intervals");
}

std::vector<hpu::model::InstructionMemoryExpectation> expectations_for(
    const PreparedRnsObject& output, const ::seal::Ciphertext& oracle,
    const ::seal::SEALContext& context)
{
    require(output.parms_id == oracle.parms_id()
            && output.components.size() == oracle.size(),
        "planned output metadata differs from SEAL");
    std::vector<hpu::model::InstructionMemoryExpectation> result;
    for (std::size_t component = 0; component < oracle.size(); ++component) {
        const auto converted = ciphertext_component_to_hpu(oracle, component, context);
        for (std::size_t limb = 0; limb < output.components[component].limbs.size(); ++limb) {
            const auto first = converted.words.begin() + limb * converted.degree;
            result.push_back({output.id + "/c" + std::to_string(component)
                    + "/mod" + std::to_string(converted.modulus_ids[limb]),
                output.components[component].limbs[limb],
                {first, first + converted.degree}});
        }
    }
    return result;
}

void run_level(
    const ::seal::SEALContext& context, const CkksLevelDescriptor& level,
    const ::seal::Ciphertext& encrypted, const ::seal::RelinKeys& relin_keys,
    const ::seal::GaloisKeys& galois_keys)
{
    constexpr int rotation_steps = -1;
    const auto rotation_element = hpu::scheme::ckks::rotation_galois_element(128, rotation_steps);
    const auto conjugation_element = hpu::scheme::ckks::conjugation_galois_element(128);
    ::seal::Evaluator evaluator(context);
    ::seal::Ciphertext tensor, oracle_relinearized, oracle_rotated, oracle_conjugated;
    evaluator.square(encrypted, tensor);
    evaluator.relinearize(tensor, relin_keys, oracle_relinearized);
    evaluator.rotate_vector(encrypted, rotation_steps, galois_keys, oracle_rotated);
    evaluator.complex_conjugate(encrypted, galois_keys, oracle_conjugated);
    require_rounding_coverage(tensor, 2, relin_keys.key(2), level, context);

    CkksApplicationImageBuilder builder(context, 2048);
    builder.add_modulus_table();
    builder.add_canonical_twiddles();
    const auto input = builder.add_ciphertext("input/x", encrypted);
    const auto prepared_tensor = builder.add_ciphertext("input/tensor", tensor);
    const auto constants = builder.add_keyswitch_constants("constants/keyswitch", level);
    const auto relin = builder.add_relinearization_key("key/relinearization", relin_keys, level);
    const auto rotation = builder.add_rotation_key("key/rotation", galois_keys, rotation_steps, level);
    const auto conjugation = builder.add_conjugation_key("key/conjugation", galois_keys, level);
    const auto rotation_tables = builder.add_rotation_twiddles("rotation", rotation_steps, level);
    const auto conjugation_tables = builder.add_conjugation_twiddles("conjugation", level);
    const auto coefficient = hpu::runtime::PolynomialDomain::coefficient;
    const auto scratch = hpu::runtime::AllocationKind::workspace;
    const auto relin_workspace = builder.reserve_ciphertext(
        "scratch/relinearization", prepared_tensor.metadata(), 3, coefficient, 1, scratch);
    const auto rotation_workspace = builder.reserve_ciphertext(
        "scratch/rotation", input.metadata(), 2, coefficient, rotation_element, scratch);
    const auto conjugation_workspace = builder.reserve_ciphertext(
        "scratch/conjugation", input.metadata(), 2, coefficient, conjugation_element, scratch);

    CkksOperationPlan plan(builder);
    const auto relinearized = plan.append_relinearize(
        "relinearize", prepared_tensor, relin, constants, "output/relinearized", &relin_workspace);
    const auto rotated = plan.append_rotate_slots(
        "rotate", input, rotation_steps, rotation, constants,
        rotation_tables, rotation_workspace, "output/rotated");
    const auto conjugated = plan.append_conjugate(
        "conjugate", input, conjugation, constants,
        conjugation_tables, conjugation_workspace, "output/conjugated");
    const auto lowered = lower_ckks_operation_plan(plan, context);
    const auto relocation = build_ckks_relocation_schedule(lowered, builder.image(), context);
    require(relocation.complete(), "KeySwitch relocation is incomplete");
    const auto runtime = lower_ckks_runtime_program(lowered, relocation);
    hpu::model::InstructionExecutionOptions options;
    // The image contains natural-indexed pre/post factors. Complete transforms
    // use the functional natural boundaries, without claiming RTL stage timing.
    options.ntt_contract = hpu::model::InstructionNttContract::natural_coefficient_boundaries;
    auto words_only = runtime.instructions;
    for (auto& instruction : words_only) {
        instruction.instruction = {};
        instruction.normalized_asm.clear();
        instruction.command26 = 0;
    }
    const auto execution = hpu::model::execute_instructions(
        words_only, runtime.spans(), builder.image(), options);
    require(execution.report.completed(), execution.report.message);
    std::vector<hpu::model::InstructionMemoryExpectation> expectations;
    for (const auto& pair : std::vector<std::pair<PreparedRnsObject, ::seal::Ciphertext>>{
             {relinearized, oracle_relinearized}, {rotated, oracle_rotated},
             {conjugated, oracle_conjugated}}) {
        const auto limbs = expectations_for(pair.first, pair.second, context);
        expectations.insert(expectations.end(), limbs.begin(), limbs.end());
    }
    const auto comparison = hpu::model::compare_instruction_memory(execution, expectations);
    require(comparison.verified, "Q" + std::to_string(level.q_moduli.size()) + ": " + comparison.message);
    require(comparison.compared_words == 6 * level.q_moduli.size() * encrypted.poly_modulus_degree(),
        "not all three KeySwitch outputs were compared");

    // Internal coefficient conversion must not overwrite either pinned input.
    for (const auto& prepared : {input, prepared_tensor}) {
        for (const auto& component : prepared.components) {
            for (const auto& span : component.limbs) {
                const auto offset = span.line_offset * hpu::runtime::kHpuMemLineWords;
                require(std::equal(builder.image().words().begin() + offset,
                        builder.image().words().begin() + offset + component.degree,
                        execution.words.begin() + offset),
                    "KeySwitch overwrote a read-only NTT input snapshot");
            }
        }
    }
}

} // namespace

int main()
{
    try {
        CkksContextSpec spec;
        spec.poly_modulus_degree = 128;
        spec.coeff_modulus_bits = {20, 20, 20, 20};
        const auto bundle = create_ckks_context(spec);
        const CkksLevelChain levels(*bundle.context);
        rounded_moddown_boundaries(*bundle.context, levels.top());
        rounded_moddown_boundaries(*bundle.context, levels.next(levels.top().parms_id));
        ::seal::KeyGenerator key_generator(*bundle.context);
        ::seal::RelinKeys relin_keys;
        ::seal::GaloisKeys galois_keys;
        key_generator.create_relin_keys(relin_keys);
        key_generator.create_galois_keys(std::vector<std::uint32_t>{
            hpu::scheme::ckks::rotation_galois_element(128, -1),
            hpu::scheme::ckks::conjugation_galois_element(128)}, galois_keys);
        ::seal::CKKSEncoder encoder(*bundle.context);
        ::seal::Plaintext encoded;
        encoder.encode(std::vector<std::complex<double>>{
            {0.25, 0.5}, {-1.0, 0.75}, {1.5, -0.25}, {0.75, -0.5}}, 4096.0, encoded);
        ::seal::Ciphertext encrypted;
        ::seal::Encryptor(*bundle.context, key_generator.secret_key()).encrypt_symmetric(encoded, encrypted);
        run_level(*bundle.context, levels.top(), encrypted, relin_keys, galois_keys);
        ::seal::Evaluator(*bundle.context).mod_switch_to_next_inplace(encrypted);
        run_level(*bundle.context, levels.next(levels.top().parms_id), encrypted, relin_keys, galois_keys);
        std::cout << "CKKS rounded KeySwitch instructions match SEAL at two levels under legacy raw stages\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CKKS KeySwitch instruction test failed: " << error.what() << '\n';
        return 1;
    }
}
