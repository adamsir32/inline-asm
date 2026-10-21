#!/usr/bin/env python3
"""Check the fixed C entry's DMA register values in RV32/RV64 machine code.

This executes only the CPU wrapper. Custom HPU words are checked and leave CPU
registers unchanged; HPU arithmetic remains covered by the instruction model.
No RISC-V runtime, target libc, Python packages, or emulator is required.
"""

import argparse
import csv
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile


def signed(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


class Elf:
    def __init__(self, path):
        data = path.read_bytes()
        assert data[:4] == b"\x7fELF" and data[5] == 1, "expected little-endian ELF"
        self.bits = 32 if data[4] == 1 else 64
        header = struct.unpack_from("<16sHHIIIIIHHHHHH" if self.bits == 32
                                    else "<16sHHIQQQIHHHHHH", data)
        assert header[2] == 243, "expected RISC-V ELF"
        self.entry = header[4]
        self.segments = []
        for index in range(header[10]):
            offset = header[5] + index * header[9]
            fields = struct.unpack_from("<IIIIIIII" if self.bits == 32
                                        else "<IIQQQQQQ", data, offset)
            if fields[0] != 1:
                continue
            if self.bits == 32:
                _, file_offset, address, _, file_size, memory_size, _, _ = fields
            else:
                _, _, file_offset, address, _, file_size, memory_size, _ = fields
            payload = bytearray(data[file_offset:file_offset + file_size])
            payload.extend(bytes(memory_size - file_size))
            self.segments.append((address, payload))
        sections = [struct.unpack_from("<IIIIIIIIII" if self.bits == 32
                                       else "<IIQQQQIIQQ", data,
                                       header[6] + i * header[11])
                    for i in range(header[12])]
        self.symbols = {}
        for section in sections:
            if section[1] != 2:  # SHT_SYMTAB
                continue
            strings = sections[section[6]]
            names = data[strings[4]:strings[4] + strings[5]]
            for offset in range(section[4], section[4] + section[5], section[9]):
                if self.bits == 32:
                    name, value, size, _, _, _ = struct.unpack_from("<IIIBBH", data, offset)
                else:
                    name, _, _, _, value, size = struct.unpack_from("<IBBHQQ", data, offset)
                end = names.find(b"\0", name)
                self.symbols[names[name:end].decode()] = (value, size)


class Cpu:
    """Small fail-closed RV32I/RV64I interpreter for compiler output."""

    def __init__(self, image, words, dma):
        self.bits = image.bits
        self.mask = (1 << self.bits) - 1
        self.segments = [(base, bytearray(data)) for base, data in image.segments]
        self.stack_base = 0x70000000 - 1024 * 1024
        self.segments.append((self.stack_base, bytearray(1024 * 1024 + 4096)))
        self.registers = [((0x102030405060708 + index * 0x10203) & self.mask)
                          for index in range(32)]
        self.registers[0] = 0
        self.registers[1] = self.stop = 0x7fff0000
        self.registers[2] = 0x70000000
        self.initial = list(self.registers)
        self.pc = image.entry
        self.words, self.dma = words, dma
        self.custom_index = self.dma_index = 0
        self.x11_writes = 0
        self.x11_writes_at_last_dma = 0
        self.x10_writes_since_dma = 0

    def memory(self, address, size, value=None):
        for base, data in self.segments:
            offset = address - base
            if 0 <= offset and offset + size <= len(data):
                if value is not None:
                    data[offset:offset + size] = (value & ((1 << (size * 8)) - 1)).to_bytes(size, "little")
                return int.from_bytes(data[offset:offset + size], "little")
        raise AssertionError(f"unmapped memory at 0x{address:x}, PC 0x{self.pc:x}")

    def write(self, register, value):
        if register:
            self.registers[register] = value & self.mask
            if register == 10:
                self.x10_writes_since_dma += 1
            if register == 11:
                self.x11_writes += 1

    def custom(self, word):
        assert self.custom_index < len(self.words), "unexpected extra HPU instruction"
        expected = self.words[self.custom_index]
        assert word == expected, (f"HPU instruction {self.custom_index}: "
                                  f"0x{word:08x} != 0x{expected:08x}")
        self.custom_index += 1
        if word & 0x7f != 0x2b:
            return
        assert self.dma_index < len(self.dma), "unexpected extra DMA"
        row = self.dma[self.dma_index]
        assert (word >> 15) & 31 == 10 and (word >> 20) & 31 == 11, "DMA ABI register mismatch"
        assert self.registers[10] == row[0], (f"DMA {self.dma_index}: x10 "
            f"{self.registers[10]} != line_offset {row[0]}")
        assert self.registers[11] == row[1], (f"DMA {self.dma_index}: x11 "
            f"{self.registers[11]} != line_count {row[1]}")
        assert self.x10_writes_since_dma, f"DMA {self.dma_index}: x10 was not reloaded"
        self.x10_writes_since_dma = 0
        self.x11_writes_at_last_dma = self.x11_writes
        self.dma_index += 1

    def run(self, check_setting_count=True):
        for _ in range(max(1000000, len(self.words) * 100)):
            if self.pc == self.stop:
                break
            word = self.memory(self.pc, 4)
            assert word & 3 == 3, f"compressed instruction at 0x{self.pc:x}; use -march=rv*im"
            opcode = word & 0x7f
            rd, f3, rs1, rs2, f7 = (word >> 7) & 31, (word >> 12) & 7, (word >> 15) & 31, (word >> 20) & 31, word >> 25
            a, b = self.registers[rs1], self.registers[rs2]
            immediate = signed(word >> 20, 12)
            next_pc = (self.pc + 4) & self.mask
            if opcode in (0x2b, 0x5b):
                self.custom(word)
            elif opcode == 0x37:
                self.write(rd, signed(word & 0xfffff000, 32))
            elif opcode == 0x17:
                self.write(rd, self.pc + signed(word & 0xfffff000, 32))
            elif opcode in (0x13, 0x1b):
                width = 32 if opcode == 0x1b else self.bits
                shift = (word >> 20) & (width - 1)
                if f3 == 0:
                    value = a + immediate
                elif f3 == 1:
                    value = a << shift
                elif f3 == 2:
                    value = int(signed(a, self.bits) < immediate)
                elif f3 == 3:
                    value = int(a < (immediate & self.mask))
                elif f3 == 4:
                    value = a ^ immediate
                elif f3 == 5:
                    value = signed(a, width) >> shift if word & (1 << 30) else (a & ((1 << width) - 1)) >> shift
                elif f3 == 6:
                    value = a | immediate
                else:
                    value = a & immediate
                self.write(rd, signed(value, 32) if opcode == 0x1b else value)
            elif opcode in (0x33, 0x3b):
                width = 32 if opcode == 0x3b else self.bits
                if f7 == 1 and f3 == 0:
                    value = a * b
                elif f7 == 0x20 and f3 == 0:
                    value = a - b
                elif f3 == 0 and f7 == 0:
                    value = a + b
                elif f3 == 1 and f7 == 0:
                    value = a << (b & (width - 1))
                elif f3 == 2 and f7 == 0:
                    value = int(signed(a, self.bits) < signed(b, self.bits))
                elif f3 == 3 and f7 == 0:
                    value = int(a < b)
                elif f3 == 4 and f7 == 0:
                    value = a ^ b
                elif f3 == 5 and f7 in (0, 0x20):
                    value = signed(a, width) >> (b & (width - 1)) if f7 else (a & ((1 << width) - 1)) >> (b & (width - 1))
                elif f3 == 6 and f7 == 0:
                    value = a | b
                elif f3 == 7 and f7 == 0:
                    value = a & b
                else:
                    raise AssertionError(f"unsupported OP at 0x{self.pc:x}: 0x{word:08x}")
                self.write(rd, signed(value, 32) if opcode == 0x3b else value)
            elif opcode == 0x03:
                size = 1 << (f3 & 3)
                value = self.memory((a + immediate) & self.mask, size)
                self.write(rd, value if f3 & 4 else signed(value, size * 8))
            elif opcode == 0x23:
                offset = signed(((word >> 25) << 5) | rd, 12)
                self.memory((a + offset) & self.mask, 1 << f3, b)
            elif opcode == 0x63:
                offset = signed(((word >> 31) << 12) | (((word >> 7) & 1) << 11)
                                | (((word >> 25) & 63) << 5) | (((word >> 8) & 15) << 1), 13)
                conditions = {0: a == b, 1: a != b, 4: signed(a, self.bits) < signed(b, self.bits),
                              5: signed(a, self.bits) >= signed(b, self.bits), 6: a < b, 7: a >= b}
                assert f3 in conditions, "reserved branch"
                if conditions[f3]:
                    next_pc = (self.pc + offset) & self.mask
            elif opcode == 0x6f:
                offset = signed(((word >> 31) << 20) | (((word >> 12) & 255) << 12)
                                | (((word >> 20) & 1) << 11) | (((word >> 21) & 1023) << 1), 21)
                self.write(rd, next_pc)
                next_pc = (self.pc + offset) & self.mask
            elif opcode == 0x67 and f3 == 0:
                target = (a + immediate) & self.mask & ~1
                self.write(rd, next_pc)
                next_pc = target
            elif opcode == 0x0f:
                pass
            else:
                raise AssertionError(f"unsupported CPU instruction at 0x{self.pc:x}: 0x{word:08x}")
            self.pc = next_pc
        else:
            raise AssertionError("CPU instruction limit exceeded")
        assert self.custom_index == len(self.words), "missing HPU instructions"
        assert self.dma_index == len(self.dma), "missing DMA instructions"
        assert self.registers[10] == 0, f"entry returned {signed(self.registers[10], self.bits)}"
        for register in (8, 9, *range(18, 28)):
            assert self.registers[register] == self.initial[register], f"callee-saved x{register} was corrupted"
        expected = sum(index == 0 or row[1] != self.dma[index - 1][1]
                       for index, row in enumerate(self.dma))
        if check_setting_count:
            assert self.x11_writes_at_last_dma == expected, (f"x11 writes "
                f"{self.x11_writes_at_last_dma} != length runs {expected}")
        return expected


def self_test():
    class Image:
        bits, entry = 32, 0x1000

    load = 0x00b5002b
    def execute(code):
        image = Image()
        image.segments = [(image.entry, b"".join(struct.pack("<I", word) for word in code))]
        return Cpu(image, [load, load], [(7, 1), (8, 1)]).run()
    prefix = [0x00700513, 0x00100593, load]
    suffix = [0x00800513, load, 0x00000513, 0x00008067]
    execute(prefix + suffix)
    for code, message in ((prefix + [0x00900513] + suffix[1:], "x10"),
                          (prefix + [0x00100593] + suffix, "x11 writes")):
        try:
            execute(code)
        except AssertionError as error:
            assert message in str(error), str(error)
        else:
            raise AssertionError(f"negative self-test did not reject {message}")


def load_package(directory):
    descriptor_path = directory / "package.json"
    if not descriptor_path.is_file():
        descriptor_path = directory / "fixture.json"
    descriptor = json.loads(descriptor_path.read_text())
    source = directory / descriptor["program_source"]
    words = [int(line.strip(), 2) for line in
             (directory / descriptor["program_inst32"]).read_text().splitlines() if line.strip()]
    with (directory / descriptor["dma_relocation_manifest"]).open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    dma = [(int(row["line_offset"]), int(row["line_count"])) for row in rows]
    assert len(dma) == sum(word & 0x7f == 0x2b for word in words), "DMA manifest count differs"
    for row in rows:
        assert int(row["word_hex"], 16) == words[int(row["instruction_index"])], "DMA manifest word differs"
    name = re.search(r"\bint\s+(hpu_run_\w+)\s*\(\s*void\s*\)", source.read_text())
    assert name, "missing fixed entry"
    return source, name[1], words, dma


def tool_environment(compiler):
    environment = dict(os.environ)
    # A relocatable Debian cross-toolchain may need its adjacent host libraries.
    libraries = compiler.parent.parent / "lib" / "x86_64-linux-gnu"
    if libraries.is_dir():
        environment["LD_LIBRARY_PATH"] = str(libraries) + os.pathsep + environment.get("LD_LIBRARY_PATH", "")
    return environment


def verify(directory, compiler, objdump, temporary):
    source, entry, words, dma = load_package(directory)
    environment = tool_environment(compiler)
    include = subprocess.check_output([str(compiler), "-print-file-name=include"], env=environment, text=True).strip()
    stress = temporary / (directory.name + "-stress.c")
    guards = [f"guard_{index}" for index in range(20)]
    values = [0x10203 + index * 17 for index in range(len(guards))]
    operands = ", ".join(f'"r"({guard})' for guard in guards)
    stress.write_text(
        f'#include "{source.with_suffix(".h").as_posix()}"\n'
        f"__attribute__((always_inline)) inline int {entry}(void);\n"
        f'#include "{source.as_posix()}"\n'
        "static volatile uintptr_t hpu_guard_values[] = {"
        + ", ".join(str(value) for value in values) + "};\n"
        "int hpu_codegen_register_stress(void) {\n"
        + "".join(f"    uintptr_t {guard} = hpu_guard_values[{index}];\n"
                  for index, guard in enumerate(guards))
        + f'    __asm__ volatile("" : : {operands});\n'
        + f"    int result = {entry}();\n"
        + f'    __asm__ volatile("" : : {operands});\n'
        + "    if (result != 0) return -6;\n"
        + "    return (" + " && ".join(f"{guard} == {value}" for guard, value in zip(guards, values))
        + ") ? 0 : -7;\n}\n")
    results = []
    for bits, abi in ((32, "ilp32"), (64, "lp64")):
        for optimization in ("-O0", "-O2"):
            for input_source, function, kind in ((source, entry, "fixed"),
                    (stress, "hpu_codegen_register_stress", "register-stress")):
                stem = f"{directory.name}-rv{bits}-{optimization[1:]}-{kind}"
                binary = temporary / (stem + ".elf")
                command = [str(compiler), f"-march=rv{bits}im", f"-mabi={abi}", optimization,
                           "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-pie", "-no-pie",
                           "-nostdlib", "-nostdinc", "-isystem", include, f"-B{compiler.parent}/",
                           "-Wl,--no-relax", "-Wl,--build-id=none", f"-Wl,-e,{function}",
                           "-Wl,-Ttext=0x10000", str(input_source), "-o", str(binary)]
                subprocess.run(command, check=True, env=environment, capture_output=True, text=True)
                if objdump:
                    output = subprocess.check_output([str(objdump), "-d", str(binary)], env=environment, text=True)
                    (temporary / (stem + ".disasm")).write_text(output)
                image = Elf(binary)
                assert image.entry == image.symbols[function][0], "wrong ELF entry"
                # The pressure harness itself legitimately uses x11 before the
                # inlined wrapper. Count settings in the standalone entry only.
                count = Cpu(image, words, dma).run(check_setting_count=kind == "fixed")
                detail = f"{count} x11 settings" if kind == "fixed" else "live CPU values preserved"
                print(f"{directory.name} RV{bits} {optimization} {kind}: {len(words)} HPU words, "
                      f"{len(dma)} DMA, {detail}", flush=True)
                results.append({"package_or_fixture": str(directory), "xlen": bits,
                                "optimization": optimization, "entry": function, "kind": kind,
                                "hpu_words": len(words), "dma_count": len(dma),
                                "x11_settings": count if kind == "fixed" else None,
                                "elf": binary.name, "verified": True})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, action="append", default=[])
    parser.add_argument("--generator", type=Path)
    parser.add_argument("--cc", type=Path)
    parser.add_argument("--objdump", type=Path)
    parser.add_argument("--keep", "--output-dir", type=Path, help="retain target ELF and disassembly in this directory")
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    self_test()
    if arguments.self_test and not arguments.package and not arguments.generator:
        print("CPU wrapper checker self-tests passed")
        return 0
    found = shutil.which("riscv64-linux-gnu-gcc") or shutil.which("riscv64-unknown-elf-gcc")
    compiler = arguments.cc or (Path(found) if found else None)
    if compiler is None or not compiler.is_file():
        print("SKIP: configure --cc with a RISC-V GCC supporting RV32 and RV64")
        return 77
    assert arguments.package or arguments.generator, "provide --package or --generator"
    with tempfile.TemporaryDirectory(prefix="hpu-target-dma-") as directory:
        temporary = arguments.keep.resolve() if arguments.keep else Path(directory)
        temporary.mkdir(parents=True, exist_ok=True)
        packages = list(arguments.package)
        if arguments.generator:
            generated = temporary / "fixture"
            subprocess.run([str(arguments.generator.resolve()), str(generated)], check=True)
            packages.append(generated)
        results = []
        for package in packages:
            results.extend(verify(package.resolve(), compiler.resolve(), arguments.objdump, temporary))
        (temporary / "validation.json").write_text(json.dumps(
            {"compiler": str(compiler.resolve()), "checker_self_tests": "pass", "cases": results}, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        print(error.stderr or str(error), file=sys.stderr)
        sys.exit(1)
