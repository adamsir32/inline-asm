#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "assembler.hpp"
#include "executable.hpp"
#include "encoder.hpp"
#include "util/hpu_asm.hpp"

namespace {

void expect_rejected(const std::string& source)
{
    try {
        (void)hpu::assemble_line(source);
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("invalid instruction was accepted: " + source);
}

void expect_encoded(const std::string& source, std::uint32_t expected)
{
    const hpu::EncodedInstruction encoded = hpu::assemble_line(source);
    if (encoded.word != expected) {
        throw std::runtime_error("unexpected encoding for: " + source);
    }
}

void expect_precoded(const std::string& source,
                     std::uint32_t expected_word,
                     std::uint32_t expected_command26)
{
    const hpu::EncodedInstruction encoded = hpu::assemble_line(source);
    if (encoded.word != expected_word || encoded.command26 != expected_command26) {
        throw std::runtime_error("unexpected instruction/precode encoding for: " + source);
    }
}

void expect_program_rejected(const std::string& source)
{
    try {
        hpu::validate_executable_program(hpu::assemble_source(source));
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("invalid executable lifetime was accepted");
}

} // namespace

int main()
{
    try {
        if (hpu::dload(0, hpu::DataType::poly).find(
                "dload x10, x11, p0, 0, 0, 0") == std::string::npos
            || hpu::dstore(2, 1).find(
                "dstore x10, x11, p2, 1, 0") == std::string::npos) {
            throw std::runtime_error("runtime DMA register ABI mismatch");
        }
        const std::string smoke =
            "dload x10, x11, p0, 0, 0, 0\n"
            "dload x10, x11, p4, 1, 0, 32\n"
            "pmodld 255\n"
            "padd p2, p0, p1\n"
            "psub p2, p0, p1\n"
            "pmul p2, p0, p1\n"
            "pmac p2, p0, p1\n"
            "pntt p0, p3, 15, 3, 1\n"
            "pintt p0, p3, 15, 3, 1\n"
            "pfree p5\n"
            "dstore x10, x11, p2, 1, 0\n"
            "psync\n";
        const auto encoded = hpu::assemble_source(smoke);
        if (encoded.size() != 12) {
            throw std::runtime_error("unexpected smoke instruction count");
        }
        if ((encoded.front().word & 0x7fU) != 0x2bU
            || (encoded[10].word & 0x7fU) != 0x2bU
            || (encoded[2].word & 0x7fU) != 0x5bU
            || (encoded.back().word & 0x7fU) != 0x5bU) {
            throw std::runtime_error("custom opcode routing mismatch");
        }
        for (const auto& item : encoded) {
            const std::uint32_t expected_kind =
                (item.word & 0x7fU) == 0x2bU ? 1U : 0U;
            if ((item.command26 >> 25U) != expected_kind) {
                throw std::runtime_error("command kind is not encoded in cmd[25]");
            }
            if (expected_kind == 0U && item.command26 != (item.word >> 7U)) {
                throw std::runtime_error("custom0 payload precode mismatch");
            }
            if (expected_kind == 1U
                && item.command26 != ((1U << 25U) | (item.word >> 7U))) {
                throw std::runtime_error("custom1 payload precode mismatch");
            }
        }

        expect_encoded("padd p2, p0, p1", 0x0080015BU);
        expect_encoded("psub p2, p0, p1", 0x1080015BU);
        expect_encoded("pmul p2, p0, p1", 0x2080015BU);
        expect_encoded("pmac p2, p0, p1", 0x3080015BU);
        expect_precoded("pntt p0, p3, 15, 0, 0", 0x4003F05BU, 0x08007E0U);
        expect_precoded("pintt p0, p3, 15, 0, 0", 0x5003F05BU, 0x0A007E0U);
        expect_precoded("pntt p2, p3, 1, 0, 0", 0x4083105BU, 0x0810620U);
        expect_encoded("pmodld 0", 0x6000005BU);
        expect_encoded("pmodld 1", 0x6000015BU);
        expect_encoded("pmodld 255", 0x6000FF5BU);
        expect_encoded("pfree p4", 0x8004005BU);
        expect_encoded("psync", 0x7000005BU);
        expect_precoded("padd p2, p0, p1", 0x0080015BU, 0x0010002U);
        expect_precoded("pmodld 255", 0x6000FF5BU, 0x0C001FEU);
        expect_precoded("dload x10, x11, p0, 0, 0, 0", 0x00B5002BU, 0x2016A00U);
        expect_precoded("dload x1, x2, p0, 0, 0, 0", 0x0020802BU, 0x2004100U);
        expect_precoded("dload x10, x11, p4, 1, 0, 32", 0x12B5402BU, 0x2256A80U);
        expect_precoded("dstore x10, x11, p2, 1, 0", 0x08B5502BU, 0x2116AA0U);
        expect_encoded("dstore x10, x11, p2, 1, 0", 0x08B5502BU);
        expect_encoded("pmul p2, p0, 255", 0x2080FFDBU);
        expect_encoded("pmac p2, p0, 255", 0x3080FFDBU);
        expect_encoded("pmul p2, p0, 64", 0x208040DBU);
        expect_encoded("pmul p2, p0, 192", 0x2080C0DBU);
        expect_encoded("pmac p2, p0, 64", 0x308040DBU);
        expect_encoded("pmac p2, p0, 192", 0x3080C0DBU);
        expect_precoded("pmul p40, p40, p63, 1", 0x2A287F5BU, 0x05450FEU);
        const auto online = hpu::assemble_line("pmul p40, p40, p63, 1");
        if (online.instruction.mode != 1 || online.instruction.psrc2 != 63 ||
            online.normalized_asm != "pmul p40, p40, p63, 1" ||
            hpu::assemble_source(hpu::pmul_online(40, 40, 63)).front().word != online.word ||
            hpu::dma_mod_bit(hpu::DataType::online_pmul_parameters) != 0 ||
            hpu::dma_bank_mask(hpu::DataType::online_pmul_parameters,
                              hpu::DloadFlag::regular_bank) != 32)
            throw std::runtime_error("online PMUL assembly or parameter role mismatch");
        auto invalid_immediate_mode = hpu::assemble_line("pmul p2, p0, 192").instruction;
        invalid_immediate_mode.mode = 1;
        bool rejected_immediate_mode = false;
        try { (void)hpu::encode_instruction(invalid_immediate_mode); }
        catch (const std::exception&) { rejected_immediate_mode = true; }
        if (!rejected_immediate_mode)
            throw std::runtime_error("immediate PMUL accepted an independent mode field");

        // Frozen vectors from the v1.2 tables cover independent MOD/REL and
        // CP bits, six object/mask bits, and all five bits of rs2.
        expect_precoded("padd p63, p62, p61", 0x0FFE3D5BU, 0x01FFC7AU);
        expect_precoded("pntt p63, p62, 15, 3, 1", 0x4FFEFE5BU, 0x09FFDFCU);
        expect_encoded("pfree p63", 0x803F005BU);
        expect_precoded("dload x30, x31, p63, 0, 0, 63", 0xFFFF0FABU, 0x3FFFE1FU);
        expect_precoded("dload x30, x31, p63, 0, 1, 63", 0xFFFF2FABU, 0x3FFFE5FU);
        expect_precoded("dload x30, x31, p63, 1, 0, 32", 0xFFFF402BU, 0x3FFFE80U);
        expect_precoded("dload x30, x31, p63, 1, 1, 32", 0xFFFF602BU, 0x3FFFEC0U);
        expect_precoded("dstore x30, x31, p63, 0, 0", 0xFDFF102BU, 0x3FBFE20U);
        expect_precoded("dstore x30, x31, p63, 0, 1", 0xFDFF302BU, 0x3FBFE60U);
        expect_precoded("dstore x30, x31, p63, 1, 0", 0xFDFF502BU, 0x3FBFEA0U);
        expect_precoded("dstore x30, x31, p63, 1, 1", 0xFDFF702BU, 0x3FBFEE0U);
        expect_encoded("dload x10, x11, p4, 1, 0, 0", 0x10B5402BU);

        const auto cached = hpu::assemble_source(
            "dload x10, x11, p63, 0, 1, 32\n"
            "dstore x10, x11, p63, 1, 1\npsync\n");
        const auto cached_dma = hpu::collect_dma_relocations(cached);
        if (cached_dma.size() != 2 || cached_dma[0].cp != 1 || cached_dma[1].cp != 1
            || cached_dma[0].flag != 32 || cached_dma[1].type_or_release != 1)
            throw std::runtime_error("DMA relocation lost independent CP/MOD/REL/mask");
        if (hpu::render_dma_manifest(cached).find("normalized_asm,cp\n") == std::string::npos
            || cached[0].normalized_asm != "dload x10, x11, p63, 0, 1, 32")
            throw std::runtime_error("DMA manifest or normalized assembly lost CP");

        const auto executable_encoded = hpu::assemble_source(
            "dload x10, x11, p4, 1, 0, 32\n"
            "pmodld 0\n"
            "dload x10, x11, p0, 0, 0, 0\n"
            "dload x10, x11, p1, 0, 0, 0\n"
            "pmul p2, p0, p1\n"
            "pfree p0\n"
            "pfree p1\n"
            "dstore x10, x11, p2, 1, 0\n"
            "pfree p4\n"
            "psync\n");
        hpu::validate_executable_program(executable_encoded);
        hpu::validate_executable_program(hpu::assemble_source(
            "dload x10, x11, p0, 0, 0, 0\n"
            "dload x10, x11, p0, 0, 0, 0\n"
            "psync\n"));
        expect_program_rejected(
            "dload x10, x11, p0, 0, 0, 0\n"
            "psync\n"
            "pfree p0\n");
        hpu::validate_executable_program(hpu::assemble_source(
            "dload x10, x11, p63, 0, 0, 0\n"
            "dstore x10, x11, p63, 0, 0\n"
            "psync\n"
            "pfree p63\n"
            "psync\n"));
        expect_program_rejected(
            "dload x10, x11, p63, 0, 0, 0\n"
            "dstore x10, x11, p63, 1, 0\n"
            "pfree p63\n"
            "psync\n");
        expect_program_rejected(
            "dload x10, x11, p40, 1, 0, 0\n"
            "dload x10, x11, p41, 1, 0, 32\n"
            "psync\n");
        const auto online_program = hpu::assemble_source(
            "dload x10, x11, p4, 1, 0, 32\n"
            "pmodld 0\n"
            "dload x10, x11, p40, 0, 0, 0\n"
            "dload x10, x11, p63, 0, 1, 32\n"
            "pmul p40, p40, p63, 1\n"
            "pmul p41, p40, p63, 1\n"
            "dstore x10, x11, p41, 1, 0\n"
            "psync\n");
        hpu::validate_executable_program(online_program);
        for (const std::uint64_t data_lines : {2U, 128U, 1024U}) {
            (void)hpu::render_fixed_executable_source("online", online_program, 4096,
                {{0, 4}, {4, data_lines}, {1028, 2}, {1030, data_lines}});
        }
        for (const auto shape : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                 {1, 2}, {3, 2}, {128, 1}, {128, 4}}) {
            bool rejected = false;
            try {
                (void)hpu::render_fixed_executable_source("online", online_program, 4096,
                    {{0, 4}, {4, shape.first}, {1028, shape.second}, {1030, shape.first}});
            } catch (const std::exception&) { rejected = true; }
            if (!rejected) throw std::runtime_error("online PMUL accepted invalid data/parameter lengths");
        }
        for (const auto arithmetic : {"pmul p4, p40, p63, 1", "pmul p40, p4, p63, 1",
                                      "pmul p40, p40, p4, 1"}) {
            expect_program_rejected(
                std::string("dload x10, x11, p4, 1, 0, 32\npmodld 0\n") +
                "dload x10, x11, p40, 0, 0, 0\n" +
                "dload x10, x11, p63, 0, 0, 32\n" + arithmetic + "\npsync\n");
        }
        const auto online_source = hpu::render_executable_source("online", online_program, 4096);
        if (online_source.find("hpu_obj_len[63] != 2") == std::string::npos ||
            online_source.find("hpu_obj_len[40] != hpu_obj_len[63]") != std::string::npos)
            throw std::runtime_error("online PMUL runtime guard uses vector operand length");
        const std::string executable =
            hpu::render_executable_source("smoke", executable_encoded, 65536);
        const std::string header = hpu::render_executable_header(
            "smoke", hpu::collect_dma_relocations(executable_encoded).size());
        const std::string manifest = hpu::render_dma_manifest(executable_encoded);
        if (hpu::collect_dma_relocations(executable_encoded).size() != 4)
            throw std::runtime_error("executable DMA count mismatch");
        if (executable.find("hpu_obj_len[64]") == std::string::npos)
            throw std::runtime_error("executable does not track all 64 objects");
        if (executable.find("#define HPU_MEM_LINE_COUNT UINT64_C(65536)")
            == std::string::npos)
            throw std::runtime_error("executable HPU_MEM capacity mismatch");
        if (executable.find("register uintptr_t hpu_rs1 __asm__(\"x10\")")
            == std::string::npos)
            throw std::runtime_error("executable x10 binding mismatch");
        if (executable.find("register uintptr_t hpu_rs2 __asm__(\"x11\")")
            == std::string::npos)
            throw std::runtime_error("executable x11 binding mismatch");
        if (executable.find(".word 0x12B5402B") == std::string::npos)
            throw std::runtime_error("executable fixed word mismatch");
        if (executable.find("spans[3].line_offset") == std::string::npos)
            throw std::runtime_error("executable DSTORE relocation mismatch");
        if (executable.find(
                "hpu_rs2 __asm__(\"x11\") = (uintptr_t)spans[3].line_count;")
            == std::string::npos)
            throw std::runtime_error("executable DSTORE x11/count ABI mismatch");
        if (executable.find(
                "spans[3].line_count != hpu_obj_len[2]") == std::string::npos)
            throw std::runtime_error("executable DSTORE OBJ.len guard mismatch");
        if (executable.find("hpu_rs2 __asm__(\"x11\") = 0;")
            != std::string::npos)
            throw std::runtime_error("executable contains zero-length DMA sideband");
        if (header.find("HPU_PROGRAM_SMOKE_DMA_COUNT = 4") == std::string::npos)
            throw std::runtime_error("executable header mismatch");
        if (manifest.find("7,3,dstore,2,1,0,x10,x11,0x08B5502B")
            == std::string::npos)
            throw std::runtime_error("DMA manifest mismatch");

        const std::vector<std::string> invalid{
            "padd p64, p0, p1",
            "padd p0, p1, 1",
            "pmul p0, p1, 256",
            "pmul p0, p1, p2, 2",
            "pmul p0, p1, p2, 3",
            "pmul p0, p1, p2, 256",
            "pmul p0, p1, p2, -1",
            "pmul p0, p1, 192, 1",
            "pmul p0, p1, p0, 1",
            "pmul p0, p1, p1, 1",
            "padd p0, p1, p2, 1",
            "psub p0, p1, p2, 1",
            "pmac p0, p1, p2, 1",
            "pntt p0, p1, 16, 0, 0",
            "pntt p0, p1, 0, 4, 0",
            "pntt p0, p1, 0, 0, 2",
            "pmodld 256",
            "pmodld -1",
            "pmodld p0, 0, 0",
            "pfree p64",
            "pfree p0, 0, 0",
            "pshcfg p0, 0, 0",
            "pshuf p0, p1, 0, 0, 0",
            "pseed 0",
            "psample p0, p1, 0, 0, 0",
            "psync 0, 0",
            "dload x32, x0, p0, 0, 0, 0",
            "dload x0, x0, p0, 2, 0, 0",
            "dload x0, x0, p0, 3, 0, 32",
            "dload x0, x0, p0, 0, 2, 0",
            "dload x0, x0, p0, 0, 256, 0",
            "dload x0, x0, p0, 0, -1, 0",
            "dload x0, x0, p0, 0, 0",
            "dload x0, x0, p0, 0",
            "dload x0, x0, p0, 1, 0, 2",
            "dload x0, x0, p0, 0, 0, 64",
            "dload x0, x0, p0, 0, 0, 256",
            "dload x0, x0, p0, 256, 0, 0",
            "dload x0, x0, p0, -1, 0",
            "pntt p0, p1, 0, 256, 0",
            "pplace p0, 1",
            "dstore x0, x0, p0, 2, 0",
            "dstore x0, x0, p0, 1, 2",
            "dstore x0, x0, p0, 1, 256",
            "dstore x0, x0, p0, 1, -1",
            "dstore x0, x0, p0, 1",
        };
        for (const std::string& source : invalid) {
            expect_rejected(source);
        }

        std::cout << "HPU encoder self-test: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "HPU encoder self-test failed: " << exception.what() << '\n';
        return 1;
    }
}
