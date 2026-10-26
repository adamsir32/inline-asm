#include "hpu/model/instruction_executor.hpp"
#include "hpu/model/hardware_ntt.hpp"
#include "assembler.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

using hpu::model::InstructionExecutionStatus;
using hpu::model::InstructionNttContract;
using hpu::runtime::AllocationKind;
using hpu::runtime::HpuMemImage;
using hpu::runtime::HpuMemSpan;
constexpr std::uint32_t kModulus = 2013265921U;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint32_t> modulus_record(std::uint32_t q)
{
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto mu = maximum / q + ((maximum % q) + 1 >= q ? 1 : 0);
    return {q, static_cast<std::uint32_t>(mu), static_cast<std::uint32_t>(mu >> 32U), 0};
}

std::size_t bit_reverse(std::size_t value, std::size_t degree)
{
    std::size_t result = 0;
    for (std::size_t width = degree; width > 1; width >>= 1U) {
        result = (result << 1U) | (value & 1U);
        value >>= 1U;
    }
    return result;
}

void arithmetic_and_dma()
{
    constexpr std::uint32_t q = 65537;
    HpuMemImage image(16);
    const auto table = image.add("moduli", modulus_record(q), AllocationKind::modulus_table);
    std::vector<std::uint32_t> left(64), right(64), expected(64);
    for (std::size_t i = 0; i < 64; ++i) {
        left[i] = std::numeric_limits<std::uint32_t>::max() - static_cast<std::uint32_t>(i * 337);
        right[i] = static_cast<std::uint32_t>(i * 71033 + 1);
        const std::uint64_t a = left[i] % q;
        const std::uint64_t b = right[i] % q;
        expected[i] = static_cast<std::uint32_t>(((a + (a * b) % q) % q) * 7 % q);
    }
    const auto a = image.add("left", left, AllocationKind::ciphertext);
    const auto b = image.add("right", right, AllocationKind::ciphertext);
    const auto output = image.reserve("output", 64, AllocationKind::output);
    const auto instructions = hpu::assemble_source(
        "dload x10, x11, p4, 1, 0, 32\n"
        "pmodld 0\n"
        "dload x10, x11, p0, 0, 0, 0\n"
        "pmul p0, p0, 1\n"
        "dload x10, x11, p1, 0, 0, 0\n"
        "pmul p1, p1, 1\n"
        "padd p2, p0, p1\n"
        "psub p2, p2, p1\n"
        "pmac p2, p0, p1\n"
        "pmul p2, p2, 7\n"
        "dstore x10, x11, p2, 0, 0\n"
        "pfree p0\npfree p1\npfree p4\npsync\n");
    const std::vector<HpuMemSpan> spans{table.span, a.span, b.span, output.span};
    const auto result = hpu::model::execute_instructions(instructions, spans, image);
    require(result.report.completed(), "encoded arithmetic execution failed");
    require(result.report.ntt_contract == InstructionNttContract::raw_bit_reversed_coefficients
                && result.report.forward_boundary_conversions == 0
                && !result.report.hardware_contract_verified,
            "software completion was confused with hardware contract verification");
    require(result.report.peak_regular_banks == 4 && result.report.stored_words == 64,
            "arithmetic resource accounting failed");
    const std::vector<hpu::model::InstructionMemoryExpectation> expectations{{"output", output.span, expected}};
    require(hpu::model::compare_instruction_memory(result, expectations).verified,
            "encoded arithmetic differs from independent oracle");

    // CP is command-local and independent of MOD/REL. Toggle the actual raw
    // instruction bit on both DMA directions; payload and SRAM placement must
    // be identical, without claiming that the model predicts LLC performance.
    auto cache_hinted = instructions;
    for (auto& item : cache_hinted)
        if ((item.word & 0x7fU) == 0x2bU) item.word ^= 1U << 13U;
    const auto hinted = hpu::model::execute_instructions(cache_hinted, spans, image);
    require(hpu::model::compare_instruction_memory(hinted, expectations).verified
                && hinted.words == result.words
                && hinted.report.peak_regular_lines == result.report.peak_regular_lines
                && hinted.report.peak_small_lines == result.report.peak_small_lines
                && hinted.report.loaded_words == result.report.loaded_words
                && hinted.report.stored_words == result.report.stored_words,
            "CP changed functional execution, ownership or physical allocation");

    // Semantic metadata is not the executable: replacing it must not affect
    // decoding, lifetime validation, DMA binding, or numerical execution.
    auto misleading_metadata = instructions;
    for (auto& item : misleading_metadata) {
        item.instruction = {};
        item.normalized_asm = "psync";
        item.command26 = 0;
    }
    const auto decoded = hpu::model::execute_instructions(misleading_metadata, spans, image);
    require(hpu::model::compare_instruction_memory(decoded, expectations).verified,
            "executor trusted companion metadata instead of encoded words");

    auto wrong_expectations = expectations;
    wrong_expectations[0].words[17] ^= 1;
    const auto mismatch = hpu::model::compare_instruction_memory(result, wrong_expectations);
    require(!mismatch.verified && mismatch.object_word_index == 17 && mismatch.object_id == "output",
            "word mismatch was not reported precisely");
    require(!hpu::model::compare_instruction_memory(result, {}).verified,
            "empty oracle comparison was accepted as verified");

    auto readonly_spans = spans;
    readonly_spans.back() = a.span;
    const auto readonly_result = hpu::model::execute_instructions(instructions, readonly_spans, image);
    require(!readonly_result.report.completed()
                && readonly_result.report.message.find("read-only") != std::string::npos,
            "DSTORE overwrote immutable input");
    auto missing_spans = spans;
    missing_spans.pop_back();
    require(!hpu::model::execute_instructions(instructions, missing_spans, image).report.completed(),
            "missing DMA binding was accepted");
    auto extra_spans = spans;
    extra_spans.push_back(output.span);
    require(!hpu::model::execute_instructions(instructions, extra_spans, image).report.completed(),
            "extra DMA binding was accepted");

    auto released_use = instructions;
    released_use[11] = hpu::assemble_source("pmul p0, p2, 1\n").front();
    const auto release_result = hpu::model::execute_instructions(released_use, spans, image);
    require(release_result.report.completed(), "DSTORE REL=0 failed to retain its source");
    auto released = instructions;
    released[10] = hpu::assemble_source("dstore x10, x11, p2, 1, 1\n").front();
    released[11] = hpu::assemble_source("pmul p0, p2, 1\n").front();
    const auto released_result = hpu::model::execute_instructions(released, spans, image);
    require(!released_result.report.completed() && released_result.report.instruction_index == 11,
            "DSTORE REL=1 failed to release its source");

    auto reserved_word = instructions;
    reserved_word[6].word |= 1U << 14U;
    require(!hpu::model::execute_instructions(reserved_word, spans, image).report.completed(),
            "reserved instruction bits were ignored");
    hpu::model::InstructionExecutionOptions unsupported_contract;
    unsupported_contract.ntt_contract = static_cast<InstructionNttContract>(999);
    const auto unknown_contract = hpu::model::execute_instructions(
        instructions, spans, image, unsupported_contract);
    require(unknown_contract.report.status == InstructionExecutionStatus::unsupported_contract
                && !unknown_contract.report.hardware_contract_verified,
            "unknown NTT contract was silently executed");
}

void supplied_stages(std::size_t degree)
{
    const auto omega = hpu::model::pow_mod(31, (kModulus - 1U) / degree, kModulus);
    hpu::model::HardwareNttModel model(degree, kModulus, omega);
    const auto forward_tables = model.forward_twiddles();
    const auto inverse_tables = model.inverse_twiddles();
    std::vector<std::uint32_t> input(degree), raw(degree);
    for (std::size_t i = 0; i < degree; ++i) input[i] = static_cast<std::uint32_t>(i * i + 17 * i + 5);
    for (std::size_t i = 0; i < degree; ++i) raw[i] = input[bit_reverse(i, degree)];
    for (std::size_t stage = 0; stage < forward_tables.size(); ++stage)
        raw = hpu::model::execute_ntt_stage(raw, forward_tables[stage], kModulus, stage);
    const auto expected_forward = model.forward(input);
    require(raw == expected_forward, "raw forward stages changed the existing mathematical model");
    for (std::size_t stage = 0; stage < inverse_tables.stages.size(); ++stage)
        raw = hpu::model::execute_ntt_stage(raw, inverse_tables.stages[stage], kModulus, stage, true);
    for (std::size_t i = 0; i < degree; ++i) {
        raw[i] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(raw[i])
                                          * inverse_tables.post_scale[i] % kModulus);
        require(raw[i] == input[bit_reverse(i, degree)],
                "raw inverse stages unexpectedly performed a hidden boundary conversion");
    }

    HpuMemImage image(4096);
    const auto table = image.add("moduli", modulus_record(kModulus), AllocationKind::modulus_table);
    const auto data = image.add("input", hpu::model::bit_reverse_coefficients(input), AllocationKind::ciphertext);
    std::vector<HpuMemSpan> spans{table.span, data.span};
    std::ostringstream body;
    body << "dload x10, x11, p4, 1, 0, 32\npmodld 0\ndload x10, x11, p0, 0, 0, 0\n";
    for (std::size_t stage = 0; stage < forward_tables.size(); ++stage) {
        const auto twiddle = image.add("forward/" + std::to_string(stage), hpu::model::pack_stage_twiddles(forward_tables[stage], kModulus, degree), AllocationKind::twiddle);
        spans.push_back(twiddle.span);
        body << "dload x10, x11, p1, 0, 0, 32\npntt p0, p1, " << stage << ", 0, 0\npfree p1\n";
    }
    const auto output = image.reserve("output", degree, AllocationKind::output);
    spans.push_back(output.span);
    body << "dstore x10, x11, p0, 1, 0\npfree p4\npsync\n";
    const auto instructions = hpu::assemble_source(body.str());
    const auto execution = hpu::model::execute_instructions(instructions, spans, image);
    require(execution.report.completed(), "supplied forward instruction chain failed");
    require(execution.report.forward_boundary_conversions == 0
                && execution.report.ntt_contract == InstructionNttContract::raw_bit_reversed_coefficients,
            "raw forward contract introduced a hidden boundary conversion");
    require(execution.report.peak_regular_banks == 2, "transform output region was not accounted");
    require(hpu::model::compare_instruction_memory(execution, {{"output", output.span, expected_forward}}).verified,
            "encoded stages ignored actual supplied twiddles/input permutation");

    auto changed_spans = spans;
    auto changed_twiddles = hpu::model::pack_stage_twiddles(forward_tables.front(), kModulus, degree);
    changed_twiddles[0] = 2;
    const auto changed = image.add("changed/first", changed_twiddles, AllocationKind::twiddle);
    changed_spans[2] = changed.span;
    const auto changed_execution = hpu::model::execute_instructions(instructions, changed_spans, image);
    require(changed_execution.report.completed()
                && !hpu::model::compare_instruction_memory(changed_execution, {{"output", output.span, expected_forward}}).verified,
            "corrupt twiddle DMA payload did not change instruction result");
    if (degree != 256) {
        // N=256's old all-one stage 0 is byte-identical to valid K=8
        // parameters; a header-free ABI cannot distinguish those bytes.
        auto legacy_spans = spans;
        legacy_spans[2] = image.add("old/full/table", forward_tables.front(), AllocationKind::twiddle).span;
        const auto legacy = hpu::model::execute_instructions(instructions, legacy_spans, image);
        require(!legacy.report.completed()
                    && legacy.report.message.find("two-line K=8") != std::string::npos,
                "encoded transform silently accepted the old full-table format");
    }
    hpu::model::InstructionExecutionOptions constrained;
    constrained.regular_bank_count = 1;
    const auto no_output_bank = hpu::model::execute_instructions(instructions, spans, image, constrained);
    require(!no_output_bank.report.completed() && no_output_bank.report.peak_regular_banks == 1,
            "NTT ran without a distinct output bank");

    // A separate inverse instruction stream executes raw stages and explicit
    // post multiplication. Its physical result is compared without an invented
    // software-only natural-order permutation.
    const auto inverse_input = image.add("inverse/input", expected_forward, AllocationKind::ciphertext);
    std::vector<HpuMemSpan> inverse_spans{table.span, inverse_input.span};
    std::ostringstream inverse_body;
    inverse_body << "dload x10, x11, p4, 1, 0, 32\npmodld 0\ndload x10, x11, p0, 0, 0, 0\n";
    for (std::size_t stage = 0; stage < inverse_tables.stages.size(); ++stage) {
        const auto twiddle = image.add("inverse/" + std::to_string(stage), hpu::model::pack_stage_twiddles(inverse_tables.stages[stage], kModulus, degree), AllocationKind::twiddle);
        inverse_spans.push_back(twiddle.span);
        inverse_body << "dload x10, x11, p1, 0, 0, 32\npintt p0, p1, " << stage << ", 0, 0\npfree p1\n";
    }
    const auto post = image.add("inverse/post", inverse_tables.post_scale, AllocationKind::twiddle);
    inverse_spans.push_back(post.span);
    inverse_spans.push_back(output.span);
    inverse_body << "dload x10, x11, p1, 0, 0, 0\npmul p0, p0, p1\npfree p1\n"
                 << "dstore x10, x11, p0, 1, 0\npfree p4\npsync\n";
    hpu::model::InstructionExecutionOptions raw_options;
    raw_options.ntt_contract = InstructionNttContract::legacy_raw_stage;
    const auto inverse_execution = hpu::model::execute_instructions(
        hpu::assemble_source(inverse_body.str()), inverse_spans, image, raw_options);
    require(hpu::model::compare_instruction_memory(inverse_execution, {{"inverse/output", output.span, raw}}).verified,
            "inverse encoded stages do not match raw stage/post semantics");
    require(inverse_execution.report.forward_boundary_conversions == 0,
            "PINTT invented a mathematical boundary conversion");
}

void pre_twist_is_pointwise()
{
    constexpr std::size_t degree = 128;
    const auto psi = hpu::model::pow_mod(31, (kModulus - 1U) / (2 * degree), kModulus);
    HpuMemImage image(32);
    const auto table = image.add("moduli", modulus_record(kModulus), AllocationKind::modulus_table);
    std::vector<std::uint32_t> input(degree), twist(degree), expected(degree);
    for (std::size_t i = 0; i < degree; ++i) {
        input[i] = static_cast<std::uint32_t>(i + 1);
        twist[i] = hpu::model::pow_mod(psi, bit_reverse(i, degree), kModulus);
        expected[i] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(input[i]) * twist[i] % kModulus);
    }
    const auto data = image.add("input", input, AllocationKind::ciphertext);
    const auto pre = image.add("existing/pre_twist", twist, AllocationKind::twiddle);
    const auto output = image.reserve("output", degree, AllocationKind::output);
    const auto instructions = hpu::assemble_source(
        "dload x10, x11, p4, 1, 0, 32\npmodld 0\ndload x10, x11, p0, 0, 0, 0\n"
        "dload x10, x11, p1, 0, 0, 0\npmul p0, p0, p1\npfree p1\n"
        "dstore x10, x11, p0, 1, 0\npfree p4\npsync\n");
    const auto execution = hpu::model::execute_instructions(instructions, {table.span, data.span, pre.span, output.span}, image);
    require(hpu::model::compare_instruction_memory(execution, {{"output", output.span, expected}}).verified,
            "pointwise PMUL secretly reindexed the supplied pre_twist table");
}

void forward_twist_layout_contract()
{
    // An independent monomial oracle distinguishes a wrong twist factor from
    // an output permutation. For c(X)=X, evaluations are psi^(2*k+1).
    constexpr std::size_t degree = 128;
    constexpr std::uint32_t q = 65537;
    constexpr std::uint32_t psi = 282;
    hpu::model::HardwareNttModel model(degree, q, hpu::model::pow_mod(psi, 2, q));
    const auto stages = model.forward_twiddles();
    const auto layout = model.forward_layout();
    std::vector<std::uint32_t> expected(degree), natural(degree), physical(degree);
    std::vector<std::uint32_t> natural_twist(degree), physical_twist(degree);
    natural[1] = 1;
    for (std::size_t p = 0; p < degree; ++p) {
        expected[p] = hpu::model::pow_mod(psi, 2 * layout[p] + 1, q);
        physical[p] = natural[bit_reverse(p, degree)];
        natural_twist[p] = hpu::model::pow_mod(psi, p, q);
        physical_twist[p] = natural_twist[bit_reverse(p, degree)];
    }
    const auto run = [&](const std::vector<std::uint32_t>& data,
                         const std::vector<std::uint32_t>& twist,
                         InstructionNttContract contract) {
        HpuMemImage image(128);
        const auto table = image.add("moduli", modulus_record(q), AllocationKind::modulus_table);
        const auto input = image.add("input", data, AllocationKind::ciphertext);
        const auto pre = image.add("pre", twist, AllocationKind::twiddle);
        std::vector<HpuMemSpan> spans{table.span, input.span, pre.span};
        std::ostringstream body;
        body << "dload x10, x11, p4, 1, 0, 32\npmodld 0\n"
             << "dload x10, x11, p0, 0, 0, 0\ndload x10, x11, p1, 0, 0, 0\n"
             << "pmul p0, p0, p1\npfree p1\n";
        for (std::size_t s = 0; s < stages.size(); ++s) {
            spans.push_back(image.add("stage/" + std::to_string(s), hpu::model::pack_stage_twiddles(stages[s], q, degree), AllocationKind::twiddle).span);
            body << "dload x10, x11, p1, 0, 0, 32\npntt p0, p1, " << s << ", 0, 0\npfree p1\n";
        }
        const auto output = image.reserve("output", degree, AllocationKind::output);
        spans.push_back(output.span);
        body << "dstore x10, x11, p0, 1, 0\npfree p4\npsync\n";
        hpu::model::InstructionExecutionOptions options;
        options.ntt_contract = contract;
        const auto result = hpu::model::execute_instructions(hpu::assemble_source(body.str()), spans, image, options);
        require(result.report.completed(), "forward twist contract execution failed");
        require(result.report.ntt_contract == contract && !result.report.hardware_contract_verified,
                "contract selection or hardware status was not preserved");
        require(result.report.forward_boundary_conversions
                    == (contract == InstructionNttContract::natural_coefficient_boundaries ? 1U : 0U),
                "forward contract applied an unreported boundary conversion");
        require(result.report.message.find(hpu::model::instruction_ntt_contract_name(contract))
                    != std::string::npos,
                "execution report omitted the software contract name");
        const auto begin = output.span.line_offset * hpu::runtime::kHpuMemLineWords;
        return std::vector<std::uint32_t>(result.words.begin() + begin, result.words.begin() + begin + degree);
    };
    require(run(natural, natural_twist, InstructionNttContract::natural_coefficient_boundaries) == expected,
            "natural coefficient boundaries disagree with the independent monomial oracle");
    require(run(physical, physical_twist, InstructionNttContract::raw_bit_reversed_coefficients) == expected,
            "physical coefficients/twist disagree with monomial oracle");
    require(run(natural, natural_twist, InstructionNttContract::legacy_raw_stage) != expected,
            "raw stage contract silently performed a natural coefficient conversion");
    auto mixed = run(natural, physical_twist, InstructionNttContract::natural_coefficient_boundaries);
    require(mixed != expected, "mixed coefficient/twist layouts were silently repaired by the model");
    auto sorted_expected = expected;
    std::sort(mixed.begin(), mixed.end());
    std::sort(sorted_expected.begin(), sorted_expected.end());
    require(mixed != sorted_expected, "mixed layouts were incorrectly treated as an output permutation");
}

void inverse_boundary_contract()
{
    // Construct the monomial spectrum directly, rather than by round-tripping
    // the implementation under test. The production raw profile retains the raw
    // inverse result c[BR(p)]; the mathematical API returns c[p].
    constexpr std::size_t degree = 1024;
    constexpr std::uint32_t q = 134072321;
    constexpr std::uint32_t psi = 58811;
    hpu::model::HardwareNttModel model(degree, q, hpu::model::pow_mod(psi, 2, q));
    const auto layout = model.forward_layout();
    const auto tables = model.inverse_twiddles();
    const auto inverse_psi = hpu::model::inverse_mod_prime(psi, q);
    std::vector<std::uint32_t> spectrum(degree), natural(degree), physical(degree), post(degree);
    natural[1] = 1;
    for (std::size_t p = 0; p < degree; ++p) {
        spectrum[p] = hpu::model::pow_mod(psi, 2 * layout[p] + 1, q);
        physical[p] = natural[bit_reverse(p, degree)];
        post[p] = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(tables.post_scale[p])
            * hpu::model::pow_mod(inverse_psi, bit_reverse(p, degree), q) % q);
    }
    require(hpu::model::negacyclic_inverse(spectrum, q, psi) == natural,
            "mathematical inverse does not return the independent natural-order monomial");
    require(physical != natural, "inverse contract probe does not distinguish the two layouts");

    HpuMemImage image(256);
    const auto moduli = image.add("moduli", modulus_record(q), AllocationKind::modulus_table);
    const auto input = image.add("input", spectrum, AllocationKind::ciphertext);
    std::vector<HpuMemSpan> spans{moduli.span, input.span};
    std::ostringstream body;
    body << "dload x10, x11, p4, 1, 0, 32\npmodld 0\ndload x10, x11, p0, 0, 0, 0\n";
    for (std::size_t s = 0; s < tables.stages.size(); ++s) {
        spans.push_back(image.add("inverse/" + std::to_string(s), hpu::model::pack_stage_twiddles(tables.stages[s], q, degree), AllocationKind::twiddle).span);
        body << "dload x10, x11, p1, 0, 0, 32\npintt p0, p1, " << s << ", 0, 0\npfree p1\n";
    }
    spans.push_back(image.add("post", hpu::model::pack_online_pmul_factors(post, q), AllocationKind::twiddle).span);
    const auto output = image.reserve("output", degree, AllocationKind::output);
    spans.push_back(output.span);
    body << "dload x10, x11, p1, 0, 1, 32\npmul p0, p0, p1, 1\npfree p1\n"
         << "dstore x10, x11, p0, 1, 0\npfree p4\npsync\n";
    const auto instructions = hpu::assemble_source(body.str());
    hpu::model::InstructionExecutionOptions options;
    options.ntt_contract = InstructionNttContract::raw_bit_reversed_coefficients;
    const auto execution = hpu::model::execute_instructions(instructions, spans, image, options);
    require(hpu::model::compare_instruction_memory(execution, {{"physical", output.span, physical}}).verified,
            "raw PINTT/post did not produce the independent physical-order monomial");
    require(!hpu::model::compare_instruction_memory(execution, {{"natural", output.span, natural}}).verified,
            "instruction inverse silently adopted the mathematical API's boundary conversion");
    require(execution.report.forward_boundary_conversions == 0
                && execution.report.inverse_boundary_conversions == 0
                && !execution.report.hardware_contract_verified,
            "raw inverse execution invented a boundary conversion or hardware proof");
}

void raw_online_negacyclic_convolution(std::size_t degree)
{
    constexpr std::uint32_t q = 65537;
    const auto psi = hpu::model::pow_mod(3, (q - 1U) / (2 * degree), q);
    const auto inverse_psi = hpu::model::inverse_mod_prime(psi, q);
    hpu::model::HardwareNttModel model(degree, q, hpu::model::pow_mod(psi, 2, q));
    const auto forward = model.forward_twiddles();
    const auto inverse = model.inverse_twiddles();
    const auto layout = model.forward_layout();
    std::vector<std::uint32_t> input(degree), pre(degree), post(degree);
    std::vector<std::uint32_t> spectrum(degree), squared(degree);
    for (std::size_t i = 0; i < degree; ++i) {
        input[i] = static_cast<std::uint32_t>((i * i + 17 * i + 1) % q);
        pre[i] = hpu::model::pow_mod(psi, i, q);
        post[i] = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(inverse.post_scale[bit_reverse(i, degree)])
            * hpu::model::pow_mod(inverse_psi, i, q) % q);
    }
    // Independent odd-root polynomial evaluation and schoolbook multiplication.
    // A round-trip alone would accept the old DFT(BR(input)) convention.
    for (std::size_t p = 0; p < degree; ++p) {
        const auto point = hpu::model::pow_mod(psi, 2 * layout[p] + 1, q);
        std::uint64_t value = 0;
        for (std::size_t i = degree; i > 0; --i)
            value = (value * point + input[i - 1]) % q;
        spectrum[p] = static_cast<std::uint32_t>(value);
    }
    for (std::size_t i = 0; i < degree; ++i) {
        for (std::size_t j = 0; j < degree; ++j) {
            const auto product = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(input[i]) * input[j] % q);
            const auto position = (i + j) % degree;
            squared[position] = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(squared[position])
                    + (i + j < degree ? product : q - product)) % q);
        }
    }
    HpuMemImage image(1024);
    const auto physical_input = hpu::model::bit_reverse_coefficients(input);
    const auto physical_pre = hpu::model::bit_reverse_coefficients(pre);
    const auto physical_post = hpu::model::bit_reverse_coefficients(post);
    const auto physical_squared = hpu::model::bit_reverse_coefficients(squared);
    const auto moduli = image.add("moduli", modulus_record(q), AllocationKind::modulus_table);
    const auto data = image.add("input", physical_input, AllocationKind::ciphertext);
    const auto pre_factor = image.add("pre", hpu::model::pack_online_pmul_factors(physical_pre, q), AllocationKind::twiddle);
    const auto ntt_output = image.reserve("ntt/output", degree, AllocationKind::output);
    const auto output = image.reserve("square/output", degree, AllocationKind::output);
    std::vector<HpuMemSpan> spans{moduli.span, data.span, pre_factor.span};
    std::ostringstream body;
    body << "dload x10, x11, p4, 1, 0, 32\npmodld 0\ndload x10, x11, p0, 0, 0, 0\n"
         << "dload x10, x11, p1, 0, 1, 32\npmul p0, p0, p1, 1\npfree p1\n";
    for (std::size_t s = 0; s < forward.size(); ++s) {
        spans.push_back(image.add("ntt/stage/" + std::to_string(s), hpu::model::pack_stage_twiddles(forward[s], q, degree), AllocationKind::twiddle).span);
        body << "dload x10, x11, p1, 0, 0, 32\npntt p0, p1, " << s << ", 0, 0\npfree p1\n";
    }
    spans.push_back(ntt_output.span);
    spans.push_back(ntt_output.span);
    body << "dstore x10, x11, p0, 1, 0\ndload x10, x11, p0, 0, 0, 0\npmul p0, p0, p0\n";
    for (std::size_t s = 0; s < inverse.stages.size(); ++s) {
        spans.push_back(image.add("intt/stage/" + std::to_string(s), hpu::model::pack_stage_twiddles(inverse.stages[s], q, degree), AllocationKind::twiddle).span);
        body << "dload x10, x11, p1, 0, 0, 32\npintt p0, p1, " << s << ", 0, 0\npfree p1\n";
    }
    spans.push_back(image.add("post", hpu::model::pack_online_pmul_factors(physical_post, q), AllocationKind::twiddle).span);
    spans.push_back(output.span);
    body << "dload x10, x11, p1, 0, 1, 32\npmul p0, p0, p1, 1\npfree p1\n"
         << "dstore x10, x11, p0, 1, 0\npfree p4\npsync\n";
    const auto program = hpu::assemble_source(body.str());
    const auto execution = hpu::model::execute_instructions(program, spans, image);
    require(hpu::model::compare_instruction_memory(execution,
        {{"odd-root evaluations", ntt_output.span, spectrum}, {"schoolbook square", output.span, physical_squared}}).verified,
        "raw online instruction stream differs from independent polynomial or convolution oracle");
    require(execution.report.forward_boundary_conversions == 0
                && execution.report.inverse_boundary_conversions == 0
                && !execution.report.hardware_contract_verified,
            "raw transform boundaries or hardware evidence were reported incorrectly");
    std::vector<std::uint32_t> misplaced_post(degree);
    for (std::size_t p = 0; p < degree; ++p)
        misplaced_post[p] = post[p];
    auto wrong_spans = spans;
    wrong_spans[wrong_spans.size() - 2] =
        image.add("misplaced_post", hpu::model::pack_online_pmul_factors(misplaced_post, q), AllocationKind::twiddle).span;
    const auto wrong = hpu::model::execute_instructions(program, wrong_spans, image);
    require(wrong.report.completed()
                && !hpu::model::compare_instruction_memory(wrong, {{"square", output.span, physical_squared}}).verified,
            "instruction model silently repaired a post factor with the wrong coefficient order");
}

void stage_parameter_context_switches()
{
    struct Case {
        std::size_t degree;
        std::uint32_t modulus;
        std::uint32_t root_power;
        bool inverse;
        std::size_t stage;
        unsigned modulus_id;
    };
    const std::vector<Case> cases {
        {128, kModulus, 1, false, 0, 0},
        {1024, 4293918721U, 3, true, 4, 1},
        {256, 998244353U, 5, false, 3, 2},
        {4096, 4293918721U, 1, false, 11, 1},
        {1024, 4293918721U, 5, false, 4, 1},
        {1024, 4293918721U, 3, true, 0, 1},
        {128, kModulus, 3, true, 6, 0},
        {128, kModulus, 1, false, 0, 0},
    };
    std::vector<std::uint32_t> moduli;
    for (auto q : {kModulus, 4293918721U, 998244353U}) {
        const auto record = modulus_record(q);
        moduli.insert(moduli.end(), record.begin(), record.end());
    }
    HpuMemImage image(4096);
    const auto table = image.add("contexts/moduli", moduli, AllocationKind::modulus_table);
    std::vector<HpuMemSpan> spans{table.span};
    std::vector<hpu::model::InstructionMemoryExpectation> expected;
    std::ostringstream body;
    body << "dload x10, x11, p4, 1, 0, 32\n";
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& item = cases[index];
        std::uint32_t omega = 0;
        for (std::uint32_t base = 2; base < 128; ++base) {
            const auto candidate = hpu::model::pow_mod(base,
                (static_cast<std::uint64_t>(item.modulus) - 1) / item.degree, item.modulus);
            if (hpu::model::pow_mod(candidate, item.degree / 2, item.modulus) != 1) {
                omega = hpu::model::pow_mod(candidate, item.root_power, item.modulus);
                break;
            }
        }
        require(omega != 0, "context test could not select primitive root");
        hpu::model::HardwareNttModel model(item.degree, item.modulus, omega);
        const auto full = item.inverse ? model.inverse_twiddles().stages[item.stage]
                                       : model.forward_twiddles()[item.stage];
        std::vector<std::uint32_t> input(item.degree);
        for (std::size_t i = 0; i < item.degree; ++i)
            input[i] = static_cast<std::uint32_t>((std::uint64_t(i) * i + 31 * i + 7) % item.modulus);
        const auto prefix = "contexts/" + std::to_string(index);
        const auto data = image.add(prefix + "/input", input, AllocationKind::ciphertext);
        const auto parameters = image.add(prefix + "/parameters",
            hpu::model::pack_stage_twiddles(full, item.modulus, item.degree), AllocationKind::twiddle);
        require(parameters.span.line_count == 2, "context parameters used non-two-line DMA");
        const auto output = image.reserve(prefix + "/output", item.degree, AllocationKind::output);
        spans.insert(spans.end(), {data.span, parameters.span, output.span});
        expected.push_back({prefix, output.span,
            hpu::model::execute_ntt_stage(input, full, item.modulus, item.stage, item.inverse)});
        body << "pmodld " << item.modulus_id << "\ndload x10, x11, p0, 0, 0, 0\n"
             << "dload x10, x11, p1, 0, 0, 32\n"
             << (item.inverse ? "pintt" : "pntt") << " p0, p1, " << item.stage << ", 0, 0\n"
             << "pfree p1\ndstore x10, x11, p0, 1, 0\n";
    }
    body << "pfree p4\npsync\n";
    hpu::model::InstructionExecutionOptions options;
    options.ntt_contract = InstructionNttContract::legacy_raw_stage;
    const auto result = hpu::model::execute_instructions(hpu::assemble_source(body.str()), spans, image, options);
    require(hpu::model::compare_instruction_memory(result, expected).verified,
            "stage parameter state leaked across direction/modulus/root/stage context reloads");

    // Even all-one inverse stage 0 requires a real, resident src2 object.
    const auto missing = hpu::assemble_source(
        "dload x10, x11, p4, 1, 0, 32\npmodld 0\ndload x10, x11, p0, 0, 0, 0\npintt p0, p1, 0, 0, 0\npsync\n");
    require(!hpu::model::execute_instructions(missing, {table.span, spans[1]}, image, options).report.completed(),
            "PINTT stage 0 bypassed its parameter object dependency");
}

void packed_64_object_capacity()
{
    constexpr std::size_t degree = 8192;
    HpuMemImage image(1024);
    const auto table = image.add("capacity/moduli", modulus_record(kModulus), AllocationKind::modulus_table);
    const auto data = image.add("capacity/input", std::vector<std::uint32_t>(degree, 1), AllocationKind::ciphertext);
    const auto parameters = image.add("capacity/parameters",
        hpu::model::pack_stage_twiddles(std::vector<std::uint32_t>(degree / 2, 1), kModulus, degree),
        AllocationKind::twiddle);
    std::vector<HpuMemSpan> spans{table.span};
    std::ostringstream body;
    body << "dload x10, x11, p63, 1, 0, 32\npmodld 0\n";
    for (unsigned id = 0; id < 40; ++id) {
        body << "dload x10, x11, p" << id << ", 0, 0, 0\n";
        spans.push_back(data.span);
    }
    for (unsigned id = 40; id < 54; ++id) {
        body << "dload x10, x11, p" << id << ", 0, 0, 32\n";
        spans.push_back(parameters.span);
    }
    const auto resident = hpu::model::execute_instructions(
        hpu::assemble_source(body.str() + "psync\n"), spans, image);
    require(resident.report.completed() && resident.report.peak_resident_objects == 55
                && resident.report.peak_regular_banks == 5
                && resident.report.peak_regular_lines == 5120
                && resident.report.peak_small_lines == 32,
            "high-ID program did not fit forty normal objects and fourteen parameter objects");
    const auto no_workspace = hpu::model::execute_instructions(
        hpu::assemble_source(body.str() + "pntt p0, p40, 0, 0, 0\npsync\n"), spans, image);
    require(!no_workspace.report.completed()
                && no_workspace.report.message.find("physical bank interval") != std::string::npos,
            "full packed banks executed an NTT without a separate result region");
}

void ordinary_twiddle_load_contract()
{
    HpuMemImage image(16);
    const auto table = image.add("ordinary/moduli", modulus_record(kModulus),
                                 AllocationKind::modulus_table);
    const auto data = image.add("ordinary/data", std::vector<std::uint32_t>(128, 1),
                                AllocationKind::ciphertext);
    const auto fragment = image.add("ordinary/fragment", std::vector<std::uint32_t>(64, 1),
                                    AllocationKind::twiddle);
    const std::string loads =
        "dload x10, x11, p63, 1, 1, 0\npmodld 0\n"
        "dload x10, x11, p61, 0, 0, 0\n"
        "dload x10, x11, p62, 0, 1, 32\n";
    const std::vector<HpuMemSpan> spans{table.span, data.span, fragment.span};
    const auto loaded = hpu::model::execute_instructions(
        hpu::assemble_source(loads + "psync\n"), spans, image);
    require(loaded.report.completed(),
            "ordinary Bank5 load incorrectly enforced a removed TYPE3 length");
    const auto consumed = hpu::model::execute_instructions(
        hpu::assemble_source(loads + "pntt p61, p62, 0, 0, 0\npsync\n"), spans, image);
    require(!consumed.report.completed()
                && consumed.report.message.find("two-line") != std::string::npos,
            "PNTT failed to validate ordinary parameter length at use");
}

void configured_modulus_capacity()
{
    constexpr std::uint32_t q = 65537;
    HpuMemImage image(64);
    std::vector<std::uint32_t> contexts;
    for (unsigned id = 0; id < 256; ++id) {
        const auto record = modulus_record(q);
        contexts.insert(contexts.end(), record.begin(), record.end());
    }
    const auto table = image.add("expanded/moduli", contexts, AllocationKind::modulus_table);
    const std::vector<std::uint32_t> input(64, 17);
    const auto data = image.add("expanded/input", input, AllocationKind::ciphertext);
    const auto output = image.reserve("expanded/output", 64, AllocationKind::output);
    const auto program = hpu::assemble_source(
        "dload x10, x11, p63, 1, 0, 0\npsync\npmodld 255\n"
        "dload x10, x11, p62, 0, 0, 1\npmul p62, p62, 1\n"
        "dstore x10, x11, p62, 0, 0\npmul p62, p62, 1\n"
        "dstore x10, x11, p62, 1, 0\npsync\n");
    const std::vector<HpuMemSpan> spans{table.span, data.span, output.span, output.span};
    hpu::model::InstructionExecutionOptions options;
    options.mod_table_lines = 16;
    const auto execution = hpu::model::execute_instructions(program, spans, image, options);
    require(hpu::model::compare_instruction_memory(execution, {{"expanded/output", output.span, input}}).verified,
            "eight-bit modulus selection or high-ID REL ownership failed");
    require(!hpu::model::execute_instructions(program, spans, image).report.completed(),
            "oversized modulus table was accepted under the default reservation");
}

void online_pmul_templates_and_execution()
{
    for (std::size_t degree = 128; degree <= 65536; degree *= 2) {
        const std::uint32_t q = degree % 1024 == 0 ? 4293918721U : 65537U;
        const auto coefficient = hpu::model::inverse_mod_prime(static_cast<std::uint32_t>(degree), q);
        const auto ratio = hpu::model::inverse_mod_prime(31, q);
        const auto parameters = hpu::model::pack_online_pmul_parameters(coefficient, ratio, q, degree);
        require(parameters.size() == 128 && parameters[0] == coefficient && coefficient != 1,
                "online post template lost its N^-1 coefficient");
        const auto factors = hpu::model::expand_online_pmul_factors(parameters, q, degree);
        auto factor = coefficient;
        std::vector<std::uint32_t> input(degree), once(degree), twice(degree);
        for (std::size_t i = 0; i < degree; ++i) {
            require(factors[i] == factor, "online factor recurrence differs from c*r^j");
            input[i] = static_cast<std::uint32_t>((std::uint64_t(i) * i + 37 * i + 19) % q);
            once[i] = static_cast<std::uint32_t>(std::uint64_t(input[i]) * factor % q);
            twice[i] = static_cast<std::uint32_t>(std::uint64_t(once[i]) * factor % q);
            factor = static_cast<std::uint32_t>(std::uint64_t(factor) * ratio % q);
        }
        if (degree == 8192) {
            const auto step = hpu::model::pow_mod(ratio, 512, q);
            for (std::size_t t = 0; t < 4; ++t)
                require(parameters[71 + t] == step, "N8192 online PMUL needs four R entries");
            require(parameters[75] == 1, "online PMUL template padding changed");
        }
        HpuMemImage image(8192);
        const auto table = image.add("online/moduli", modulus_record(q), AllocationKind::modulus_table);
        const auto data = image.add("online/input", input, AllocationKind::ciphertext);
        const auto param = image.add("online/parameters", parameters, AllocationKind::twiddle);
        const auto separate = image.reserve("online/separate", degree, AllocationKind::output);
        const auto inplace = image.reserve("online/inplace", degree, AllocationKind::output);
        const auto reused = image.reserve("online/reused", degree, AllocationKind::output);
        const auto program = hpu::assemble_source(
            "dload x10, x11, p63, 1, 0, 32\npmodld 0\n"
            "dload x10, x11, p40, 0, 0, 0\n"
            "dload x10, x11, p62, 0, 1, 32\n"
            "pmul p41, p40, p62, 1\ndstore x10, x11, p41, 0, 0\n"
            "pmul p40, p40, p62, 1\ndstore x10, x11, p40, 1, 0\n"
            "pmul p41, p41, p62, 1\ndstore x10, x11, p41, 1, 0\n"
            "pfree p62\npfree p63\npsync\n");
        const std::vector<HpuMemSpan> spans{table.span, data.span, param.span,
            separate.span, inplace.span, reused.span};
        const auto execution = hpu::model::execute_instructions(program, spans, image);
        require(hpu::model::compare_instruction_memory(execution,
            {{"separate", separate.span, once}, {"inplace", inplace.span, once},
             {"reused", reused.span, twice}}).verified,
            "online PMUL failed independent/in-place/template-reuse coefficient oracle");
        require(execution.report.forward_boundary_conversions == 0
                    && execution.report.inverse_boundary_conversions == 0,
                "online PMUL introduced an NTT boundary permutation");

        // Execution must consume DMA parameters rather than regenerate a root
        // from metadata: a changed U lane affects the corresponding coefficients.
        auto changed = parameters;
        changed[17] = (changed[17] + 1U) % q;
        auto wrong_spans = spans;
        wrong_spans[2] = image.add("online/changed", changed, AllocationKind::twiddle).span;
        const auto wrong = hpu::model::execute_instructions(program, wrong_spans, image);
        require(wrong.report.completed()
                    && !hpu::model::compare_instruction_memory(wrong,
                        {{"changed", separate.span, once}}).verified,
                "online PMUL ignored the supplied U/V/R payload");
    }

    // Zero is a legal canonical factor, unlike the nonzero NTT root contract.
    const auto zero = hpu::model::pack_online_pmul_parameters(0, 0, 65537, 128);
    require(hpu::model::expand_online_pmul_factors(zero, 65537, 128)
                == std::vector<std::uint32_t>(128, 0),
            "online PMUL rejected canonical zero factors");
}

void online_pmul_operand_validation()
{
    HpuMemImage image(64);
    const auto table = image.add("online-invalid/moduli", modulus_record(65537), AllocationKind::modulus_table);
    const auto data = image.add("online-invalid/input", std::vector<std::uint32_t>(128, 1), AllocationKind::ciphertext);
    const auto parameters = image.add("online-invalid/parameters",
        hpu::model::pack_online_pmul_parameters(1, 3, 65537, 128), AllocationKind::twiddle);
    const auto short_param = image.add("online-invalid/short", std::vector<std::uint32_t>(64, 1), AllocationKind::twiddle);
    const auto long_param = image.add("online-invalid/long", std::vector<std::uint32_t>(192, 1), AllocationKind::twiddle);
    const auto short_data = image.add("online-invalid/short-data", std::vector<std::uint32_t>(64, 1), AllocationKind::ciphertext);
    const auto uneven_data = image.add("online-invalid/uneven-data", std::vector<std::uint32_t>(192, 1), AllocationKind::ciphertext);
    const std::string loads = "dload x10, x11, p63, 1, 0, 32\npmodld 0\n"
        "dload x10, x11, p40, 0, 0, 0\ndload x10, x11, p62, 0, 1, 32\n";
    auto rejects = [&](const std::string& command, HpuMemSpan source, HpuMemSpan param) {
        bool rejected = false;
        try {
            const auto execution = hpu::model::execute_instructions(
                hpu::assemble_source(loads + command + "\npsync\n"), {table.span, source, param}, image);
            rejected = !execution.report.completed();
        } catch (const std::runtime_error&) {
            // ID aliasing is statically illegal and rejected by the encoder.
            rejected = true;
        }
        require(rejected, "online PMUL accepted invalid operands");
    };
    rejects("pmul p41, p40, p62, 1", data.span, short_param.span);
    rejects("pmul p41, p40, p62, 1", data.span, long_param.span);
    rejects("pmul p41, p40, p62, 1", short_data.span, parameters.span);
    rejects("pmul p41, p40, p62, 1", uneven_data.span, parameters.span);
    rejects("pmul p62, p40, p62, 1", data.span, parameters.span);
    rejects("pmul p41, p40, p40, 1", data.span, parameters.span);
    rejects("pmul p41, p40, p61, 1", data.span, parameters.span);
    rejects("pmul p41, p40, p63, 1", data.span, parameters.span);
    rejects("pmul p41, p63, p62, 1", data.span, parameters.span);
    rejects("pmul p63, p40, p62, 1", data.span, parameters.span);

    // I=1 owns all imm8 bits. 64, 192 and 255 must remain ordinary scalar
    // PMULs even though the same bit positions encode object-mode selection.
    const auto output = image.reserve("online-invalid/scalar-output", 128, AllocationKind::output);
    for (unsigned immediate : {64U, 192U, 255U}) {
        const auto program = hpu::assemble_source(loads + "pmul p40, p40, "
            + std::to_string(immediate) + "\ndstore x10, x11, p40, 1, 0\npsync\n");
        const auto execution = hpu::model::execute_instructions(program,
            {table.span, data.span, parameters.span, output.span}, image);
        require(hpu::model::compare_instruction_memory(execution,
            {{"scalar", output.span, std::vector<std::uint32_t>(128, immediate)}}).verified,
            "full imm8 PMUL was misdecoded as online/object mode");
    }
}

} // namespace

int main()
{
    try {
        arithmetic_and_dma();
        supplied_stages(128);
        supplied_stages(256);
        supplied_stages(1024);
        pre_twist_is_pointwise();
        forward_twist_layout_contract();
        inverse_boundary_contract();
        raw_online_negacyclic_convolution(128);
        raw_online_negacyclic_convolution(256);
        raw_online_negacyclic_convolution(1024);
        stage_parameter_context_switches();
        packed_64_object_capacity();
        ordinary_twiddle_load_contract();
        configured_modulus_capacity();
        online_pmul_templates_and_execution();
        online_pmul_operand_validation();
        std::cout << "Instruction execution tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Instruction execution test failed: " << error.what() << '\n';
        return 1;
    }
}
