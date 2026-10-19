#pragma once

#include "hpu/compiler/resource_ir.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace hpu::compiler::detail {

// Sparse event lists: construction and traversal visit actual uses, never the
// Cartesian product of instruction positions and all virtual objects.
class UseIndex {
public:
    static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

    explicit UseIndex(const VirtualProgram& program)
        : positions_(program.objects.size()), cursors_(program.objects.size())
    {
        for (std::size_t i = 0; i < program.instructions.size(); ++i) {
            const auto& op = program.instructions[i];
            if (op.instruction.mnemonic == hpu::Mnemonic::kPfree) continue;
            for (const auto id : op.uses) add(id, i);
            if (op.reload_retained_object)
                for (const auto id : op.definitions) add(id, i);
        }
    }

    void consume(const VirtualInstruction& op, std::size_t position)
    {
        if (op.instruction.mnemonic == hpu::Mnemonic::kPfree) return;
        for (const auto id : op.uses) consume(id, position);
        if (op.reload_retained_object)
            for (const auto id : op.definitions) consume(id, position);
    }

    std::size_t next(ObjectId id) const
    {
        const auto& uses = positions_.at(id);
        const auto cursor = cursors_.at(id);
        return cursor == uses.size() ? none : uses[cursor];
    }

private:
    void add(ObjectId id, std::size_t position)
    {
        auto& uses = positions_.at(id);
        if (uses.empty() || uses.back() != position) uses.push_back(position);
    }
    void consume(ObjectId id, std::size_t position)
    {
        auto& cursor = cursors_.at(id);
        const auto& uses = positions_.at(id);
        while (cursor < uses.size() && uses[cursor] <= position) ++cursor;
    }
    std::vector<std::vector<std::size_t>> positions_;
    std::vector<std::size_t> cursors_;
};

} // namespace hpu::compiler::detail
