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
        "dload x10, x11, p4, 2, 1\n"
        "pmodld 0\n"
        "dload x10, x11, p0, 1, 0\n"
        "pmul p0, p0, 1\n"
        "dload x10, x11, p1, 1, 0\n"
        "pmul p1, p1, 1\n"
        "padd p2, p0, p1\n"
        "psub p2, p2, p1\n"
        "pmac p2, p0, p1\n"
        "pmul p2, p2, 7\n"
        "dstore x10, x11, p2, 0\n"
        "pfree p0\npfree p1\npfree p4\npsync\n");
    const std::vector<HpuMemSpan> spans{table.span, a.span, b.span, output.span};
    const auto result = hpu::model::execute_instructions(instructions, spans, image);
    require(result.report.completed(), "encoded arithmetic execution failed");
    require(result.report.ntt_contract == InstructionNttContract::natural_coefficient_boundaries
                && result.report.forward_boundary_conversions == 0
                && !result.report.hardware_contract_verified,
            "software completion was confused with hardware contract verification");
    require(result.report.peak_regular_banks == 3 && result.report.stored_words == 64,
            "arithmetic resource accounting failed");
    const std::vector<hpu::model::InstructionMemoryExpectation> expectations{{"output", output.span, expected}};
    require(hpu::model::compare_instruction_memory(result, expectations).verified,
            "encoded arithmetic differs from independent oracle");

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
    require(!release_result.report.completed() && release_result.report.instruction_index == 11,
            "DSTORE failed to release its source for REL=0");

    auto reserved_word = instructions;
    reserved_word[3].word |= 1U << 10U;
    require(!hpu::model::execute_instructions(reserved_word, spans, image).report.completed(),
            "reserved instruction bits were ignored");
    auto unsupported_mode = instructions;
    unsupported_mode[3].word |= 1U << 9U;
    require(hpu::model::execute_instructions(unsupported_mode, spans, image).report.status
                == InstructionExecutionStatus::unsupported_contract,
            "unsupported mode was not distinguished from invalid program");
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
    const auto data = image.add("input", input, AllocationKind::ciphertext);
    std::vector<HpuMemSpan> spans{table.span, data.span};
    std::ostringstream body;
    body << "dload x10, x11, p4, 2, 1\npmodld 0\ndload x10, x11, p0, 1, 0\n";
    for (std::size_t stage = 0; stage < forward_tables.size(); ++stage) {
        const auto twiddle = image.add("forward/" + std::to_string(stage), forward_tables[stage], AllocationKind::twiddle);
        spans.push_back(twiddle.span);
        body << "dload x10, x11, p1, 1, 0\npntt p0, p1, " << stage << ", 0, 0\npfree p1\n";
    }
    const auto output = image.reserve("output", degree, AllocationKind::output);
    spans.push_back(output.span);
    body << "dstore x10, x11, p0, 1\npfree p4\npsync\n";
    const auto instructions = hpu::assemble_source(body.str());
    const auto execution = hpu::model::execute_instructions(instructions, spans, image);
    require(execution.report.completed(), "supplied forward instruction chain failed");
    require(execution.report.forward_boundary_conversions == 1,
            "natural forward contract did not report its boundary conversion");
    require(execution.report.peak_regular_banks == 3, "transform output bank was not accounted");
    require(hpu::model::compare_instruction_memory(execution, {{"output", output.span, expected_forward}}).verified,
            "encoded stages ignored actual supplied twiddles/input permutation");

    auto changed_spans = spans;
    auto changed_twiddles = forward_tables.front();
    changed_twiddles[0] = 2;
    const auto changed = image.add("changed/first", changed_twiddles, AllocationKind::twiddle);
    changed_spans[2] = changed.span;
    const auto changed_execution = hpu::model::execute_instructions(instructions, changed_spans, image);
    require(changed_execution.report.completed()
                && !hpu::model::compare_instruction_memory(changed_execution, {{"output", output.span, expected_forward}}).verified,
            "corrupt twiddle DMA payload did not change instruction result");
    hpu::model::InstructionExecutionOptions constrained;
    constrained.regular_bank_count = 2;
    const auto no_output_bank = hpu::model::execute_instructions(instructions, spans, image, constrained);
    require(!no_output_bank.report.completed() && no_output_bank.report.peak_regular_banks == 3,
            "NTT ran without an out-of-place output bank");

    // A separate inverse instruction stream executes raw stages and explicit
    // post multiplication. Its physical result is compared without an invented
    // software-only natural-order permutation.
    const auto inverse_input = image.add("inverse/input", expected_forward, AllocationKind::ciphertext);
    std::vector<HpuMemSpan> inverse_spans{table.span, inverse_input.span};
    std::ostringstream inverse_body;
    inverse_body << "dload x10, x11, p4, 2, 1\npmodld 0\ndload x10, x11, p0, 1, 0\n";
    for (std::size_t stage = 0; stage < inverse_tables.stages.size(); ++stage) {
        const auto twiddle = image.add("inverse/" + std::to_string(stage), inverse_tables.stages[stage], AllocationKind::twiddle);
        inverse_spans.push_back(twiddle.span);
        inverse_body << "dload x10, x11, p1, 1, 0\npintt p0, p1, " << stage << ", 0, 0\npfree p1\n";
    }
    const auto post = image.add("inverse/post", inverse_tables.post_scale, AllocationKind::twiddle);
    inverse_spans.push_back(post.span);
    inverse_spans.push_back(output.span);
    inverse_body << "dload x10, x11, p1, 1, 0\npmul p0, p0, p1\npfree p1\n"
                 << "dstore x10, x11, p0, 1\npfree p4\npsync\n";
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
        "dload x10, x11, p4, 2, 1\npmodld 0\ndload x10, x11, p0, 1, 0\n"
        "dload x10, x11, p1, 1, 0\npmul p0, p0, p1\npfree p1\n"
        "dstore x10, x11, p0, 1\npfree p4\npsync\n");
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
        body << "dload x10, x11, p4, 2, 1\npmodld 0\n"
             << "dload x10, x11, p0, 1, 0\ndload x10, x11, p1, 1, 0\n"
             << "pmul p0, p0, p1\npfree p1\n";
        for (std::size_t s = 0; s < stages.size(); ++s) {
            spans.push_back(image.add("stage/" + std::to_string(s), stages[s], AllocationKind::twiddle).span);
            body << "dload x10, x11, p1, 1, 0\npntt p0, p1, " << s << ", 0, 0\npfree p1\n";
        }
        const auto output = image.reserve("output", degree, AllocationKind::output);
        spans.push_back(output.span);
        body << "dstore x10, x11, p0, 1\npfree p4\npsync\n";
        hpu::model::InstructionExecutionOptions options;
        options.ntt_contract = contract;
        const auto result = hpu::model::execute_instructions(hpu::assemble_source(body.str()), spans, image, options);
        require(result.report.completed(), "forward twist contract execution failed");
        require(result.report.ntt_contract == contract && !result.report.hardware_contract_verified,
                "contract selection or hardware status was not preserved");
        require(result.report.forward_boundary_conversions
                    == (contract != InstructionNttContract::legacy_raw_stage ? 1U : 0U),
                "forward contract applied an unreported boundary conversion");
        require(result.report.message.find(hpu::model::instruction_ntt_contract_name(contract))
                    != std::string::npos,
                "execution report omitted the software contract name");
        const auto begin = output.span.line_offset * hpu::runtime::kHpuMemLineWords;
        return std::vector<std::uint32_t>(result.words.begin() + begin, result.words.begin() + begin + degree);
    };
    require(run(natural, natural_twist, InstructionNttContract::natural_coefficient_boundaries) == expected,
            "natural coefficient boundaries disagree with the independent monomial oracle");
    require(run(physical, physical_twist, InstructionNttContract::legacy_raw_stage) == expected,
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
    // the implementation under test. The legacy raw profile retains the raw
    // inverse result c[BR(p)]; the mathematical API returns c[p].
    constexpr std::size_t degree = 128;
    constexpr std::uint32_t q = 65537;
    constexpr std::uint32_t psi = 282;
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

    HpuMemImage image(128);
    const auto moduli = image.add("moduli", modulus_record(q), AllocationKind::modulus_table);
    const auto input = image.add("input", spectrum, AllocationKind::ciphertext);
    std::vector<HpuMemSpan> spans{moduli.span, input.span};
    std::ostringstream body;
    body << "dload x10, x11, p4, 2, 1\npmodld 0\ndload x10, x11, p0, 1, 0\n";
    for (std::size_t s = 0; s < tables.stages.size(); ++s) {
        spans.push_back(image.add("inverse/" + std::to_string(s), tables.stages[s], AllocationKind::twiddle).span);
        body << "dload x10, x11, p1, 1, 0\npintt p0, p1, " << s << ", 0, 0\npfree p1\n";
    }
    spans.push_back(image.add("post", post, AllocationKind::twiddle).span);
    const auto output = image.reserve("output", degree, AllocationKind::output);
    spans.push_back(output.span);
    body << "dload x10, x11, p1, 1, 0\npmul p0, p0, p1\npfree p1\n"
         << "dstore x10, x11, p0, 1\npfree p4\npsync\n";
    const auto instructions = hpu::assemble_source(body.str());
    hpu::model::InstructionExecutionOptions options;
    options.ntt_contract = InstructionNttContract::legacy_raw_stage;
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

void natural_negacyclic_convolution(std::size_t degree)
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
    const auto moduli = image.add("moduli", modulus_record(q), AllocationKind::modulus_table);
    const auto data = image.add("input", input, AllocationKind::ciphertext);
    const auto pre_factor = image.add("pre", pre, AllocationKind::twiddle);
    const auto ntt_output = image.reserve("ntt/output", degree, AllocationKind::output);
    const auto output = image.reserve("square/output", degree, AllocationKind::output);
    std::vector<HpuMemSpan> spans{moduli.span, data.span, pre_factor.span};
    std::ostringstream body;
    body << "dload x10, x11, p4, 2, 1\npmodld 0\ndload x10, x11, p0, 1, 0\n"
         << "dload x10, x11, p1, 1, 0\npmul p0, p0, p1\npfree p1\n";
    for (std::size_t s = 0; s < forward.size(); ++s) {
        spans.push_back(image.add("ntt/stage/" + std::to_string(s), forward[s], AllocationKind::twiddle).span);
        body << "dload x10, x11, p1, 1, 0\npntt p0, p1, " << s << ", 0, 0\npfree p1\n";
    }
    spans.push_back(ntt_output.span);
    spans.push_back(ntt_output.span);
    body << "dstore x10, x11, p0, 1\ndload x10, x11, p0, 1, 0\npmul p0, p0, p0\n";
    for (std::size_t s = 0; s < inverse.stages.size(); ++s) {
        spans.push_back(image.add("intt/stage/" + std::to_string(s), inverse.stages[s], AllocationKind::twiddle).span);
        body << "dload x10, x11, p1, 1, 0\npintt p0, p1, " << s << ", 0, 0\npfree p1\n";
    }
    spans.push_back(image.add("post", post, AllocationKind::twiddle).span);
    spans.push_back(output.span);
    body << "dload x10, x11, p1, 1, 0\npmul p0, p0, p1\npfree p1\n"
         << "dstore x10, x11, p0, 1\npfree p4\npsync\n";
    const auto program = hpu::assemble_source(body.str());
    const auto execution = hpu::model::execute_instructions(program, spans, image);
    require(hpu::model::compare_instruction_memory(execution,
        {{"odd-root evaluations", ntt_output.span, spectrum}, {"schoolbook square", output.span, squared}}).verified,
        "natural boundary instruction stream differs from independent polynomial or convolution oracle");
    require(execution.report.forward_boundary_conversions == 1
                && execution.report.inverse_boundary_conversions == 1
                && !execution.report.hardware_contract_verified,
            "natural transform boundaries or hardware evidence were reported incorrectly");
    std::vector<std::uint32_t> misplaced_post(degree);
    for (std::size_t p = 0; p < degree; ++p)
        misplaced_post[p] = post[bit_reverse(p, degree)];
    auto wrong_spans = spans;
    wrong_spans[wrong_spans.size() - 2] =
        image.add("misplaced_post", misplaced_post, AllocationKind::twiddle).span;
    const auto wrong = hpu::model::execute_instructions(program, wrong_spans, image);
    require(wrong.report.completed()
                && !hpu::model::compare_instruction_memory(wrong, {{"square", output.span, squared}}).verified,
            "instruction model silently repaired a post factor with the wrong coefficient order");
}

} // namespace

int main()
{
    try {
        arithmetic_and_dma();
        supplied_stages(128);
        supplied_stages(256);
        pre_twist_is_pointwise();
        forward_twist_layout_contract();
        inverse_boundary_contract();
        natural_negacyclic_convolution(128);
        natural_negacyclic_convolution(256);
        std::cout << "Instruction execution tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Instruction execution test failed: " << error.what() << '\n';
        return 1;
    }
}
