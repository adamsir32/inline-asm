#pragma once

#include <cstdint>
#include <string>

namespace hpu {

enum class Format {
    kAR3,
    kSTG,
    kMOD,
    kCFG,
    kSYNC,
    kDMA,
};

enum class Mnemonic {
    kPadd,
    kPsub,
    kPmul,
    kPmac,
    kPntt,
    kPintt,
    kPmodld,
    kPfree,
    kPsync,
    kDload,
    kDstore,
};

struct Instruction {
    Mnemonic mnemonic {};

    // Semantic object operands reused by AR3 and STG encodings. For
    // pntt/pintt, pdst is pdata and psrc1 is ptwiddle; encode_stg maps them
    // to the STG-specific PDATA6 and PPARAM6 fields.
    int pdst = -1;
    int psrc1 = -1;
    int psrc2 = -1;
    int imm8 = -1;

    int idx0 = -1;
    int idx1 = -1;
    int mod_id = -1;
    // STG transform mode; for object PMUL, 1 selects online c*r^j factors.
    // Immediate arithmetic keeps mode=0: bits[15:8] are the full imm8.
    std::uint8_t mode = 0;
    std::uint8_t flag = 0;
    std::uint16_t cfg = 0;
    std::uint8_t tag = 0;

    int rs1 = -1;
    int rs2 = -1;
    std::uint8_t obj_id = 0;
    // DMA MOD (DLOAD) or REL (DSTORE), encoded in bit 14.
    std::uint8_t type = 0;
    // Per-command LLC allocation hint, independent of MOD/REL and bank mask.
    std::uint8_t cp = 0;
    // DLOAD bank mask (0 means MOD-dependent default); DSTORE must be zero.
    std::uint8_t dma_flag = 0;
};

struct EncodedInstruction {
    Instruction instruction;
    std::uint32_t word = 0;
    std::uint32_t command26 = 0;
    std::string normalized_asm;
};

std::string to_string(Mnemonic mnemonic);
std::string to_string(const Instruction& instruction);
Format instruction_format(Mnemonic mnemonic);

}  // namespace hpu
