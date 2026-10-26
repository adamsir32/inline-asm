#include "executable.hpp"

#include <cctype>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace hpu {
namespace {

// Current target ABI: PNTT/PINTT src2 carries K=8 online parameters,
// independently of the polynomial degree. Keep encode independent of models.
constexpr std::uint64_t kStageParameterLines = 2;
constexpr std::uint64_t kMinNttPolynomialLines = 2;
constexpr std::uint64_t kMaxNttPolynomialLines = 1024;

bool is_online_pmul(const Instruction& instruction) {
    return instruction.mnemonic == Mnemonic::kPmul
        && instruction.imm8 < 0 && instruction.mode == 1;
}

std::string hex_word(std::uint32_t word) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0') << word;
    return output.str();
}

std::string csv_field(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char ch : value) {
        if (ch == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

void validate_fixed_spans(const std::vector<EncodedInstruction>& encoded,
                         std::uint64_t capacity,
                         const std::vector<ExecutableDmaSpan>& spans,
                         ExecutableTwiddleFormat twiddle_format,
                         std::uint32_t mod_table_lines) {
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    if (capacity == 0 || capacity > maximum)
        throw std::invalid_argument("fixed executable capacity exceeds the uint32 span ABI");
    if (collect_dma_relocations(encoded).size() != spans.size())
        throw std::invalid_argument("fixed executable DMA span count differs from instructions");
    if (mod_table_lines == 0 || mod_table_lines > 16)
        throw std::invalid_argument("modulus table reservation must be 1..16 lines");
    std::array<std::uint64_t, 64> lengths{};
    std::array<std::uint8_t, 64> types{};
    int modulus_table_object = -1;
    std::size_t dma_index = 0;
    for (const auto& item : encoded) {
        const auto& op = item.instruction;
        if (instruction_format(op.mnemonic) == Format::kDMA) {
            const auto& span = spans.at(dma_index++);
            if (span.line_count == 0 || span.line_count > maximum || span.line_offset > maximum ||
                span.line_offset >= capacity || span.line_count > capacity - span.line_offset)
                throw std::invalid_argument("fixed executable DMA span is zero or outside the memory window");
            if (op.mnemonic == Mnemonic::kDload) {
                const auto allowed = op.dma_flag != 0 ? op.dma_flag
                    : (op.type == 1 ? 0x20 : 0x1f);
                const auto small_limit = kSmallBankLineCount - ((mod_table_lines + 1) & ~1U);
                const auto limit = op.type == 1 ? mod_table_lines
                    : ((allowed & 0x1f) != 0 ? 1024 : small_limit);
                if (span.line_count > limit)
                    throw std::invalid_argument("fixed executable DMA exceeds its MOD/bank capacity");
                if (modulus_table_object == op.obj_id) modulus_table_object = -1;
                lengths.at(op.obj_id) = span.line_count;
                types.at(op.obj_id) = op.type;
                if (op.type == 1) modulus_table_object = op.obj_id;
            } else {
                if (span.line_count != lengths.at(op.obj_id))
                    throw std::invalid_argument("fixed executable DSTORE length differs from its object");
                if (op.type == 1) {
                    lengths.at(op.obj_id) = 0;
                    if (modulus_table_object == op.obj_id) modulus_table_object = -1;
                }
            }
        } else {
            switch (op.mnemonic) {
            case Mnemonic::kPadd: case Mnemonic::kPsub:
            case Mnemonic::kPmul: case Mnemonic::kPmac: {
                const auto length = lengths.at(op.psrc1);
                if (is_online_pmul(op)) {
                    if (length < kMinNttPolynomialLines || length > kMaxNttPolynomialLines ||
                        (length & (length - 1)) != 0 || lengths.at(op.psrc2) != 2 ||
                        types.at(op.psrc1) == 1 || types.at(op.psrc2) == 1 ||
                        (lengths.at(op.pdst) != 0 && types.at(op.pdst) == 1))
                        throw std::invalid_argument(
                            "fixed executable online PMUL requires power-of-two data and two non-modulus parameter lines");
                } else if ((op.imm8 < 0 && length != lengths.at(op.psrc2)) ||
                    (op.mnemonic == Mnemonic::kPmac && lengths.at(op.pdst) != length))
                    throw std::invalid_argument("fixed executable arithmetic operand lengths differ");
                lengths.at(op.pdst) = length;
                types.at(op.pdst) = 0;
                break;
            }
            case Mnemonic::kPntt: case Mnemonic::kPintt: {
                const auto data_lines = lengths.at(op.pdst);
                const auto table_lines = lengths.at(op.psrc1);
                const bool valid_twiddle = twiddle_format == ExecutableTwiddleFormat::online_k8
                    ? table_lines == kStageParameterLines : data_lines == 2 * table_lines;
                if (data_lines < kMinNttPolynomialLines ||
                    data_lines > kMaxNttPolynomialLines ||
                    (data_lines & (data_lines - 1)) != 0 ||
                    !valid_twiddle || types.at(op.pdst) == 1 || types.at(op.psrc1) == 1 ||
                    (std::uint64_t{1} << op.idx0) >= data_lines * 64)
                    throw std::invalid_argument(
                        "fixed executable transform requires a power-of-two polynomial "
                        "and non-modulus twiddles matching its declared format/stage");
                break;
            }
            case Mnemonic::kPmodld:
                if (modulus_table_object < 0 ||
                    (static_cast<unsigned>(op.mod_id) >> 4) >= lengths.at(modulus_table_object))
                    throw std::invalid_argument("fixed executable PMODLD exceeds the loaded table");
                break;
            case Mnemonic::kPfree:
                lengths.at(op.idx0) = 0;
                if (modulus_table_object == op.idx0) modulus_table_object = -1;
                break;
            default: break;
            }
        }
    }
}

}  // namespace

std::string c_identifier(std::string value) {
    if (value.empty()) {
        throw std::runtime_error("empty program name");
    }
    for (char& ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
            ch = '_';
        }
    }
    if (std::isdigit(static_cast<unsigned char>(value.front()))) {
        value.insert(value.begin(), '_');
    }
    return value;
}

std::vector<DmaRelocation> collect_dma_relocations(
    const std::vector<EncodedInstruction>& encoded) {
    std::vector<DmaRelocation> relocations;
    for (std::size_t instruction_index = 0;
         instruction_index < encoded.size(); ++instruction_index) {
        const auto& instruction = encoded[instruction_index].instruction;
        if (instruction_format(instruction.mnemonic) != Format::kDMA) {
            continue;
        }
        if (instruction.rs1 != 10 || instruction.rs2 != 11) {
            throw std::runtime_error(
                "executable DMA instruction does not use the frozen x10/x11 ABI");
        }
        relocations.push_back(DmaRelocation{
            instruction_index,
            relocations.size(),
            instruction.mnemonic,
            instruction.obj_id,
            instruction.type,
            instruction.dma_flag,
            instruction.cp,
        });
    }
    return relocations;
}

void validate_executable_program(
    const std::vector<EncodedInstruction>& encoded) {
    std::array<bool, 64> live{};
    int modulus_table_object = -1;
    auto require_live = [&](int object, std::size_t instruction_index,
                            const char* role) {
        if (object < 0 || object > 63 || !live.at(object)) {
            throw std::runtime_error(
                "object p" + std::to_string(object) + " is not live as " + role
                + " at instruction " + std::to_string(instruction_index));
        }
    };

    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const auto& instruction = encoded[index].instruction;
        switch (instruction.mnemonic) {
        case Mnemonic::kDload:
            if (instruction.type == 1 && modulus_table_object >= 0 &&
                modulus_table_object != instruction.obj_id)
                throw std::runtime_error("only one modulus-table object may be live");
            if (modulus_table_object == instruction.obj_id) modulus_table_object = -1;
            live.at(instruction.obj_id) = true;
            if (instruction.type == 1) modulus_table_object = instruction.obj_id;
            break;
        case Mnemonic::kDstore:
            require_live(instruction.obj_id, index, "dstore source");
            if (instruction.type == 1) {
                live.at(instruction.obj_id) = false;
                if (modulus_table_object == instruction.obj_id) modulus_table_object = -1;
            }
            break;
        case Mnemonic::kPadd:
        case Mnemonic::kPsub:
        case Mnemonic::kPmul:
        case Mnemonic::kPmac:
            require_live(instruction.psrc1, index, "arithmetic source 1");
            if (instruction.imm8 < 0)
                require_live(instruction.psrc2, index, "arithmetic source 2");
            if (is_online_pmul(instruction) &&
                (instruction.psrc2 == instruction.psrc1 || instruction.psrc2 == instruction.pdst ||
                 modulus_table_object == instruction.pdst ||
                 modulus_table_object == instruction.psrc1 ||
                 modulus_table_object == instruction.psrc2))
                throw std::runtime_error("online PMUL requires distinct non-modulus parameters and non-modulus data/destination");
            if (instruction.mnemonic == Mnemonic::kPmac)
                require_live(instruction.pdst, index, "accumulator destination");
            live.at(instruction.pdst) = true;
            break;
        case Mnemonic::kPntt:
        case Mnemonic::kPintt:
            require_live(instruction.pdst, index, "transform data");
            require_live(instruction.psrc1, index, "transform twiddle");
            break;
        case Mnemonic::kPmodld:
            if (modulus_table_object < 0)
                throw std::runtime_error(
                    "pmodld has no live modulus table at instruction " + std::to_string(index));
            break;
        case Mnemonic::kPfree:
            require_live(instruction.idx0, index, "pfree target");
            live.at(instruction.idx0) = false;
            if (modulus_table_object == instruction.idx0) modulus_table_object = -1;
            break;
        case Mnemonic::kPsync:
            // Synchronization neither releases objects nor requires an empty table.
            break;
        }
    }
    if (encoded.empty() || encoded.back().instruction.mnemonic != Mnemonic::kPsync)
        throw std::runtime_error("executable program must end with psync");
}

std::string render_executable_header(
    const std::string& stem,
    std::size_t dma_count) {
    const std::string id = c_identifier(stem);
    std::string guard = "HPU_PROGRAM_" + id + "_H";
    for (char& ch : guard) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }

    std::ostringstream output;
    output << "#ifndef " << guard << "\n#define " << guard << "\n\n"
           << "#include <stddef.h>\n#include <stdint.h>\n\n"
           << "#ifndef HPU_DMA_SPAN_T_DEFINED\n"
           << "#define HPU_DMA_SPAN_T_DEFINED\n"
           << "typedef struct hpu_dma_span {\n"
           << "    uint32_t line_offset;\n"
           << "    uint32_t line_count;\n"
           << "} hpu_dma_span_t;\n"
           << "#endif\n\n"
           << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
           << "enum { HPU_PROGRAM_";
    for (char ch : id) {
        output << static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    output << "_DMA_COUNT = " << dma_count << " };\n"
           << "int hpu_program_" << id
           << "(const hpu_dma_span_t *spans, size_t span_count);\n\n"
           << "#ifdef __cplusplus\n}\n#endif\n\n"
           << "#endif\n";
    return output.str();
}

std::string render_executable_source(
    const std::string& stem,
    const std::vector<EncodedInstruction>& encoded,
    std::uint64_t hpu_mem_line_count,
    std::uint32_t mod_table_lines) {
    if (mod_table_lines == 0 || mod_table_lines > 16)
        throw std::runtime_error("modulus table reservation must be 1..16 lines");
    if (hpu_mem_line_count == 0) {
        throw std::runtime_error("HPU_MEM line count must be nonzero");
    }
    validate_executable_program(encoded);
    const std::string id = c_identifier(stem);
    const auto relocations = collect_dma_relocations(encoded);
    std::ostringstream output;
    output << "/* Generated file: fixed HPU instruction words plus per-DMA relocation. */\n"
           << "#include \"" << stem << ".h\"\n\n"
           << "#define HPU_MEM_LINE_COUNT UINT64_C(" << hpu_mem_line_count
           << ")\n"
           << "enum { HPU_SMALL_BANK_LINE_COUNT = "
           << kSmallBankLineCount << " };\n\n"
           << "int hpu_program_" << id
           << "(const hpu_dma_span_t *spans, size_t span_count) {\n"
           << "    if (span_count != HPU_PROGRAM_";
    for (char ch : id) {
        output << static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    output << "_DMA_COUNT || (span_count != 0 && spans == NULL)) return -1;\n"
           << "    volatile uint32_t hpu_obj_len[64];\n"
           << "    for (size_t i = 0; i < 64; ++i) hpu_obj_len[i] = 0;\n"
           << "    for (size_t i = 0; i < span_count; ++i) {\n"
           << "        if (spans[i].line_count == 0 ||\n"
           << "            spans[i].line_offset >= HPU_MEM_LINE_COUNT ||\n"
           << "            spans[i].line_count > HPU_MEM_LINE_COUNT - spans[i].line_offset)\n"
           << "            return -2;\n"
           << "    }\n";

    std::size_t dma_index = 0;
    std::array<std::uint8_t, 64> object_types{};
    int modulus_table_object = -1;
    for (std::size_t instruction_index = 0;
         instruction_index < encoded.size(); ++instruction_index) {
        const auto& item = encoded[instruction_index];
        const auto& instruction = item.instruction;
        output << "    /* " << instruction_index << ": "
               << item.normalized_asm << " */\n";
        if (instruction_format(instruction.mnemonic) == Format::kDMA) {
            if (instruction.mnemonic == Mnemonic::kDload) {
                const auto allowed = instruction.dma_flag != 0 ? instruction.dma_flag
                    : (instruction.type == 1 ? 0x20 : 0x1f);
                const auto limit = instruction.type == 1 ? mod_table_lines
                    : ((allowed & 0x1f) != 0 ? 1024
                       : kSmallBankLineCount - ((mod_table_lines + 1) & ~1U));
                output << "    if (spans[" << dma_index
                       << "].line_count > " << limit << ") return -3;\n";
            }
            if (instruction.mnemonic == Mnemonic::kDload) {
                object_types.at(instruction.obj_id) = instruction.type;
                if (modulus_table_object == instruction.obj_id) modulus_table_object = -1;
                if (instruction.type == 1) modulus_table_object = instruction.obj_id;
                output << "    hpu_obj_len[" << static_cast<unsigned>(instruction.obj_id)
                       << "] = spans[" << dma_index << "].line_count;\n";
            } else {
                output << "    if (hpu_obj_len["
                       << static_cast<unsigned>(instruction.obj_id)
                       << "] == 0 || spans[" << dma_index
                       << "].line_count != hpu_obj_len["
                       << static_cast<unsigned>(instruction.obj_id)
                       << "]) return -5;\n";
            }
            output << "#if defined(__riscv)\n"
                   << "    {\n"
                   << "        register uintptr_t hpu_rs1 __asm__(\"x10\") = "
                   << "(uintptr_t)spans[" << dma_index << "].line_offset;\n"
                   << "        register uintptr_t hpu_rs2 __asm__(\"x11\") = "
                   << "(uintptr_t)spans[" << dma_index << "].line_count;\n";
            output << "        __asm__ volatile(\".word " << hex_word(item.word)
                   << "\" : : \"r\"(hpu_rs1), \"r\"(hpu_rs2) : \"memory\");\n"
                   << "    }\n"
                   << "#endif\n";
            if (instruction.mnemonic == Mnemonic::kDstore && instruction.type == 1) {
                if (modulus_table_object == instruction.obj_id) modulus_table_object = -1;
                output << "    hpu_obj_len["
                       << static_cast<unsigned>(instruction.obj_id)
                       << "] = 0;\n";
            }
            ++dma_index;
        } else {
            if (instruction.mnemonic == Mnemonic::kPadd
                || instruction.mnemonic == Mnemonic::kPsub
                || instruction.mnemonic == Mnemonic::kPmul
                || instruction.mnemonic == Mnemonic::kPmac) {
                output << "    if (hpu_obj_len[" << instruction.psrc1 << "] == 0";
                if (is_online_pmul(instruction)) {
                    const auto data = "hpu_obj_len[" + std::to_string(instruction.psrc1) + "]";
                    output << " || " << data << " < 2 || " << data << " > 1024 || ("
                           << data << " & (" << data << " - 1)) != 0 || hpu_obj_len["
                           << instruction.psrc2 << "] != 2";
                } else if (instruction.imm8 < 0)
                    output << " || hpu_obj_len[" << instruction.psrc1
                           << "] != hpu_obj_len[" << instruction.psrc2 << "]";
                if (instruction.mnemonic == Mnemonic::kPmac)
                    output << " || hpu_obj_len[" << instruction.pdst
                           << "] != hpu_obj_len[" << instruction.psrc1 << "]";
                output << ") return -6;\n";
                object_types.at(instruction.pdst) = 0;
                output << "    hpu_obj_len["
                       << static_cast<unsigned>(instruction.pdst)
                       << "] = hpu_obj_len["
                       << static_cast<unsigned>(instruction.psrc1) << "];\n";
            } else if (instruction.mnemonic == Mnemonic::kPntt
                       || instruction.mnemonic == Mnemonic::kPintt) {
                const auto data = "hpu_obj_len[" + std::to_string(instruction.pdst) + "]";
                output << "    if (" << data << " < 2 || " << data << " > 1024 || ("
                       << data << " & (" << data << " - 1)) != 0 || hpu_obj_len["
                       << instruction.psrc1 << "] != 2 || UINT32_C("
                       << (std::uint32_t{1} << instruction.idx0) << ") >= " << data
                       << " * 64) return -7;\n";
                if (object_types.at(instruction.pdst) == 1 || object_types.at(instruction.psrc1) == 1)
                    output << "    return -7; /* modulus table is not transform data/parameters */\n";
            } else if (instruction.mnemonic == Mnemonic::kPmodld) {
                output << "    if (UINT32_C(" << (static_cast<unsigned>(instruction.mod_id) >> 4)
                       << ") >= hpu_obj_len[" << modulus_table_object << "]) return -8;\n";
            } else if (instruction.mnemonic == Mnemonic::kPfree) {
                if (modulus_table_object == instruction.idx0) modulus_table_object = -1;
                output << "    hpu_obj_len["
                       << static_cast<unsigned>(instruction.idx0) << "] = 0;\n";
            }
            output << "#if defined(__riscv)\n"
                   << "    __asm__ volatile(\".word " << hex_word(item.word)
                   << "\" : : : \"memory\");\n"
                   << "#endif\n";
        }
    }
    output << "#if !defined(__riscv)\n"
           << "    return -4;\n"
           << "#else\n"
           << "    return 0;\n"
           << "#endif\n"
           << "}\n";
    return output.str();
}

std::string render_fixed_executable_source(
    const std::string& stem,
    const std::vector<EncodedInstruction>& encoded,
    std::uint64_t hpu_mem_line_count,
    const std::vector<ExecutableDmaSpan>& spans,
    ExecutableTwiddleFormat twiddle_format,
    std::uint32_t mod_table_lines) {
    const auto dynamic_source = render_executable_source(stem, encoded, hpu_mem_line_count, mod_table_lines);
    validate_fixed_spans(encoded, hpu_mem_line_count, spans, twiddle_format, mod_table_lines);
    const auto id = c_identifier(stem);
    std::ostringstream output;
    output << dynamic_source;
    if (!spans.empty()) {
        output << "\nstatic const hpu_dma_span_t hpu_program_" << id << "_resolved_spans[] = {\n";
        for (const auto& span : spans)
            output << "    { UINT32_C(" << span.line_offset << "), UINT32_C(" << span.line_count << ") },\n";
        output << "};\n";
    }
    output << "\n/* Fixed spans validated at generation; x11 changes only with DMA length. */\n"
           << "int hpu_run_" << id << "(void) {\n";
    if (!spans.empty()) output << "    (void)hpu_program_" << id << "_resolved_spans;\n";
    output << "#if defined(__riscv)\n"
           << "    __asm__ volatile(\n";
    std::size_t dma_index = 0;
    std::uint64_t previous_count = 0;
    for (const auto& item : encoded) {
        if (instruction_format(item.instruction.mnemonic) == Format::kDMA) {
            const auto& span = spans.at(dma_index++);
            output << "        \"li x10, " << span.line_offset << "\\n\\t\"\n";
            if (span.line_count != previous_count) {
                output << "        \"li x11, " << span.line_count << "\\n\\t\"\n";
                previous_count = span.line_count;
            }
        }
        output << "        \".word " << hex_word(item.word) << "\\n\\t\"\n";
    }
    // One indivisible asm region gives the compiler an exact register/memory
    // contract. No intervening C code or calls can overwrite the cached x11.
    output << "        : : : \"x10\", \"x11\", \"memory\");\n"
           << "    return 0;\n"
           << "#else\n"
           << "    return -4;\n"
           << "#endif\n"
           << "}\n";
    return output.str();
}

std::string render_dma_manifest(
    const std::vector<EncodedInstruction>& encoded) {
    const auto relocations = collect_dma_relocations(encoded);
    std::ostringstream output;
    output << "instruction_index,dma_index,direction,obj_id,type_or_release,flag,"
              "rs1,rs2,word_hex,normalized_asm,cp\n";
    for (const auto& relocation : relocations) {
        const auto& item = encoded[relocation.instruction_index];
        output << relocation.instruction_index << ',' << relocation.dma_index << ','
               << to_string(relocation.direction) << ','
               << static_cast<unsigned>(relocation.object_id) << ','
               << static_cast<unsigned>(relocation.type_or_release) << ','
               << static_cast<unsigned>(relocation.flag) << ",x10,x11,"
               << hex_word(item.word) << ',' << csv_field(item.normalized_asm) << ','
               << static_cast<unsigned>(relocation.cp) << '\n';
    }
    return output.str();
}

}  // namespace hpu
