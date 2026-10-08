#include "hpu/compiler/resource_ir.hpp"
#include "hpu/compiler/operator_resources.hpp"

#include "encoder.hpp"
#include "executable.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace hpu::compiler {
namespace {

constexpr std::size_t kNoUse = std::numeric_limits<std::size_t>::max();
constexpr const char* kScratchArena = "$compiler/scratch_arena";

bool contains(hpu::runtime::HpuMemSpan outer, hpu::runtime::HpuMemSpan inner)
{
    return inner.line_count != 0 && inner.line_offset >= outer.line_offset
        && inner.line_offset - outer.line_offset <= outer.line_count
        && inner.line_count <= outer.line_count - (inner.line_offset - outer.line_offset);
}

bool overlaps(hpu::runtime::HpuMemSpan a, hpu::runtime::HpuMemSpan b)
{
    return a.line_offset < b.line_offset + b.line_count
        && b.line_offset < a.line_offset + a.line_count;
}

bool same_span(hpu::runtime::HpuMemSpan a, hpu::runtime::HpuMemSpan b)
{
    return a.line_offset == b.line_offset && a.line_count == b.line_count;
}

hpu::EncodedInstruction encode(hpu::Instruction instruction)
{
    const auto word = hpu::encode_instruction(instruction);
    return {instruction, word, hpu::precode_command26(word), hpu::to_string(instruction)};
}

void unique_push(std::vector<ObjectId>& values, ObjectId value)
{
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

std::string json_string(const std::string& value)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char character : value) {
        if (character == '\\' || character == '"') {
            result += '\\';
            result += static_cast<char>(character);
        } else if (character < 0x20) {
            result += "\\u00";
            result += hex[character >> 4];
            result += hex[character & 15];
        } else {
            result += static_cast<char>(character);
        }
    }
    result += '"';
    return result;
}

const char* layout_name(MemoryLayout layout)
{
    switch (layout) {
    case MemoryLayout::coefficient_natural: return "coefficient_natural";
    case MemoryLayout::coefficient_bit_reversed: return "coefficient_bit_reversed";
    case MemoryLayout::canonical_ntt_physical: return "canonical_ntt_physical";
    default: return "unspecified";
    }
}

std::vector<std::optional<std::size_t>> dma_by_instruction(
    const std::vector<hpu::EncodedInstruction>& instructions,
    const std::vector<ResolvedDma>& dma)
{
    const auto encoded_dma = hpu::collect_dma_relocations(instructions);
    if (encoded_dma.size() != dma.size()) {
        throw std::invalid_argument("resolved DMA count differs from encoded program");
    }
    std::vector<std::optional<std::size_t>> indices(instructions.size());
    for (std::size_t index = 0; index < dma.size(); ++index) {
        if (dma[index].instruction_index != encoded_dma[index].instruction_index) {
            throw std::invalid_argument("resolved DMA must be in encoded instruction order");
        }
        indices[dma[index].instruction_index] = index;
    }
    return indices;
}

// Removing a writeback is safe only when there is one exact consumer before
// the next overlapping write. Other loads could observe the old DDR contents.
std::map<std::size_t, std::size_t> deferred_store_pairs(
    const std::vector<hpu::EncodedInstruction>& instructions,
    const std::vector<ResolvedDma>& dma,
    const hpu::runtime::HpuMemImage& image)
{
    std::map<std::size_t, std::size_t> pairs;
    for (std::size_t index = 0; index < dma.size(); ++index) {
        const auto& store = dma[index];
        const auto& allocation = image.allocation(store.allocation_id);
        if (instructions[store.instruction_index].instruction.mnemonic
                != hpu::Mnemonic::kDstore
            || allocation.kind != hpu::runtime::AllocationKind::workspace
            || allocation.read_only) {
            continue;
        }
        std::optional<std::size_t> consumer;
        for (std::size_t next = index + 1; next < dma.size(); ++next) {
            if (!overlaps(store.span, dma[next].span)) continue;
            if (instructions[dma[next].instruction_index].instruction.mnemonic
                    == hpu::Mnemonic::kDload
                && same_span(store.span, dma[next].span)) {
                consumer = next;
            }
            break;
        }
        if (!consumer) continue;
        bool another_read = false;
        for (std::size_t next = *consumer + 1; next < dma.size(); ++next) {
            if (!overlaps(store.span, dma[next].span)) continue;
            another_read = instructions[dma[next].instruction_index].instruction.mnemonic
                != hpu::Mnemonic::kDstore;
            // A partial overlapping write leaves part of the old value live.
            if (!contains(dma[next].span, store.span)) another_read = true;
            break;
        }
        if (!another_read) pairs.emplace(index, *consumer);
    }
    return pairs;
}

void analyze_buffers(VirtualProgram& program,
                     const std::vector<ResolvedDma>& dma,
                     const std::vector<hpu::EncodedInstruction>& instructions,
                     const hpu::runtime::HpuMemImage& image)
{
    std::map<std::string, BufferId> ids;
    std::vector<std::vector<bool>> written;
    for (BufferId index = 0; index < program.buffers.size(); ++index) {
        auto& buffer = program.buffers[index];
        ids.emplace(buffer.id, index);
        buffer.first_use = kNoUse;
        buffer.fully_defined_before_read = true;
        written.emplace_back(buffer.original_span.line_count, false);
    }
    for (const auto& entry : dma) {
        const auto& allocation = image.allocation(entry.allocation_id);
        const BufferId id = ids.at(allocation.id);
        auto& buffer = program.buffers[id];
        buffer.first_use = std::min(buffer.first_use, entry.instruction_index);
        buffer.last_use = std::max(buffer.last_use, entry.instruction_index);
        const auto offset = entry.span.line_offset - allocation.span.line_offset;
        const bool load = instructions[entry.instruction_index].instruction.mnemonic
            == hpu::Mnemonic::kDload;
        for (std::uint64_t line = offset; line < offset + entry.span.line_count; ++line) {
            if (load && !written[id][line]) buffer.fully_defined_before_read = false;
            if (!load) written[id][line] = true;
        }
    }
    for (auto& buffer : program.buffers) {
        buffer.retained = buffer.kind != hpu::runtime::AllocationKind::workspace
            || buffer.read_only || !buffer.fully_defined_before_read;
        if (buffer.first_use == kNoUse) {
            // Unused payloads are kept for deterministic image compatibility.
            buffer.first_use = 0;
            buffer.retained = true;
        }
    }
}

ResourceReport inspect_original(const std::vector<hpu::EncodedInstruction>& instructions,
                                const std::vector<ResolvedDma>& dma,
                                const hpu::runtime::HpuMemImage& image,
                                const CompileOptions& options)
{
    const auto lookup = dma_by_instruction(instructions, dma);
    std::array<std::uint64_t, 8> lines{};
    std::array<bool, 8> small{};
    ResourceReport report;
    report.original_ddr_lines = image.used_lines();
    report.allocated_ddr_lines = image.used_lines();
    report.original_dma_count = report.dma_count = dma.size();
    auto measure = [&](bool extra_bank) {
        std::size_t banks = extra_bank ? 1 : 0;
        std::size_t slots = 0;
        std::uint64_t small_lines = 0;
        for (std::size_t slot = 0; slot < lines.size(); ++slot) {
            if (!lines[slot]) continue;
            ++slots;
            if (small[slot]) {
                small_lines += lines[slot];
            } else {
                if (lines[slot] > options.regular_bank_lines) {
                    throw std::invalid_argument("object exceeds one regular bank");
                }
                ++banks;
            }
        }
        report.peak_regular_banks = std::max(report.peak_regular_banks, banks);
        report.peak_object_slots = std::max(report.peak_object_slots, slots);
        if (banks > options.regular_bank_count) {
            throw std::invalid_argument("fixed schedule exceeds available regular banks");
        }
        if (small_lines > 32) {
            throw std::invalid_argument("fixed schedule exceeds small bank capacity");
        }
    };
    for (std::size_t index = 0; index < instructions.size(); ++index) {
        const auto& op = instructions[index].instruction;
        switch (op.mnemonic) {
        case hpu::Mnemonic::kDload:
            lines[op.obj_id] = dma[*lookup[index]].span.line_count;
            small[op.obj_id] = op.dma_flag != 0;
            break;
        case hpu::Mnemonic::kDstore: lines[op.obj_id] = 0; break;
        case hpu::Mnemonic::kPfree: lines[op.idx0] = 0; break;
        case hpu::Mnemonic::kPadd:
        case hpu::Mnemonic::kPsub:
        case hpu::Mnemonic::kPmul:
        case hpu::Mnemonic::kPmac:
            lines[op.pdst] = lines[op.psrc1];
            small[op.pdst] = false;
            break;
        default: break;
        }
        measure(op.mnemonic == hpu::Mnemonic::kPntt
                || op.mnemonic == hpu::Mnemonic::kPintt);
    }
    return report;
}

struct PendingDma {
    std::size_t instruction_index;
    BufferRange range;
    ResolvedDma origin;
};

struct ScheduledProgram {
    std::vector<hpu::EncodedInstruction> instructions;
    std::vector<PendingDma> dma;
    ResourceReport report;
};

class Scheduler {
public:
    Scheduler(VirtualProgram& program, const CompileOptions& options)
        : program_(program), options_(options), states_(program.objects.size()) {}

    ScheduledProgram run()
    {
        for (cursor_ = 0; cursor_ < program_.instructions.size(); ++cursor_) {
            const auto op = program_.instructions[cursor_];
            current_origin_ = op.origin.value_or(ResolvedDma{});
            excluded_.clear();
            excluded_.insert(op.uses.begin(), op.uses.end());
            excluded_.insert(op.definitions.begin(), op.definitions.end());
            switch (op.instruction.mnemonic) {
            case hpu::Mnemonic::kDload: load(op); break;
            case hpu::Mnemonic::kDstore: store(op); break;
            case hpu::Mnemonic::kPfree:
                if (next_use(op.uses.at(0)) == kNoUse) release(op.uses.at(0));
                break;
            case hpu::Mnemonic::kPmodld:
                emit(op.instruction);
                break;
            case hpu::Mnemonic::kPsync:
                for (ObjectId id = 0; id < states_.size(); ++id) release(id);
                emit(op.instruction);
                break;
            default: kernel(op); break;
            }
        }
        hpu::validate_executable_program(result_.instructions);
        result_.report.dma_count = result_.dma.size();
        return std::move(result_);
    }

private:
    struct State {
        std::optional<std::uint8_t> slot;
        std::optional<BufferRange> home;
        std::optional<BufferId> spill;
        bool dirty = false;
        bool initialized = false;
    };

    std::size_t next_use(ObjectId id) const
    {
        for (std::size_t index = cursor_ + 1; index < program_.instructions.size(); ++index) {
            const auto& op = program_.instructions[index];
            if (op.instruction.mnemonic == hpu::Mnemonic::kPfree) continue;
            if (std::find(op.uses.begin(), op.uses.end(), id) != op.uses.end()
                || (op.reload_retained_object
                    && std::find(op.definitions.begin(), op.definitions.end(), id)
                        != op.definitions.end())) {
                return index;
            }
        }
        return kNoUse;
    }

    bool small(ObjectId id) const { return program_.objects[id].dma_flag != 0; }

    std::size_t banks() const
    {
        std::size_t result = 0;
        for (ObjectId id = 0; id < states_.size(); ++id) {
            if (states_[id].slot && !small(id)) ++result;
        }
        return result;
    }

    void measure(std::size_t extra = 0)
    {
        const auto regular = banks() + extra;
        std::size_t slots = 0;
        std::uint64_t small_lines = 0;
        for (ObjectId id = 0; id < states_.size(); ++id) {
            if (!states_[id].slot) continue;
            ++slots;
            if (small(id)) small_lines += program_.objects[id].line_count;
        }
        result_.report.peak_regular_banks = std::max(result_.report.peak_regular_banks, regular);
        result_.report.peak_object_slots = std::max(result_.report.peak_object_slots, slots);
        if (regular > options_.regular_bank_count) {
            throw std::logic_error("scheduler exceeded regular bank capacity");
        }
        if (small_lines > 32) {
            throw std::invalid_argument("schedule exceeds small bank capacity");
        }
    }

    void emit(hpu::Instruction instruction) { result_.instructions.push_back(encode(instruction)); }

    void emit_dma(hpu::Instruction instruction, BufferRange range, ResolvedDma origin)
    {
        const std::size_t index = result_.instructions.size();
        emit(instruction);
        origin.instruction_index = index;
        result_.dma.push_back({index, range, std::move(origin)});
        auto& buffer = program_.buffers[range.buffer];
        buffer.first_use = std::min(buffer.first_use, cursor_);
        buffer.last_use = std::max(buffer.last_use, cursor_);
    }

    void release(ObjectId id)
    {
        auto& state = states_[id];
        if (!state.slot) return;
        hpu::Instruction free;
        free.mnemonic = hpu::Mnemonic::kPfree;
        free.idx0 = *state.slot;
        free.idx1 = 0;
        emit(free);
        state.slot.reset();
    }

    void evict(ObjectId id)
    {
        auto& state = states_[id];
        if (state.dirty && next_use(id) != kNoUse) {
            if (!state.spill) {
                VirtualBuffer buffer;
                buffer.id = "$compiler/spill/" + std::to_string(id);
                buffer.original_span = {0, program_.objects[id].line_count};
                buffer.word_count = buffer.original_span.line_count
                    * hpu::runtime::kHpuMemLineWords;
                buffer.retained = false;
                buffer.fully_defined_before_read = true;
                buffer.first_use = cursor_;
                buffer.last_use = cursor_;
                state.spill = program_.buffers.size();
                program_.buffers.push_back(std::move(buffer));
            }
            const BufferRange spill{*state.spill, 0, program_.objects[id].line_count};
            hpu::Instruction store;
            store.mnemonic = hpu::Mnemonic::kDstore;
            store.rs1 = 10;
            store.rs2 = 11;
            store.obj_id = *state.slot;
            store.type = 0;
            auto origin = current_origin_;
            emit_dma(store, spill, std::move(origin));
            state.slot.reset();
            state.home = spill;
            state.dirty = false;
            ++result_.report.spills;
        } else {
            release(id);
        }
    }

    void make_room(std::size_t requested_banks, bool need_slot)
    {
        auto available_slot = [&] {
            std::array<bool, 8> used{};
            used[4] = true;
            for (const auto& state : states_) if (state.slot) used[*state.slot] = true;
            return std::find(used.begin(), used.end(), false) != used.end();
        };
        while (banks() + requested_banks > options_.regular_bank_count
               || (need_slot && !available_slot())) {
            std::optional<ObjectId> victim;
            std::size_t farthest = 0;
            for (ObjectId id = 0; id < states_.size(); ++id) {
                if (!states_[id].slot || excluded_.count(id) || small(id)) continue;
                const auto next = next_use(id);
                if (!victim || next > farthest || (next == farthest && id < *victim)) {
                    victim = id;
                    farthest = next;
                }
            }
            if (!victim) {
                throw std::invalid_argument("kernel operands exceed regular bank capacity");
            }
            evict(*victim);
        }
    }

    std::uint8_t allocate(ObjectId id)
    {
        if (program_.objects[id].line_count > options_.regular_bank_lines && !small(id)) {
            throw std::invalid_argument("virtual object exceeds one regular bank");
        }
        const bool modulus = small(id) && program_.objects[id].type == 2;
        if (modulus) {
            for (ObjectId other = 0; other < states_.size(); ++other) {
                if (states_[other].slot == 4) release(other);
            }
            states_[id].slot = 4;
        } else {
            make_room(small(id) ? 0 : 1, true);
            std::array<bool, 8> used{};
            used[4] = true;
            for (const auto& state : states_) if (state.slot) used[*state.slot] = true;
            const auto first = std::find(used.begin(), used.end(), false);
            states_[id].slot = static_cast<std::uint8_t>(first - used.begin());
        }
        measure();
        return *states_[id].slot;
    }

    void ensure(ObjectId id)
    {
        auto& state = states_[id];
        if (state.slot) return;
        if (!state.initialized || !state.home) {
            throw std::logic_error("virtual object has no reloadable definition");
        }
        const auto slot = allocate(id);
        hpu::Instruction load;
        load.mnemonic = hpu::Mnemonic::kDload;
        load.rs1 = 10;
        load.rs2 = 11;
        load.obj_id = slot;
        load.type = program_.objects[id].type;
        load.dma_flag = program_.objects[id].dma_flag;
        auto origin = current_origin_;
        emit_dma(load, *state.home, std::move(origin));
        ++result_.report.reloads;
    }

    void load(const VirtualInstruction& op)
    {
        const ObjectId id = op.definitions.at(0);
        auto& state = states_[id];
        if (op.reload_retained_object) {
            ensure(id);
            return;
        }
        state.home = op.memory;
        state.initialized = true;
        state.dirty = false;
        auto instruction = op.instruction;
        instruction.obj_id = allocate(id);
        emit_dma(instruction, *op.memory, *op.origin);
    }

    void store(const VirtualInstruction& op)
    {
        const ObjectId id = op.uses.at(0);
        if (!op.required_store) {
            ++result_.report.deferred_scratch_stores;
            return;
        }
        ensure(id);
        auto instruction = op.instruction;
        instruction.obj_id = *states_[id].slot;
        emit_dma(instruction, *op.memory, *op.origin);
        states_[id].slot.reset();
        states_[id].home = op.memory;
        states_[id].dirty = false;
    }

    void kernel(const VirtualInstruction& op)
    {
        for (ObjectId id : op.uses) ensure(id);
        const ObjectId destination = op.definitions.at(0);
        if (!states_[destination].slot) allocate(destination);
        const bool stage = op.instruction.mnemonic == hpu::Mnemonic::kPntt
            || op.instruction.mnemonic == hpu::Mnemonic::kPintt;
        if (stage) make_room(1, false);
        auto instruction = op.instruction;
        instruction.pdst = *states_[destination].slot;
        if (stage) {
            // STG uses begin with data, then twiddle.
            instruction.psrc1 = *states_[op.uses.at(1)].slot;
        } else {
            instruction.psrc1 = *states_[op.uses.at(0)].slot;
            if (instruction.imm8 < 0) instruction.psrc2 = *states_[op.uses.at(1)].slot;
        }
        emit(instruction);
        states_[destination].initialized = true;
        states_[destination].dirty = true;
        measure(stage ? 1 : 0);
    }

    VirtualProgram& program_;
    const CompileOptions& options_;
    std::vector<State> states_;
    ScheduledProgram result_;
    std::set<ObjectId> excluded_;
    std::size_t cursor_ = 0;
    ResolvedDma current_origin_;
};

std::vector<LogicalBufferPlacement> place_buffers(
    const VirtualProgram& program,
    const hpu::runtime::HpuMemImage& original,
    hpu::runtime::HpuMemImage& image,
    std::uint64_t& arena_lines)
{
    std::vector<LogicalBufferPlacement> result(program.buffers.size());
    std::vector<BufferId> scratch;
    for (BufferId id = 0; id < program.buffers.size(); ++id) {
        const auto& buffer = program.buffers[id];
        auto& placement = result[id];
        placement.logical_id = buffer.id;
        placement.original_span = buffer.original_span;
        placement.retained = buffer.retained;
        placement.first_use = buffer.first_use;
        placement.last_use = buffer.last_use;
        placement.layout = buffer.layout;
        if (!buffer.retained) {
            scratch.push_back(id);
            continue;
        }
        const auto begin = original.words().begin()
            + buffer.original_span.line_offset * hpu::runtime::kHpuMemLineWords;
        std::vector<std::uint32_t> words(begin, begin + buffer.word_count);
        const auto allocation = image.add(buffer.id, words, buffer.kind, buffer.read_only);
        placement.physical_allocation_id = allocation.id;
        placement.span = allocation.span;
    }
    std::sort(scratch.begin(), scratch.end(), [&](BufferId a, BufferId b) {
        const auto& left = program.buffers[a];
        const auto& right = program.buffers[b];
        return left.first_use != right.first_use
            ? left.first_use < right.first_use : a < b;
    });
    std::vector<BufferId> placed;
    arena_lines = 0;
    for (BufferId id : scratch) {
        const auto& buffer = program.buffers[id];
        std::vector<hpu::runtime::HpuMemSpan> occupied;
        for (BufferId previous : placed) {
            const auto& other = program.buffers[previous];
            if (other.first_use <= buffer.last_use && buffer.first_use <= other.last_use) {
                occupied.push_back(result[previous].span);
            }
        }
        std::sort(occupied.begin(), occupied.end(), [](auto a, auto b) {
            return a.line_offset < b.line_offset;
        });
        std::uint64_t offset = 0;
        for (const auto& span : occupied) {
            if (offset + buffer.original_span.line_count <= span.line_offset) break;
            offset = std::max(offset, span.line_offset + span.line_count);
        }
        result[id].physical_allocation_id = kScratchArena;
        result[id].span = {offset, buffer.original_span.line_count};
        arena_lines = std::max(arena_lines, offset + buffer.original_span.line_count);
        placed.push_back(id);
    }
    if (arena_lines) {
        const auto arena = image.reserve(kScratchArena,
            arena_lines * hpu::runtime::kHpuMemLineWords,
            hpu::runtime::AllocationKind::workspace);
        for (BufferId id : scratch) result[id].span.line_offset += arena.span.line_offset;
    }
    image.trim_capacity_to_used_lines();
    return result;
}

} // namespace

VirtualProgram import_resolved_program(
    const std::vector<hpu::EncodedInstruction>& instructions,
    const std::vector<ResolvedDma>& dma,
    const hpu::runtime::HpuMemImage& image)
{
    hpu::validate_executable_program(instructions);
    const auto lookup = dma_by_instruction(instructions, dma);
    const auto pairs = deferred_store_pairs(instructions, dma, image);
    std::map<std::size_t, ObjectId> pending_reload;
    std::array<std::optional<ObjectId>, 8> live;
    std::map<std::string, BufferId> buffer_ids;
    VirtualProgram result;
    for (const auto& allocation : image.allocations()) {
        VirtualBuffer buffer;
        buffer.id = allocation.id;
        buffer.original_span = allocation.span;
        buffer.word_count = allocation.word_count;
        buffer.kind = allocation.kind;
        buffer.read_only = allocation.read_only;
        buffer_ids.emplace(buffer.id, result.buffers.size());
        result.buffers.push_back(std::move(buffer));
    }
    auto object = [&](int slot) -> ObjectId {
        if (slot < 0 || slot >= 8 || !live[slot]) {
            throw std::invalid_argument("virtual instruction references a released object");
        }
        return *live[slot];
    };
    std::optional<ResolvedDma> origin;
    for (std::size_t index = 0; index < instructions.size(); ++index) {
        const auto& instruction = instructions[index].instruction;
        VirtualInstruction op;
        op.instruction = instruction;
        if (lookup[index]) {
            origin = dma[*lookup[index]];
            const auto& allocation = image.allocation(origin->allocation_id);
            if (!contains(allocation.span, origin->span)) {
                throw std::invalid_argument("DMA range exceeds allocation: " + allocation.id);
            }
            if (instruction.mnemonic == hpu::Mnemonic::kDstore && allocation.read_only) {
                throw std::invalid_argument("DSTORE targets read-only allocation: " + allocation.id);
            }
            origin->allocation_id = allocation.id;
            op.memory = BufferRange{buffer_ids.at(allocation.id),
                origin->span.line_offset - allocation.span.line_offset,
                origin->span.line_count};
        }
        op.origin = origin;
        switch (instruction.mnemonic) {
        case hpu::Mnemonic::kDload: {
            const auto reused = pending_reload.find(*lookup[index]);
            ObjectId id;
            if (reused != pending_reload.end()) {
                id = reused->second;
                op.reload_retained_object = true;
            } else {
                id = result.objects.size();
                result.objects.push_back({op.memory->line_count, instruction.type,
                    instruction.dma_flag});
            }
            live[instruction.obj_id] = id;
            op.definitions.push_back(id);
            break;
        }
        case hpu::Mnemonic::kDstore: {
            const ObjectId id = object(instruction.obj_id);
            if (result.objects[id].line_count != op.memory->line_count) {
                throw std::invalid_argument("DSTORE length differs from resident object");
            }
            op.uses.push_back(id);
            const auto match = pairs.find(*lookup[index]);
            if (match != pairs.end()
                && instructions[dma[match->second].instruction_index].instruction.type
                    == result.objects[id].type
                && instructions[dma[match->second].instruction_index].instruction.dma_flag
                    == result.objects[id].dma_flag) {
                op.required_store = false;
                pending_reload.emplace(match->second, id);
            }
            live[instruction.obj_id].reset();
            break;
        }
        case hpu::Mnemonic::kPfree:
            op.uses.push_back(object(instruction.idx0));
            live[instruction.idx0].reset();
            break;
        case hpu::Mnemonic::kPntt:
        case hpu::Mnemonic::kPintt:
            op.uses.push_back(object(instruction.pdst));
            op.uses.push_back(object(instruction.psrc1));
            op.definitions.push_back(object(instruction.pdst));
            break;
        case hpu::Mnemonic::kPadd:
        case hpu::Mnemonic::kPsub:
        case hpu::Mnemonic::kPmul:
        case hpu::Mnemonic::kPmac: {
            op.uses.push_back(object(instruction.psrc1));
            if (instruction.imm8 < 0) op.uses.push_back(object(instruction.psrc2));
            if (instruction.mnemonic == hpu::Mnemonic::kPmac) {
                unique_push(op.uses, object(instruction.pdst));
            }
            if (!live[instruction.pdst]
                || result.objects[*live[instruction.pdst]].line_count
                    != result.objects[op.uses[0]].line_count) {
                if (instruction.mnemonic == hpu::Mnemonic::kPmac) {
                    throw std::invalid_argument("PMAC accumulator shape differs from source");
                }
                const ObjectId id = result.objects.size();
                const auto& source = result.objects[op.uses[0]];
                result.objects.push_back({source.line_count, source.type, 0});
                live[instruction.pdst] = id;
            }
            op.definitions.push_back(object(instruction.pdst));
            break;
        }
        default: break;
        }
        result.instructions.push_back(std::move(op));
    }
    analyze_buffers(result, dma, instructions, image);
    return result;
}

VirtualProgram import_operator_program(
    const hpu::resource::OperatorProgram& source,
    const std::vector<hpu::EncodedInstruction>& baseline,
    const std::vector<ResolvedDma>& dma,
    const hpu::runtime::HpuMemImage& image)
{
    using Role = hpu::resource::Role;
    if (source.operations.empty() || source.operations.size() != baseline.size())
        throw std::invalid_argument("operator IR does not cover the complete baseline program");
    const auto lookup = dma_by_instruction(baseline, dma);
    const auto pairs = deferred_store_pairs(baseline, dma, image);
    std::map<std::size_t, ObjectId> pending_reload;
    std::map<Role, ObjectId> live;
    std::map<std::string, BufferId> buffer_ids;
    VirtualProgram result;
    result.origin = ResourceIrOrigin::operator_emission;
    for (const auto& allocation : image.allocations()) {
        VirtualBuffer buffer;
        buffer.id = allocation.id;
        buffer.original_span = allocation.span;
        buffer.word_count = allocation.word_count;
        buffer.kind = allocation.kind;
        buffer.read_only = allocation.read_only;
        buffer_ids.emplace(buffer.id, result.buffers.size());
        result.buffers.push_back(std::move(buffer));
    }
    auto object = [&](hpu::resource::Operand operand) -> ObjectId {
        const auto found = live.find(operand.role);
        if (found == live.end()) throw std::invalid_argument("operator IR uses an undefined operand");
        return found->second;
    };
    std::optional<ResolvedDma> origin;
    for (std::size_t index = 0; index < source.operations.size(); ++index) {
        const auto& node = source.operations[index];
        const auto& physical = baseline[index].instruction;
        const auto& instruction = node.instruction;
        if (instruction.mnemonic != physical.mnemonic || instruction.imm8 != physical.imm8 ||
            instruction.mod_id != physical.mod_id || instruction.mode != physical.mode ||
            instruction.flag != physical.flag || instruction.type != physical.type ||
            instruction.dma_flag != physical.dma_flag ||
            ((instruction.mnemonic == hpu::Mnemonic::kPntt ||
              instruction.mnemonic == hpu::Mnemonic::kPintt) && instruction.idx0 != physical.idx0))
            throw std::invalid_argument("operator IR disagrees with its compatibility baseline");
        VirtualInstruction op;
        op.instruction = instruction;
        if (lookup[index]) {
            origin = dma[*lookup[index]];
            const auto& allocation = image.allocation(origin->allocation_id);
            if (!contains(allocation.span, origin->span))
                throw std::invalid_argument("operator DMA range exceeds allocation: " + allocation.id);
            if (instruction.mnemonic == hpu::Mnemonic::kDstore && allocation.read_only)
                throw std::invalid_argument("operator output targets read-only allocation: " + allocation.id);
            origin->allocation_id = allocation.id;
            op.memory = BufferRange{buffer_ids.at(allocation.id),
                origin->span.line_offset - allocation.span.line_offset, origin->span.line_count};
        }
        op.origin = origin;
        switch (instruction.mnemonic) {
        case hpu::Mnemonic::kDload: {
            const auto reused = pending_reload.find(*lookup[index]);
            ObjectId id;
            if (reused != pending_reload.end()) {
                id = reused->second;
                op.reload_retained_object = true;
            } else {
                id = result.objects.size();
                result.objects.push_back({op.memory->line_count, instruction.type, instruction.dma_flag});
            }
            live[node.outputs.at(0).role] = id;
            op.definitions.push_back(id);
            break;
        }
        case hpu::Mnemonic::kDstore: {
            const auto id = object(node.inputs.at(0));
            if (result.objects[id].line_count != op.memory->line_count)
                throw std::invalid_argument("operator output has an incompatible length");
            op.uses.push_back(id);
            const auto match = pairs.find(*lookup[index]);
            if (match != pairs.end() && baseline[dma[match->second].instruction_index].instruction.type
                    == result.objects[id].type &&
                baseline[dma[match->second].instruction_index].instruction.dma_flag == result.objects[id].dma_flag) {
                op.required_store = false;
                pending_reload.emplace(match->second, id);
            }
            live.erase(node.inputs.at(0).role);
            break;
        }
        case hpu::Mnemonic::kPfree:
            op.uses.push_back(object(node.inputs.at(0)));
            live.erase(node.inputs.at(0).role);
            break;
        case hpu::Mnemonic::kPntt:
        case hpu::Mnemonic::kPintt:
            op.uses = {object(node.inputs.at(0)), object(node.inputs.at(1))};
            op.definitions = {object(node.outputs.at(0))};
            break;
        case hpu::Mnemonic::kPadd:
        case hpu::Mnemonic::kPsub:
        case hpu::Mnemonic::kPmul:
        case hpu::Mnemonic::kPmac: {
            for (const auto input : node.inputs) op.uses.push_back(object(input));
            const auto destination = node.outputs.at(0).role;
            const auto found = live.find(destination);
            if (found == live.end() || result.objects[found->second].line_count != result.objects[op.uses.at(0)].line_count) {
                if (instruction.mnemonic == hpu::Mnemonic::kPmac)
                    throw std::invalid_argument("operator accumulator shape differs from source");
                const auto& operand = result.objects[op.uses.at(0)];
                live[destination] = result.objects.size();
                result.objects.push_back({operand.line_count, operand.type, 0});
            }
            op.definitions = {live.at(destination)};
            break;
        }
        case hpu::Mnemonic::kPmodld: break;
        case hpu::Mnemonic::kPsync: live.clear(); break;
        }
        result.instructions.push_back(std::move(op));
    }
    analyze_buffers(result, dma, baseline, image);
    return result;
}

CompiledResources optimize_resources(
    const std::vector<hpu::EncodedInstruction>& instructions,
    const std::vector<ResolvedDma>& dma,
    const hpu::runtime::HpuMemImage& image,
    const CompileOptions& options)
{
    return compile_resources(import_resolved_program(instructions, dma, image),
                             instructions, dma, image, options);
}

CompiledResources compile_resources(
    VirtualProgram program,
    const std::vector<hpu::EncodedInstruction>& instructions,
    const std::vector<ResolvedDma>& dma,
    const hpu::runtime::HpuMemImage& image,
    const CompileOptions& options)
{
    if (options.regular_bank_count == 0 || options.regular_bank_count > 5
        || options.regular_bank_lines == 0 || options.regular_bank_lines > 1024) {
        throw std::invalid_argument("invalid resource limits for HPU ISA");
    }
    for (const auto& annotation : options.buffer_layouts) {
        const auto found = std::find_if(program.buffers.begin(), program.buffers.end(),
            [&](const auto& buffer) { return buffer.id == annotation.first; });
        if (found == program.buffers.end())
            throw std::invalid_argument("layout annotation names an absent buffer: " + annotation.first);
        switch (annotation.second) {
        case MemoryLayout::unspecified:
        case MemoryLayout::coefficient_natural:
        case MemoryLayout::coefficient_bit_reversed:
        case MemoryLayout::canonical_ntt_physical: break;
        default: throw std::invalid_argument("unknown buffer layout annotation");
        }
        found->layout = annotation.second;
    }
    if (options.optimization == OptimizationMode::no_opt) {
        CompiledResources result(image.capacity_lines());
        result.image = image;
        result.instructions = instructions;
        result.dma = dma;
        result.report = inspect_original(instructions, dma, image, options);
        result.report.ir_origin = program.origin;
        for (const auto& buffer : program.buffers) {
            result.buffers.push_back({buffer.id, buffer.id, buffer.original_span,
                buffer.original_span, true, buffer.first_use, buffer.last_use, buffer.layout});
        }
        return result;
    }
    auto scheduled = Scheduler(program, options).run();
    std::uint64_t capacity = image.used_lines();
    for (const auto& buffer : program.buffers) {
        if (buffer.id.rfind("$compiler/spill/", 0) == 0) {
            capacity += buffer.original_span.line_count;
        }
    }
    CompiledResources result(std::max<std::uint64_t>(1, capacity));
    result.instructions = std::move(scheduled.instructions);
    result.report = scheduled.report;
    result.report.ir_origin = program.origin;
    result.report.original_dma_count = dma.size();
    result.report.original_ddr_lines = image.used_lines();
    result.buffers = place_buffers(program, image, result.image,
        result.report.scratch_arena_lines);
    result.report.allocated_ddr_lines = result.image.used_lines();
    result.dma.reserve(scheduled.dma.size());
    for (auto& entry : scheduled.dma) {
        const auto& placement = result.buffers[entry.range.buffer];
        entry.origin.instruction_index = entry.instruction_index;
        entry.origin.allocation_id = placement.physical_allocation_id;
        entry.origin.span = {placement.span.line_offset + entry.range.line_offset,
                            entry.range.line_count};
        result.dma.push_back(std::move(entry.origin));
    }
    return result;
}

hpu::runtime::HpuMemSpan relocate_span(
    const CompiledResources& resources,
    const std::string& logical_allocation_id,
    hpu::runtime::HpuMemSpan original_span)
{
    const auto found = std::find_if(resources.buffers.begin(), resources.buffers.end(),
        [&](const auto& buffer) { return buffer.logical_id == logical_allocation_id; });
    if (found == resources.buffers.end() || !contains(found->original_span, original_span)) {
        throw std::invalid_argument("cannot relocate logical range: " + logical_allocation_id);
    }
    return {found->span.line_offset + original_span.line_offset - found->original_span.line_offset,
            original_span.line_count};
}

std::string render_resource_report(const CompiledResources& resources)
{
    const auto& report = resources.report;
    std::ostringstream output;
    output << "{\n  \"original_ddr_lines\": " << report.original_ddr_lines
           << ",\n  \"ir_source\": " << json_string(
               report.ir_origin == ResourceIrOrigin::operator_emission ? "operator" : "encoded_import")
           << ",\n  \"allocated_ddr_lines\": " << report.allocated_ddr_lines
           << ",\n  \"scratch_arena_lines\": " << report.scratch_arena_lines
           << ",\n  \"original_dma_count\": " << report.original_dma_count
           << ",\n  \"dma_count\": " << report.dma_count
           << ",\n  \"peak_regular_banks\": " << report.peak_regular_banks
           << ",\n  \"peak_object_slots\": " << report.peak_object_slots
           << ",\n  \"deferred_scratch_stores\": " << report.deferred_scratch_stores
           << ",\n  \"spills\": " << report.spills
           << ",\n  \"reloads\": " << report.reloads
           << ",\n  \"logical_buffers\": [";
    for (std::size_t index = 0; index < resources.buffers.size(); ++index) {
        const auto& buffer = resources.buffers[index];
        output << (index ? ",\n" : "\n")
               << "    {\"logical_id\": " << json_string(buffer.logical_id)
               << ", \"allocation_id\": " << json_string(buffer.physical_allocation_id)
               << ", \"original_line_offset\": " << buffer.original_span.line_offset
               << ", \"original_line_count\": " << buffer.original_span.line_count
               << ", \"line_offset\": " << buffer.span.line_offset
               << ", \"line_count\": " << buffer.span.line_count
               << ", \"retained\": " << (buffer.retained ? "true" : "false")
               << ", \"first_use\": " << buffer.first_use
               << ", \"last_use\": " << buffer.last_use
               << ", \"layout\": " << json_string(layout_name(buffer.layout)) << '}';
    }
    output << "\n  ]\n}\n";
    return output.str();
}

} // namespace hpu::compiler

namespace hpu::resource {
hpu::compiler::VirtualProgram bind_program(const OperatorProgram& program,
    const std::vector<hpu::EncodedInstruction>& baseline,
    const std::vector<hpu::compiler::ResolvedDma>& dma,
    const hpu::runtime::HpuMemImage& image)
{
    return hpu::compiler::import_operator_program(program, baseline, dma, image);
}
} // namespace hpu::resource
