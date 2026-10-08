#include "hpu/model/instruction_executor.hpp"

#include "hpu/model/hardware_ntt.hpp"
#include "encoder.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace hpu::model {
namespace {

using hpu::Mnemonic;
using hpu::runtime::HpuMemSpan;
constexpr std::size_t kLineWords = hpu::runtime::kHpuMemLineWords;

class UnsupportedContract : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

hpu::Instruction decode_word(std::uint32_t word)
{
    hpu::Instruction result;
    const auto opcode = word & 0x7fU;
    if (opcode == 0x2bU) {
        const bool store = ((word >> 12U) & 1U) != 0;
        result.mnemonic = store ? Mnemonic::kDstore : Mnemonic::kDload;
        result.obj_id = static_cast<std::uint8_t>((word >> 25U) & 7U);
        result.rs1 = static_cast<int>((word >> 15U) & 31U);
        result.rs2 = static_cast<int>((word >> 20U) & 31U);
        const auto operation = static_cast<std::uint8_t>((word >> 13U) & 3U);
        result.type = store ? operation >> 1U : operation;
        result.dma_flag = static_cast<std::uint8_t>((word >> 7U) & 1U);
    } else if (opcode == 0x5bU) {
        const auto operation = word >> 28U;
        switch (operation) {
        case 0: result.mnemonic = Mnemonic::kPadd; break;
        case 1: result.mnemonic = Mnemonic::kPsub; break;
        case 2: result.mnemonic = Mnemonic::kPmul; break;
        case 3: result.mnemonic = Mnemonic::kPmac; break;
        case 4: result.mnemonic = Mnemonic::kPntt; break;
        case 5: result.mnemonic = Mnemonic::kPintt; break;
        case 6: result.mnemonic = Mnemonic::kPmodld; break;
        case 7: result.mnemonic = Mnemonic::kPsync; break;
        case 8: result.mnemonic = Mnemonic::kPfree; break;
        default: throw std::invalid_argument("unknown custom2 opcode");
        }
        if (operation < 4) {
            result.pdst = static_cast<int>((word >> 25U) & 7U);
            result.psrc1 = static_cast<int>((word >> 22U) & 7U);
            result.mode = static_cast<std::uint8_t>((word >> 8U) & 3U);
            result.flag = static_cast<std::uint8_t>((word >> 7U) & 1U);
            const auto op2 = static_cast<int>((word >> 14U) & 255U);
            if ((result.mode & 1U) != 0) result.imm8 = op2;
            else result.psrc2 = op2;
        } else if (operation < 6) {
            result.pdst = static_cast<int>((word >> 25U) & 7U);
            result.psrc1 = static_cast<int>((word >> 14U) & 255U);
            result.idx0 = static_cast<int>((word >> 10U) & 15U);
            result.mode = static_cast<std::uint8_t>((word >> 8U) & 3U);
            result.flag = static_cast<std::uint8_t>((word >> 7U) & 1U);
        } else if (operation == 6) {
            result.mod_id = static_cast<int>((word >> 14U) & 255U);
        } else if (operation == 8) {
            result.idx0 = static_cast<int>((word >> 22U) & 7U);
            result.idx1 = 0;
        }
    } else {
        throw std::invalid_argument("word is not an HPU custom1/custom2 instruction");
    }
    // Reject reserved bits and malformed duplicated STG operands. This check
    // uses independently decoded fields, never the companion metadata.
    if (hpu::encode_instruction(result) != word)
        throw std::invalid_argument("instruction contains invalid/reserved fields");
    return result;
}

std::size_t span_offset(HpuMemSpan span, std::size_t memory_words)
{
    const auto maximum = std::numeric_limits<std::size_t>::max() / kLineWords;
    if (span.line_count == 0 || span.line_offset > maximum || span.line_count > maximum)
        throw std::invalid_argument("invalid DMA line span");
    const auto offset = static_cast<std::size_t>(span.line_offset) * kLineWords;
    const auto length = static_cast<std::size_t>(span.line_count) * kLineWords;
    if (offset > memory_words || length > memory_words - offset)
        throw std::invalid_argument("DMA span exceeds actual image words");
    return offset;
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

void require_canonical(const std::vector<std::uint32_t>& words, std::uint32_t modulus,
                       const char* role)
{
    if (std::any_of(words.begin(), words.end(),
        [&](std::uint32_t word) { return word >= modulus; }))
        throw std::invalid_argument(std::string(role) + " contains a noncanonical residue");
}

struct ResidentObject {
    std::vector<std::uint32_t> words;
    bool small_bank = false;
    std::uint8_t type = 1;
};

} // namespace

const char* instruction_ntt_contract_name(InstructionNttContract contract) noexcept
{
    switch (contract) {
    case InstructionNttContract::legacy_raw_stage:
        return "legacy_raw_stage";
    case InstructionNttContract::natural_coefficient_boundaries:
        return "natural_coefficient_boundaries";
    }
    return "unknown";
}

InstructionExecutionResult execute_instructions(
    const std::vector<hpu::EncodedInstruction>& instructions,
    const std::vector<HpuMemSpan>& ordered_dma_spans,
    const hpu::runtime::HpuMemImage& image,
    InstructionExecutionOptions options)
{
    InstructionExecutionResult result;
    result.words = image.words();
    auto& report = result.report;
    report.ntt_contract = options.ntt_contract;
    std::array<std::optional<ResidentObject>, 8> objects;
    int modulus_object = -1;
    std::optional<std::uint32_t> modulus;
    std::size_t dma_index = 0;
    bool finished = false;

    auto require_object = [&](int id) -> ResidentObject& {
        if (id < 0 || id >= static_cast<int>(objects.size()) || !objects[id])
            throw std::invalid_argument("object p" + std::to_string(id) + " is not resident");
        return *objects[id];
    };
    auto account_banks = [&](std::size_t extra = 0) {
        std::size_t resident = 0;
        std::size_t regular = extra;
        for (const auto& object : objects) {
            if (!object) continue;
            ++resident;
            if (!object->small_bank) ++regular;
        }
        report.peak_resident_objects = std::max(report.peak_resident_objects, resident);
        report.peak_regular_banks = std::max(report.peak_regular_banks, regular);
        if (regular > options.regular_bank_count)
            throw std::invalid_argument("insufficient regular banks including transform output");
    };
    auto release = [&](int id) {
        require_object(id);
        objects[id].reset();
        if (modulus_object == id) modulus_object = -1;
    };

    try {
        if (options.ntt_contract != InstructionNttContract::legacy_raw_stage
            && options.ntt_contract != InstructionNttContract::natural_coefficient_boundaries)
            throw UnsupportedContract("unknown instruction NTT software contract");
        if (instructions.empty() || options.regular_bank_count == 0
            || options.regular_bank_lines == 0)
            throw std::invalid_argument("empty program or invalid bank limits");
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            report.instruction_index = index;
            const auto instruction = decode_word(instructions[index].word);
            switch (instruction.mnemonic) {
            case Mnemonic::kDload:
            case Mnemonic::kDstore: {
                if (instruction.rs1 != 10 || instruction.rs2 != 11)
                    throw UnsupportedContract("DMA requires frozen x10/x11 span ABI");
                if (dma_index >= ordered_dma_spans.size())
                    throw std::invalid_argument("missing resolved DMA span");
                const auto span = ordered_dma_spans[dma_index++];
                const auto offset = span_offset(span, result.words.size());
                const auto length = static_cast<std::size_t>(span.line_count) * kLineWords;
                const auto id = instruction.obj_id;
                if (instruction.mnemonic == Mnemonic::kDload) {
                    if (objects[id]) throw std::invalid_argument("DLOAD overwrites a resident object");
                    const bool small = instruction.dma_flag != 0;
                    if (span.line_count > (small ? 32 : options.regular_bank_lines))
                        throw std::invalid_argument("DLOAD object exceeds bank capacity");
                    if (small && instruction.type != 2)
                        throw UnsupportedContract("small-bank non-modulus objects are unsupported");
                    if (!small && instruction.type == 2)
                        throw UnsupportedContract("modulus table requires small bank");
                    if (small && modulus_object >= 0)
                        throw std::invalid_argument("multiple resident small-bank modulus tables");
                    objects[id] = ResidentObject{
                        std::vector<std::uint32_t>(result.words.begin() + offset,
                                                  result.words.begin() + offset + length),
                        small, instruction.type};
                    if (small) modulus_object = id;
                    report.loaded_words += length;
                    account_banks();
                } else {
                    const auto& source = require_object(id);
                    if (source.words.size() != length)
                        throw std::invalid_argument("DSTORE span differs from resident object length");
                    for (const auto& allocation : image.allocations()) {
                        if (!allocation.read_only) continue;
                        const auto a_begin = allocation.span.line_offset;
                        const auto a_end = a_begin + allocation.span.line_count;
                        if (span.line_offset < a_end && a_begin < span.line_offset + span.line_count)
                            throw std::invalid_argument("DSTORE overwrites read-only allocation " + allocation.id);
                    }
                    std::copy(source.words.begin(), source.words.end(), result.words.begin() + offset);
                    report.stored_words += length;
                    release(id); // Current executable ABI releases for both REL values.
                }
                break;
            }
            case Mnemonic::kPmodld: {
                if (modulus_object < 0) throw std::invalid_argument("PMODLD has no modulus table");
                if (instruction.mod_id > 63)
                    throw UnsupportedContract("MOD_ID exceeds current 0..63 application ABI");
                const auto& table = require_object(modulus_object).words;
                const auto offset = static_cast<std::size_t>(instruction.mod_id) * 4;
                if (offset + 4 > table.size()) throw std::invalid_argument("MOD_ID exceeds loaded table");
                const auto q = table[offset];
                if (q < 65537) throw std::invalid_argument("modulus is outside HPU PE range");
                const auto maximum = std::numeric_limits<std::uint64_t>::max();
                const auto mu = maximum / q + ((maximum % q) + 1 >= q ? 1 : 0);
                const auto stored_mu = table[offset + 1]
                    | (static_cast<std::uint64_t>(table[offset + 2] & 0xffffU) << 32U);
                if (table[offset + 3] != 0 || (table[offset + 2] & 0xffff0000U) != 0
                    || stored_mu != mu)
                    throw std::invalid_argument("modulus table has invalid Barrett mu/reserved words");
                modulus = q;
                break;
            }
            case Mnemonic::kPadd:
            case Mnemonic::kPsub:
            case Mnemonic::kPmul:
            case Mnemonic::kPmac: {
                if (!modulus) throw std::invalid_argument("arithmetic has no active modulus");
                if (instruction.flag != 0 || (instruction.mode & 2U) != 0)
                    throw UnsupportedContract("arithmetic mode/flag is not modeled");
                const auto& left = require_object(instruction.psrc1);
                if (left.small_bank) throw std::invalid_argument("arithmetic reads small-bank object");
                const auto* right = instruction.imm8 < 0 ? &require_object(instruction.psrc2) : nullptr;
                if (right && (right->small_bank || right->words.size() != left.words.size()))
                    throw std::invalid_argument("arithmetic source layouts/lengths differ");
                const bool mac = instruction.mnemonic == Mnemonic::kPmac;
                if (instruction.mnemonic == Mnemonic::kPadd || instruction.mnemonic == Mnemonic::kPsub) {
                    require_canonical(left.words, *modulus, "add/sub left operand");
                    require_canonical(right->words, *modulus, "add/sub right operand");
                }
                const ResidentObject* accumulator = mac ? &require_object(instruction.pdst) : nullptr;
                if (accumulator && (accumulator->small_bank || accumulator->words.size() != left.words.size()))
                    throw std::invalid_argument("PMAC accumulator layout/length differs");
                if (accumulator) require_canonical(accumulator->words, *modulus, "PMAC accumulator");
                if (objects[instruction.pdst] && (objects[instruction.pdst]->small_bank
                    || objects[instruction.pdst]->words.size() != left.words.size()))
                    throw std::invalid_argument("arithmetic destination layout/length differs");
                std::vector<std::uint32_t> output(left.words.size());
                for (std::size_t i = 0; i < output.size(); ++i) {
                    const std::uint64_t a = left.words[i];
                    const std::uint64_t b = right ? right->words[i] : instruction.imm8;
                    std::uint64_t value;
                    if (instruction.mnemonic == Mnemonic::kPadd) value = a + b;
                    else if (instruction.mnemonic == Mnemonic::kPsub) value = a + *modulus - b;
                    else {
                        value = (a * b) % *modulus;
                        if (mac) value += accumulator->words[i];
                    }
                    output[i] = static_cast<std::uint32_t>(value % *modulus);
                }
                objects[instruction.pdst] = ResidentObject{std::move(output), false, 1};
                account_banks();
                break;
            }
            case Mnemonic::kPntt:
            case Mnemonic::kPintt: {
                if (!modulus) throw std::invalid_argument("transform has no active modulus");
                if (instruction.mode != 0 || instruction.flag != 0)
                    throw UnsupportedContract("transform mode/flag is not modeled");
                const auto& data = require_object(instruction.pdst);
                const auto& twiddle = require_object(instruction.psrc1);
                if (data.small_bank || twiddle.small_bank || data.words.size() != 2 * twiddle.words.size())
                    throw std::invalid_argument("transform data/twiddle lengths or banks differ");
                const auto degree = data.words.size();
                if (degree < 128 || degree > 65536 || (degree & (degree - 1)) != 0)
                    throw std::invalid_argument("transform degree must be a power of two in [128, 65536]");
                account_banks(1); // Out-of-place stage output, in addition to resident inputs.
                auto input = data.words;
                const bool inverse = instruction.mnemonic == Mnemonic::kPintt;
                if (!inverse && instruction.idx0 == 0 && options.ntt_contract
                    == InstructionNttContract::natural_coefficient_boundaries) {
                    for (std::size_t i = 0; i < input.size(); ++i)
                        input[i] = data.words[bit_reverse(i, input.size())];
                    ++report.forward_boundary_conversions;
                }
                auto output = execute_ntt_stage(
                    input, twiddle.words, *modulus,
                    static_cast<std::size_t>(instruction.idx0), inverse);
                if (inverse && options.ntt_contract
                    == InstructionNttContract::natural_coefficient_boundaries
                    && (std::size_t{1} << (instruction.idx0 + 1)) == degree) {
                    for (std::size_t i = 0; i < output.size(); ++i)
                        input[i] = output[bit_reverse(i, degree)];
                    output = std::move(input);
                    ++report.inverse_boundary_conversions;
                }
                objects[instruction.pdst]->words = std::move(output);
                break;
            }
            case Mnemonic::kPfree:
                release(instruction.idx0);
                break;
            case Mnemonic::kPsync:
                if (index + 1 != instructions.size())
                    throw std::invalid_argument("PSYNC must be the terminal instruction");
                for (const auto& object : objects)
                    if (object) throw std::invalid_argument("resident object survives terminal PSYNC");
                finished = true;
                break;
            }
            ++report.executed_instructions;
        }
        if (!finished) throw std::invalid_argument("missing terminal PSYNC");
        if (dma_index != ordered_dma_spans.size())
            throw std::invalid_argument("extra resolved DMA spans");
        report.status = InstructionExecutionStatus::complete;
        report.message = std::string("Encoded words and concrete DMA spans executed under software contract ")
            + instruction_ntt_contract_name(report.ntt_contract)
            + "; oracle comparison and hardware validation are separate";
    } catch (const UnsupportedContract& error) {
        report.status = InstructionExecutionStatus::unsupported_contract;
        report.message = error.what();
    } catch (const std::exception& error) {
        report.status = InstructionExecutionStatus::invalid_program;
        report.message = error.what();
    }
    return result;
}

InstructionMemoryComparison compare_instruction_memory(
    const InstructionExecutionResult& execution,
    const std::vector<InstructionMemoryExpectation>& expectations)
{
    InstructionMemoryComparison result;
    if (!execution.report.completed()) {
        result.message = "Instruction execution incomplete at instruction "
            + std::to_string(execution.report.instruction_index) + ": " + execution.report.message;
        return result;
    }
    if (expectations.empty()) {
        result.message = "No oracle output expectations supplied";
        return result;
    }
    try {
        for (const auto& expectation : expectations) {
            result.object_id = expectation.id;
            const auto offset = span_offset(expectation.span, execution.words.size());
            const auto capacity = static_cast<std::size_t>(expectation.span.line_count) * kLineWords;
            if (expectation.words.empty() || expectation.words.size() > capacity)
                throw std::invalid_argument("oracle expectation has invalid payload length");
            for (std::size_t i = 0; i < expectation.words.size(); ++i) {
                ++result.compared_words;
                if (expectation.words[i] == execution.words[offset + i]) continue;
                result.object_word_index = i;
                result.expected = expectation.words[i];
                result.actual = execution.words[offset + i];
                result.message = "Instruction result differs from oracle at " + expectation.id
                    + " word " + std::to_string(i) + ": expected " + std::to_string(result.expected)
                    + ", actual " + std::to_string(result.actual);
                return result;
            }
        }
    } catch (const std::exception& error) {
        result.message = error.what();
        return result;
    }
    result.verified = true;
    result.object_id.clear();
    result.message = "All supplied oracle payload words match encoded instruction execution";
    return result;
}

} // namespace hpu::model
