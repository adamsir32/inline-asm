#include "hpu/compiler/ssa_optimization.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace hpu::resource {
namespace {
using Mnemonic = hpu::Mnemonic;
using Image = hpu::runtime::HpuMemImage;
using Span = hpu::runtime::HpuMemSpan;
constexpr auto none = std::numeric_limits<std::size_t>::max();

bool compute(Mnemonic kind)
{
    return kind == Mnemonic::kPadd || kind == Mnemonic::kPsub ||
           kind == Mnemonic::kPmul || kind == Mnemonic::kPmac ||
           kind == Mnemonic::kPntt || kind == Mnemonic::kPintt;
}
bool overlap(Span a, Span b)
{
    return a.line_offset < b.line_offset + b.line_count &&
           b.line_offset < a.line_offset + a.line_count;
}
bool equal(Span a, Span b)
{
    return a.line_offset == b.line_offset && a.line_count == b.line_count;
}
struct Node {
    Operation operation;
    std::optional<DataPort> port;
    std::vector<ValueId> implicit_inputs;
    std::vector<std::size_t> memory_dependencies;
    std::size_t original_index = 0;
};
struct Interval {
    Span span;
    std::size_t writer;
};
using Intervals = std::map<std::string, std::vector<Interval>>;

void overwrite(std::vector<Interval>& intervals, Span span, std::size_t writer)
{
    std::vector<Interval> next;
    for (const auto& item : intervals) {
        if (!overlap(item.span, span)) { next.push_back(item); continue; }
        if (item.span.line_offset < span.line_offset)
            next.push_back({{item.span.line_offset, span.line_offset - item.span.line_offset}, item.writer});
        const auto end = item.span.line_offset + item.span.line_count;
        const auto write_end = span.line_offset + span.line_count;
        if (write_end < end) next.push_back({{write_end, end - write_end}, item.writer});
    }
    if (writer != none) next.push_back({span, writer});
    intervals = std::move(next);
}
void intersecting(const std::vector<Interval>& intervals, Span span, std::vector<std::size_t>& found)
{
    for (const auto& item : intervals)
        if (overlap(item.span, span)) found.push_back(item.writer);
}

std::set<std::string> output_ids(const std::set<std::string>& requested, const Image& image)
{
    std::set<std::string> result;
    for (const auto& name : requested) result.insert(image.allocation(name).id);
    return result;
}

std::vector<Node> normalize(const OperatorProgram& source, const Image& image,
                            SsaOptimizationReport& report)
{
    std::vector<Node> nodes;
    std::vector<bool> defined(source.values.size());
    std::vector<bool> port_seen(source.ports.size());
    std::optional<int> context;
    std::optional<std::tuple<std::string, std::uint64_t, std::uint64_t>> modulus_identity;
    for (std::size_t index = 0; index < source.operations.size(); ++index) {
        auto operation = source.operations[index];
        const auto kind = operation.instruction.mnemonic;
        for (auto value : operation.inputs)
            if (value >= defined.size() || !defined[value])
                throw std::invalid_argument("SSA optimization encountered an undefined input");
        for (auto value : operation.outputs) {
            if (value >= defined.size() || defined[value] || source.values[value].parameter)
                throw std::invalid_argument("SSA optimization encountered a duplicate definition");
            defined[value] = true;
        }
        const bool memory = kind == Mnemonic::kDload || kind == Mnemonic::kDstore;
        if (memory != operation.port.has_value())
            throw std::invalid_argument("SSA optimization requires explicit memory ports");
        Node node;
        node.original_index = index;
        if (operation.port) {
            const auto id = *operation.port;
            if (id >= source.ports.size() || port_seen[id])
                throw std::invalid_argument("SSA optimization encountered a duplicated data port");
            port_seen[id] = true;
            node.port = source.ports[id];
            if (!node.port->binding || node.port->operation_index != index ||
                node.port->load != (kind == Mnemonic::kDload))
                throw std::invalid_argument("SSA optimization requires a consistent bound data port");
            auto& binding = *node.port->binding;
            const auto& allocation = image.allocation(binding.allocation_id);
            if (binding.span.line_count == 0 ||
                binding.span.line_offset < allocation.span.line_offset ||
                binding.span.line_offset - allocation.span.line_offset > allocation.span.line_count ||
                binding.span.line_count > allocation.span.line_count -
                    (binding.span.line_offset - allocation.span.line_offset))
                throw std::invalid_argument("SSA data reference exceeds its allocation");
            if (!node.port->load && allocation.read_only)
                throw std::invalid_argument("SSA data reference writes a read-only allocation");
            binding.allocation_id = allocation.id;
            if (node.port->load && operation.instruction.type == 1) {
                if (!allocation.read_only)
                    throw std::invalid_argument("native SSA requires an immutable modulus table");
                const auto identity = std::make_tuple(allocation.id, binding.span.line_offset, binding.span.line_count);
                if (modulus_identity && *modulus_identity != identity)
                    throw std::invalid_argument("native SSA requires a single modulus-table identity");
                modulus_identity = identity;
            }
        }
        if (kind == Mnemonic::kPmodld) {
            context = operation.instruction.mod_id;
            continue;
        }
        if (kind == Mnemonic::kPfree) {
            ++report.lifetime_hints_removed;
            continue;
        }
        if (compute(kind) && !operation.modulus_context) operation.modulus_context = context;
        // A source variable's previous assignment is not an allocation rule.
        operation.reuse.reset();
        if (kind == Mnemonic::kDstore) operation.instruction.type = 0;
        node.operation = std::move(operation);
        nodes.push_back(std::move(node));
    }
    if (std::find(port_seen.begin(), port_seen.end(), false) != port_seen.end())
        throw std::invalid_argument("SSA optimization encountered an unused data port");
    return nodes;
}

OperatorProgram materialize(const OperatorProgram& source, const std::vector<Node>& nodes)
{
    OperatorProgram result;
    result.values = source.values;
    std::optional<int> selected;
    for (const auto& node : nodes) {
        auto operation = node.operation;
        if (compute(operation.instruction.mnemonic) && operation.modulus_context &&
            selected != operation.modulus_context) {
            Operation select;
            select.instruction.mnemonic = Mnemonic::kPmodld;
            select.instruction.mod_id = *operation.modulus_context;
            result.operations.push_back(std::move(select));
            selected = operation.modulus_context;
        }
        if (node.port) {
            auto port = *node.port;
            port.operation_index = result.operations.size();
            port.binding->instruction_index = port.operation_index;
            operation.port = result.ports.size();
            result.ports.push_back(std::move(port));
        } else operation.port.reset();
        if (operation.instruction.mnemonic == Mnemonic::kDload && operation.instruction.type == 1)
            selected.reset();
        for (auto value : operation.outputs)
            result.current_values[result.values.at(value).variable.symbol] = value;
        result.operations.push_back(std::move(operation));
    }
    return result;
}

struct Shape {
    std::uint64_t lines = 0;
    unsigned type = 0;
    unsigned mask = 0;
};
struct CachedRead {
    Span span;
    ValueId value;
    Shape shape;
    hpu::compiler::MemoryLayout layout;
};

std::vector<Node> eliminate_io(std::vector<Node> nodes, const OperatorProgram& source,
                               const Image& image, const std::set<std::string>& required,
                               SsaOptimizationReport& report, bool reuse = true)
{
    std::vector<ValueId> replacement(source.values.size());
    for (ValueId i = 0; i < replacement.size(); ++i) replacement[i] = i;
    const auto resolved = [&](ValueId id) {
        while (replacement.at(id) != id) id = replacement.at(id);
        return id;
    };
    std::vector<Shape> shapes(source.values.size());
    std::map<std::string, std::vector<CachedRead>> reads, writes;
    Intervals last_writers;
    std::vector<Node> kept;
    std::optional<ValueId> modulus_table;
    std::vector<std::size_t> observed_writers;
    const auto observable = [&](const std::string& id) {
        return required.empty()
            ? image.allocation(id).kind != hpu::runtime::AllocationKind::workspace
            : required.count(id) != 0;
    };
    for (auto& node : nodes) {
        auto& operation = node.operation;
        for (auto& input : operation.inputs) input = resolved(input);
        const auto kind = operation.instruction.mnemonic;
        if (node.port) {
            const auto& binding = *node.port->binding;
            auto& cached = reads[binding.allocation_id];
            auto& stored = writes[binding.allocation_id];
            if (kind == Mnemonic::kDload) {
                const Shape requested{binding.span.line_count, operation.instruction.type, operation.instruction.dma_flag};
                const auto matches = [&](const CachedRead& candidate) {
                    return requested.type != 1 && equal(candidate.span, binding.span) && candidate.shape.lines == requested.lines &&
                        candidate.shape.type == requested.type && candidate.shape.mask == requested.mask &&
                        candidate.layout == node.port->layout;
                };
                const auto prior_read = std::find_if(cached.begin(), cached.end(), matches);
                const auto prior_store = std::find_if(stored.begin(), stored.end(), matches);
                if (reuse && (prior_read != cached.end() || prior_store != stored.end())) {
                    const auto value = prior_read != cached.end() ? prior_read->value : prior_store->value;
                    replacement.at(operation.outputs.at(0)) = value;
                    if (requested.type == 1) modulus_table = value;
                    if (prior_read != cached.end()) ++report.merged_reads;
                    else ++report.forwarded_reads;
                    continue;
                }
                const auto value = operation.outputs.at(0);
                shapes.at(value) = requested;
                cached.push_back({binding.span, value, requested, node.port->layout});
                intersecting(last_writers[binding.allocation_id], binding.span, node.memory_dependencies);
                if (requested.type == 1) modulus_table = value;
            } else {
                const auto invalidate = [&](std::vector<CachedRead>& entries) {
                    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const CachedRead& entry) {
                        return overlap(entry.span, binding.span);
                    }), entries.end());
                };
                invalidate(cached); invalidate(stored);
                const auto value = operation.inputs.at(0);
                stored.push_back({binding.span, value, shapes.at(value), node.port->layout});
                overwrite(last_writers[binding.allocation_id], binding.span, kept.size());
            }
        } else if (compute(kind)) {
            if (modulus_table) node.implicit_inputs.push_back(*modulus_table);
            auto shape = shapes.at(operation.inputs.at(0));
            if (!operation.tied_input) shape.mask = 0;
            shapes.at(operation.outputs.at(0)) = shape;
        } else if (kind == Mnemonic::kPsync) {
            // A synchronization point is also a conservative external-memory
            // visibility boundary. Never fold a read across it.
            reads.clear(); writes.clear();
            for (const auto& entry : last_writers)
                if (observable(entry.first))
                    for (const auto& writer : entry.second) observed_writers.push_back(writer.writer);
        }
        kept.push_back(std::move(node));
    }

    std::vector<std::size_t> producers(source.values.size(), none);
    for (std::size_t i = 0; i < kept.size(); ++i)
        for (auto value : kept[i].operation.outputs) producers.at(value) = i;
    std::vector<bool> live(kept.size());
    std::vector<std::size_t> pending = std::move(observed_writers);
    for (const auto& entry : last_writers) {
        if (observable(entry.first))
            for (const auto& writer : entry.second) pending.push_back(writer.writer);
    }
    for (std::size_t i = 0; i < kept.size(); ++i)
        if (kept[i].operation.instruction.mnemonic == Mnemonic::kPsync) pending.push_back(i);
    while (!pending.empty()) {
        const auto i = pending.back(); pending.pop_back();
        if (live.at(i)) continue;
        live[i] = true;
        const auto& node = kept[i];
        for (auto value : node.operation.inputs)
            if (producers.at(value) != none) pending.push_back(producers[value]);
        for (auto value : node.implicit_inputs)
            if (producers.at(value) != none) pending.push_back(producers[value]);
        for (auto dependency : node.memory_dependencies) pending.push_back(dependency);
    }
    std::vector<Node> result;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (live[i]) result.push_back(std::move(kept[i]));
        else ++report.dead_operations;
    }
    // Dependency indexes referred to the pre-DCE node list. Reconstruct all
    // scheduling dependencies below from the retained data/memory operations.
    for (auto& node : result) node.memory_dependencies.clear();
    return result;
}

std::vector<Node> schedule(std::vector<Node> nodes, std::size_t value_count)
{
    const auto count = nodes.size();
    std::vector<std::set<std::size_t>> predecessors(count);
    std::vector<std::size_t> producer(value_count, none), remaining(value_count);
    std::vector<std::uint64_t> value_lines(value_count);
    Intervals writers, readers;
    std::optional<std::size_t> barrier;
    std::size_t segment_begin = 0;
    std::vector<std::size_t> context_users;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& node = nodes[i];
        auto& before = predecessors[i];
        if (barrier) before.insert(*barrier);
        const auto input = [&](ValueId value) {
            if (producer.at(value) != none) before.insert(producer[value]);
            ++remaining.at(value);
        };
        for (auto value : node.operation.inputs) input(value);
        for (auto value : node.implicit_inputs) input(value);
        for (auto value : node.operation.outputs) producer.at(value) = i;
        if (node.port) {
            if (node.port->load && node.operation.instruction.type == 1) {
                // MOD loads replace a fixed hardware context, even when their
                // DDR allocations differ. They follow every old-table user.
                before.insert(context_users.begin(), context_users.end());
                context_users.clear();
            }
            const auto& binding = *node.port->binding;
            std::vector<std::size_t> dependencies;
            intersecting(writers[binding.allocation_id], binding.span, dependencies);
            if (!node.port->load) {
                intersecting(readers[binding.allocation_id], binding.span, dependencies);
                overwrite(writers[binding.allocation_id], binding.span, i);
                overwrite(readers[binding.allocation_id], binding.span, none);
            } else {
                readers[binding.allocation_id].push_back({binding.span, i});
                value_lines.at(node.operation.outputs.at(0)) = binding.span.line_count;
            }
            before.insert(dependencies.begin(), dependencies.end());
        } else if (compute(node.operation.instruction.mnemonic)) {
            context_users.push_back(i);
            const auto size = value_lines.at(node.operation.inputs.at(0));
            for (auto value : node.operation.outputs) value_lines.at(value) = size;
        }
        if (node.operation.instruction.mnemonic == Mnemonic::kPsync) {
            for (std::size_t j = segment_begin; j < i; ++j) before.insert(j);
            barrier = i; segment_begin = i + 1;
            writers.clear(); readers.clear();
        }
    }
    std::vector<std::vector<std::size_t>> successors(count);
    std::vector<std::size_t> blockers(count);
    for (std::size_t i = 0; i < count; ++i) {
        blockers[i] = predecessors[i].size();
        for (auto dependency : predecessors[i]) successors.at(dependency).push_back(i);
    }
    std::set<std::size_t> ready;
    std::vector<bool> available(count), emitted(count);
    // Loads become available when their dependencies finish, but are emitted
    // only immediately before a selected consumer. This treats logical input
    // declarations as data sources rather than eager DMA scheduling decisions.
    const auto announce = [&](std::size_t first, auto&& self) -> void {
        available[first] = true;
        for (auto next : successors[first]) {
            if (--blockers[next] == 0) {
                if (nodes[next].operation.instruction.mnemonic == Mnemonic::kDload) self(next, self);
                else ready.insert(next);
            }
        }
    };
    for (std::size_t i = 0; i < count; ++i)
        if (blockers[i] == 0 && !available[i]) {
            if (nodes[i].operation.instruction.mnemonic == Mnemonic::kDload) announce(i, announce);
            else ready.insert(i);
        }
    std::vector<Node> result;
    result.reserve(count);
    std::uint64_t live_lines = 0;
    const auto append = [&](std::size_t i) {
        if (emitted[i]) return;
        emitted[i] = true;
        for (auto value : nodes[i].operation.outputs) {
            if (remaining[value]) live_lines += value_lines[value];
        }
        for (auto value : nodes[i].operation.inputs)
            if (remaining[value] && --remaining[value] == 0) live_lines -= value_lines[value];
        for (auto value : nodes[i].implicit_inputs)
            if (remaining[value] && --remaining[value] == 0) live_lines -= value_lines[value];
        result.push_back(nodes[i]);
    };
    while (!ready.empty()) {
        auto chosen = *ready.begin();
        auto best = std::make_tuple(std::numeric_limits<std::uint64_t>::max(),
                                    std::numeric_limits<std::uint64_t>::max(), none);
        for (auto candidate : ready) {
            std::uint64_t bytes = 0, pressure = live_lines;
            for (auto dependency : predecessors[candidate]) {
                if (!emitted[dependency] && nodes[dependency].port && nodes[dependency].port->load) {
                    bytes += nodes[dependency].port->binding->span.line_count * 256;
                    pressure += nodes[dependency].port->binding->span.line_count;
                }
            }
            for (auto output : nodes[candidate].operation.outputs) pressure += value_lines[output];
            for (auto input : nodes[candidate].operation.inputs)
                if (remaining[input] == 1 && pressure >= value_lines[input]) pressure -= value_lines[input];
            const auto score = std::make_tuple(bytes, pressure, nodes[candidate].original_index);
            if (score < best) { best = score; chosen = candidate; }
        }
        ready.erase(chosen);
        // Only loads are available before emission. Their own dependencies are
        // already emitted or similarly available loads; preserve that order.
        const auto materialize_reads = [&](std::size_t i, auto&& self) -> void {
            for (auto dependency : predecessors[i]) {
                if (!emitted[dependency]) {
                    if (!available[dependency] || nodes[dependency].operation.instruction.mnemonic != Mnemonic::kDload)
                        throw std::logic_error("SSA scheduling dependency is not ready");
                    self(dependency, self);
                    append(dependency);
                }
            }
        };
        materialize_reads(chosen, materialize_reads);
        append(chosen);
        announce(chosen, announce);
    }
    // A live standalone load can occur in an explicitly constructed fragment.
    for (std::size_t i = 0; i < count; ++i)
        if (!emitted[i] && available[i] && nodes[i].operation.instruction.mnemonic == Mnemonic::kDload) append(i);
    if (result.size() != nodes.size()) throw std::logic_error("SSA scheduling dependency graph is cyclic");
    return result;
}
} // namespace

OperatorProgram canonical_operator_program(const OperatorProgram& source,
    const hpu::runtime::HpuMemImage& image, const std::set<std::string>& required_outputs)
{
    (void)output_ids(required_outputs, image);
    SsaOptimizationReport report;
    return materialize(source, normalize(source, image, report));
}

OperatorProgram output_root_operator_program(const OperatorProgram& source,
    const hpu::runtime::HpuMemImage& image, const std::set<std::string>& required_outputs,
    SsaOptimizationReport* output_report)
{
    SsaOptimizationReport report;
    report.original_operations = source.operations.size();
    const auto nodes = eliminate_io(normalize(source, image, report), source, image,
                                   output_ids(required_outputs, image), report, false);
    auto result = materialize(source, nodes);
    report.operations = result.operations.size();
    if (output_report) *output_report = report;
    return result;
}

OperatorProgram optimize_operator_program(const OperatorProgram& source,
    const hpu::runtime::HpuMemImage& image, const std::set<std::string>& required_outputs,
    bool reorder, SsaOptimizationReport* output_report)
{
    SsaOptimizationReport report;
    report.original_operations = source.operations.size();
    auto nodes = eliminate_io(normalize(source, image, report), source, image,
                              output_ids(required_outputs, image), report);
    if (reorder) {
        const auto ordered = schedule(nodes, source.values.size());
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].original_index != ordered[i].original_index) report.reordered = true;
        nodes = ordered;
    }
    auto result = materialize(source, nodes);
    report.operations = result.operations.size();
    if (output_report) *output_report = report;
    return result;
}

} // namespace hpu::resource
