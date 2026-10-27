#include "evaluator_application.hpp"

#include <seal/seal.h>
#include <seal/util/numth.h>
#include <kuku/kuku.h>
#include <kuku/locfunc.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kOprfFixtureKey = UINT64_C(0x4150534948505531);
constexpr std::uint64_t kSealSeed = UINT64_C(0x41505349434f4e43);

struct Profile {
    std::string name;
    std::size_t sender_size;
    std::size_t receiver_size;
    std::size_t intersection_size;
    std::size_t degree;
    std::uint64_t plain_modulus;
    std::size_t felts_per_item;
    std::size_t hash_function_count;
    std::size_t table_size;
    std::size_t max_items_per_bin;
    std::uint32_t ps_low_degree;
    std::set<std::uint32_t> query_powers;
    std::string parameter_origin;
};

struct Options {
    Profile profile;
    std::string scenario;
    std::string match_mode;
    evaluator_example::Options evaluator;
};

struct Token {
    std::uint64_t low = 0;
    std::uint64_t high = 0;

    bool operator==(const Token& other) const noexcept
    {
        return low == other.low && high == other.high;
    }
};

// Explicit regression-input boundary. The default workload path does not use
// injected tokens unless --test-input is supplied.
bool g_test_input = false;
bool g_test_require_fixture_tokens = false;
bool g_test_publish_mismatch = false;
bool g_trace_vectors = false;
std::vector<std::uint64_t> g_test_sender;
std::vector<std::uint64_t> g_test_receiver;
std::map<std::uint64_t, Token> g_test_tokens;

void load_test_input(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file) throw std::runtime_error("cannot open --test-input file");
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream input(line);
        char side = 0;
        std::uint64_t item = 0, low = 0, high = 0;
        std::string extra;
        if (!(input >> side >> item >> low >> high) || (input >> extra) ||
            (side != 'S' && side != 'R'))
            throw std::invalid_argument("invalid --test-input line");
        const Token value{low, high};
        const auto previous = g_test_tokens.find(item);
        if (previous != g_test_tokens.end() && !(previous->second == value))
            throw std::invalid_argument("conflicting test token for item");
        g_test_tokens[item] = value;
        (side == 'S' ? g_test_sender : g_test_receiver).push_back(item);
    }
    if (g_test_sender.empty() || g_test_receiver.empty())
        throw std::invalid_argument("test input needs sender and receiver items");
    g_test_input = true;
}

struct CuckooEntry {
    bool occupied = false;
    std::uint64_t item = 0;
    Token token;
    std::size_t receiver_index = 0;
};

struct QueryLayout {
    std::vector<CuckooEntry> table;
    std::vector<std::size_t> item_to_table;
};

struct Dataset {
    std::vector<std::uint64_t> sender;
    std::vector<std::uint64_t> receiver;
    std::vector<std::uint64_t> expected_intersection;
};

struct PowersNode {
    std::uint32_t power = 0;
    std::uint32_t depth = 0;
    bool source = false;
    std::pair<std::uint32_t, std::uint32_t> parents{};
};

struct OpCounts {
    std::size_t ciphertext_multiply = 0;
    std::size_t square = 0;
    std::size_t relinearize = 0;
    std::size_t multiply_plain = 0;
    std::size_t add = 0;
    std::size_t sub = 0;
    std::size_t add_plain = 0;
    std::size_t mod_switch = 0;

    std::size_t total() const noexcept
    {
        return ciphertext_multiply + square + relinearize + multiply_plain + add + sub +
               add_plain + mod_switch;
    }
};

struct PackageSummary {
    std::string directory;
    std::size_t nodes = 0;
    std::size_t instructions = 0;
    std::size_t dma_spans = 0;
    std::uint64_t memory_lines = 0;
};

void accumulate(OpCounts& total, const OpCounts& value)
{
    total.ciphertext_multiply += value.ciphertext_multiply;
    total.square += value.square;
    total.relinearize += value.relinearize;
    total.multiply_plain += value.multiply_plain;
    total.add += value.add;
    total.sub += value.sub;
    total.add_plain += value.add_plain;
    total.mod_switch += value.mod_switch;
}

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t splitmix64(std::uint64_t value)
{
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

Token test_oprf(std::uint64_t item)
{
    // Compute the original fixture regardless of the test input mode, so the
    // genuine-token switch can check reachability without trusting the file.
    const auto low = splitmix64(item ^ kOprfFixtureKey);
    const auto high = splitmix64(low ^ (item << 1) ^ UINT64_C(0xd6e8feb86659fd93));
    const Token fixture{low ? low : 1, high};
    if (g_test_input) {
        const auto found = g_test_tokens.find(item);
        if (found == g_test_tokens.end())
            throw std::invalid_argument("test item has no injected token");
        if (g_test_require_fixture_tokens && !(found->second == fixture))
            throw std::invalid_argument("test token does not equal test_oprf(item)");
        return found->second;
    }
    // This deliberately deterministic fixture represents the output boundary of
    // APSI's FourQ OPRF. It is not a replacement for the cryptographic OPRF.
    return fixture;
}

std::size_t hash_location(const Token& token, std::size_t function,
                          std::size_t table_size)
{
    const auto mixed = splitmix64(token.low ^
        splitmix64(token.high + UINT64_C(0x9e3779b97f4a7c15) * (function + 1)));
    return static_cast<std::size_t>(mixed % table_size);
}

std::vector<std::size_t> all_locations(const Token& token, const Profile& profile)
{
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < profile.hash_function_count; ++i) {
        const auto location = hash_location(token, i, profile.table_size);
        if (std::find(result.begin(), result.end(), location) == result.end())
            result.push_back(location);
    }
    return result;
}

kuku::item_type kuku_item(const Token& token)
{
    return kuku::make_item(token.low, token.high);
}

std::vector<std::size_t> trident_locations(const Token& token, const Profile& profile)
{
    // Trident SenderDB constructs its Kuku location functions once per
    // database. Reuse the same keyed tables across all sender items.
    static thread_local std::vector<kuku::LocFunc> hash_functions;
    static thread_local std::size_t cached_table_size = 0;
    if (cached_table_size != profile.table_size ||
        hash_functions.size() != profile.hash_function_count) {
        hash_functions.clear();
        hash_functions.reserve(profile.hash_function_count);
        for (std::size_t i = 0; i < profile.hash_function_count; ++i)
            hash_functions.emplace_back(profile.table_size, kuku::make_item(i, 0));
        cached_table_size = profile.table_size;
    }
    // SenderDB::all_locations uses an unordered_set, including its iteration
    // order when placing an item into BinBundles.
    std::unordered_set<std::size_t> locations;
    for (const auto& hash : hash_functions)
        locations.insert(hash(kuku_item(token)));
    return {locations.begin(), locations.end()};
}

QueryLayout build_trident_cuckoo_layout(const std::vector<std::uint64_t>& items,
                                         const Profile& profile)
{
    kuku::KukuTable cuckoo(profile.table_size, 0,
        static_cast<std::uint32_t>(profile.hash_function_count),
        kuku::make_item(0, 0), 500, kuku::make_item(0, 0));
    QueryLayout result;
    result.table.resize(profile.table_size);
    result.item_to_table.resize(items.size(), profile.table_size);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto token = test_oprf(items[i]);
        if (!cuckoo.insert(kuku_item(token)))
            require(cuckoo.is_empty_item(cuckoo.leftover_item()),
                    "Trident Kuku receiver insertion exhausted its probe budget");
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto token = test_oprf(items[i]);
        const auto location = cuckoo.query(kuku_item(token)).location();
        require(location < profile.table_size, "Trident Kuku query lost a token");
        result.table[location] = {true, items[i], token, i};
        result.item_to_table[i] = location;
    }
    return result;
}

std::vector<std::uint64_t> algebraize(const Token& token, std::size_t felt_count,
                                      std::uint32_t bits_per_felt)
{
    require(felt_count * bits_per_felt <= 128 + bits_per_felt - 1,
            "APSI item algebraization exceeds one padded OPRF token");
    std::vector<std::uint64_t> result(felt_count);
    for (std::size_t felt = 0; felt < felt_count; ++felt) {
        std::uint64_t value = 0;
        for (std::uint32_t bit = 0; bit < bits_per_felt; ++bit) {
            const auto source = felt * bits_per_felt + bit;
            const auto set = source >= 128 ? std::uint64_t{0} : source < 64
                ? ((token.low >> source) & 1U)
                : ((token.high >> (source - 64)) & 1U);
            value |= set << bit;
        }
        result[felt] = value;
    }
    return result;
}

QueryLayout build_cuckoo_layout(const std::vector<std::uint64_t>& items,
                                const Profile& profile)
{
    QueryLayout result;
    result.table.resize(profile.table_size);
    result.item_to_table.resize(items.size(), profile.table_size);
    for (std::size_t input_index = 0; input_index < items.size(); ++input_index) {
        CuckooEntry pending{true, items[input_index], test_oprf(items[input_index]), input_index};
        std::size_t choice = 0;
        bool inserted = false;
        for (std::size_t attempt = 0; attempt < 500; ++attempt) {
            const auto locations = all_locations(pending.token, profile);
            require(!locations.empty(), "APSI cuckoo item has no candidate location");
            const auto location = locations[choice % locations.size()];
            if (!result.table[location].occupied) {
                result.table[location] = pending;
                inserted = true;
                break;
            }
            std::swap(pending, result.table[location]);
            const auto displaced = all_locations(pending.token, profile);
            const auto current = std::find(displaced.begin(), displaced.end(), location);
            choice = current == displaced.end()
                ? 0
                : (static_cast<std::size_t>(current - displaced.begin()) + 1) % displaced.size();
        }
        require(inserted, "APSI receiver cuckoo hashing exhausted its eviction budget");
    }
    for (std::size_t location = 0; location < result.table.size(); ++location)
        if (result.table[location].occupied)
            result.item_to_table[result.table[location].receiver_index] = location;
    require(std::none_of(result.item_to_table.begin(), result.item_to_table.end(),
                         [&](std::size_t value) { return value == profile.table_size; }),
            "APSI cuckoo translation table lost an input item");
    return result;
}

std::uint64_t add_mod(std::uint64_t left, std::uint64_t right, std::uint64_t modulus)
{
    const auto sum = left + right;
    return sum >= modulus ? sum - modulus : sum;
}

std::uint64_t multiply_mod(std::uint64_t left, std::uint64_t right,
                           std::uint64_t modulus)
{
    return (left * right) % modulus;
}

std::uint64_t power_mod(std::uint64_t value, std::uint32_t power,
                        std::uint64_t modulus)
{
    std::uint64_t result = 1;
    while (power) {
        if (power & 1U) result = multiply_mod(result, value, modulus);
        value = multiply_mod(value, value, modulus);
        power >>= 1U;
    }
    return result;
}

std::vector<std::uint64_t> root_polynomial(const std::vector<std::uint64_t>& roots,
                                           std::uint64_t modulus)
{
    if (roots.empty()) return {1};
    std::vector<std::uint64_t> coefficients{1};
    for (const auto root : roots) {
        std::vector<std::uint64_t> next(coefficients.size() + 1);
        const auto negative_root = root ? modulus - root : 0;
        for (std::size_t i = 0; i < coefficients.size(); ++i) {
            next[i] = add_mod(next[i],
                multiply_mod(coefficients[i], negative_root, modulus), modulus);
            next[i + 1] = add_mod(next[i + 1], coefficients[i], modulus);
        }
        coefficients = std::move(next);
    }
    return coefficients;
}

std::set<std::uint32_t> create_powers_set(std::uint32_t ps_low_degree,
                                          std::uint32_t target_degree)
{
    std::set<std::uint32_t> result;
    if (!ps_low_degree) {
        for (std::uint32_t power = 1; power <= target_degree; ++power) result.insert(power);
        return result;
    }
    for (std::uint32_t power = 1; power <= ps_low_degree; ++power) result.insert(power);
    const auto stride = ps_low_degree + 1;
    for (std::uint32_t power = stride; power <= target_degree; power += stride)
        result.insert(power);
    return result;
}

std::vector<PowersNode> configure_powers_dag(const std::set<std::uint32_t>& sources,
                                              const std::set<std::uint32_t>& targets)
{
    require(sources.count(1) && !sources.count(0) && targets.count(1) && !targets.count(0),
            "APSI PowersDag requires power one and rejects power zero");
    require(std::includes(targets.begin(), targets.end(), sources.begin(), sources.end()),
            "APSI PowersDag source powers are not a subset of target powers");
    std::map<std::uint32_t, PowersNode> nodes;
    for (const auto source : sources) nodes[source] = {source, 0, true, {0, 0}};
    for (const auto power : targets) {
        if (nodes.count(power)) continue;
        std::uint32_t optimal_depth = power - 1;
        std::pair<std::uint32_t, std::uint32_t> optimal{power - 1, 1};
        for (const auto left : targets) {
            if (left >= power) break;
            const auto right = power - left;
            if (!targets.count(right) || !nodes.count(left) || !nodes.count(right)) continue;
            const auto depth = std::max(nodes.at(left).depth, nodes.at(right).depth) + 1;
            if (depth < optimal_depth) {
                optimal_depth = depth;
                optimal = {left, right};
            }
        }
        require(nodes.count(optimal.first) && nodes.count(optimal.second),
                "APSI PowersDag target cannot be decomposed");
        nodes[power] = {power, optimal_depth, false, optimal};
    }
    std::vector<PowersNode> result;
    for (const auto& entry : nodes) result.push_back(entry.second);
    return result;
}

Profile small_profile()
{
    return {"small", 10, 5, 2, 8192, 65537, 8, 3, 4096, 16, 0, {1,3,5},
            "Poseidon Trident create_params1 / APSI UnlabeledSmall1"};
}

Profile medium_profile()
{
    return {"medium", 8192, 512, 32, 4096, 270337, 5, 3, 1638, 125, 5,
            {1,2,3,4,5,6,18,30,42,54,60},
            "Microsoft APSI 1M-1024-com parameter shape; sender/receiver counts reduced"};
}

Profile target_profile()
{
    auto profile = medium_profile();
    profile.name = "target";
    profile.sender_size = 1000000;
    profile.receiver_size = 1024;
    profile.intersection_size = 32;
    profile.parameter_origin =
        "Trident APSI parameters/1M-1024-com.json; 1M/1024 item workload";
    return profile;
}

Options parse_options(int argc, char** argv)
{
    Profile profile = small_profile();
    std::string scenario = "multi";
    std::string match_mode = "exact";
    bool optimize = false;
    std::string test_input_path;
    std::vector<char*> remaining{argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--profile") {
            if (++i >= argc) throw std::invalid_argument("--profile requires small, medium, or target");
            const std::string value(argv[i]);
            if (value == "small") profile = small_profile();
            else if (value == "medium") profile = medium_profile();
            else if (value == "target") profile = target_profile();
            else throw std::invalid_argument("--profile must be small, medium, or target");
        } else if (argument == "--scenario") {
            if (++i >= argc) throw std::invalid_argument("--scenario requires a value");
            scenario = argv[i];
        } else if (argument == "--match-mode") {
            if (++i >= argc) throw std::invalid_argument("--match-mode requires exact or trident");
            match_mode = argv[i];
            if (match_mode != "exact" && match_mode != "trident")
                throw std::invalid_argument("--match-mode must be exact or trident");
        } else if (argument == "--optimize-resources") {
            optimize = true;
        } else if (argument == "--test-input") {
            if (++i >= argc) throw std::invalid_argument("--test-input requires a file");
            test_input_path = argv[i];
        } else if (argument == "--test-require-fixture-tokens") {
            g_test_require_fixture_tokens = true;
        } else if (argument == "--test-publish-mismatch") {
            g_test_publish_mismatch = true;
        } else if (argument == "--trace-vectors") {
            g_trace_vectors = true;
        } else {
            remaining.push_back(argv[i]);
        }
    }
    if (profile.name != "small" && scenario != "multi")
        throw std::invalid_argument("medium and target profiles support the multi scenario only");
    if (profile.name == "small" && scenario != "empty" && scenario != "single" &&
        scenario != "multi" && scenario != "boundary")
        throw std::invalid_argument("small --scenario must be empty, single, multi, or boundary");
    auto evaluator = evaluator_example::options(
        static_cast<int>(remaining.size()), remaining.data());
    evaluator.compiler.optimize_resources = optimize;
    if (!test_input_path.empty()) load_test_input(test_input_path);
    return {std::move(profile), std::move(scenario), std::move(match_mode),
            std::move(evaluator)};
}

std::uint64_t find_item_for_location(std::size_t wanted, const Profile& profile,
                                     std::uint64_t start)
{
    for (std::uint64_t item = start; item < start + UINT64_C(10000000); ++item)
        if (hash_location(test_oprf(item), 0, profile.table_size) == wanted) return item;
    throw std::runtime_error("failed to synthesize APSI boundary fixture");
}

Dataset build_dataset(const Options& options)
{
    const auto& profile = options.profile;
    Dataset result;
    if (g_test_input) {
        result.sender = g_test_sender;
        result.receiver = g_test_receiver;
    } else if (profile.name == "medium" || profile.name == "target") {
        result.sender.resize(profile.sender_size);
        std::iota(result.sender.begin(), result.sender.end(), UINT64_C(1));
        for (std::size_t i = 0; i < profile.intersection_size; ++i)
            result.receiver.push_back(static_cast<std::uint64_t>(i + 1));
        for (std::size_t i = result.receiver.size(); i < profile.receiver_size; ++i)
            result.receiver.push_back(UINT64_C(1000000) + i);
    } else if (options.scenario == "boundary") {
        const auto first = find_item_for_location(0, profile, UINT64_C(2000000));
        const auto last = find_item_for_location(profile.table_size - 1, profile,
                                                 first + 1);
        result.sender = {first, last, 11,12,13,14,15,16,17,18};
        result.receiver = {first, last, 1001,1002,1003};
    } else {
        result.sender.resize(profile.sender_size);
        std::iota(result.sender.begin(), result.sender.end(), UINT64_C(1));
        if (options.scenario == "empty") result.receiver = {101,102,103,104,105};
        else if (options.scenario == "single") result.receiver = {1,101,102,103,104};
        else result.receiver = {1,4,101,102,103};
    }
    auto sorted_sender = result.sender;
    auto sorted_receiver = result.receiver;
    std::sort(sorted_sender.begin(), sorted_sender.end());
    std::sort(sorted_receiver.begin(), sorted_receiver.end());
    std::set_intersection(sorted_sender.begin(), sorted_sender.end(),
                          sorted_receiver.begin(), sorted_receiver.end(),
                          std::back_inserter(result.expected_intersection));
    return result;
}

using FullTokenFelts = std::array<std::uint64_t, 8>;
using CoefficientMatrix = std::vector<std::vector<std::uint64_t>>;

struct ExactSenderPolynomials {
    // [partition][bundle][result group][coefficient][batch slot]
    std::vector<std::vector<std::array<CoefficientMatrix, 2>>> coefficients;
    std::vector<std::vector<bool>> occupied;
    std::size_t maximum_degree = 0;
};

struct TridentSenderPolynomials {
    // One independent felt-root polynomial per batch slot and BinBundle.
    // [partition][bundle][coefficient][batch slot]
    std::vector<std::vector<CoefficientMatrix>> coefficients;
    std::vector<std::vector<bool>> occupied;
    // [partition][bundle][batch slot][roots]
    std::vector<std::vector<std::vector<std::vector<std::uint64_t>>>> roots;
    std::size_t maximum_degree = 0;
};

TridentSenderPolynomials build_trident_sender_coefficients(
    const Dataset& dataset, const Profile& profile, std::uint64_t plain_modulus,
    std::uint32_t bits_per_felt, std::size_t bundle_count, std::size_t items_per_bundle)
{
    using FeltBins = std::vector<std::vector<std::uint64_t>>;
    std::vector<std::vector<FeltBins>> bundles(bundle_count);
    for (const auto item : dataset.sender) {
        const auto token = test_oprf(item);
        const auto felts = algebraize(token, profile.felts_per_item, bits_per_felt);
        for (const auto location : trident_locations(token, profile)) {
            const auto bundle = location / items_per_bundle;
            const auto base = (location % items_per_bundle) * profile.felts_per_item;
            auto& partitions = bundles[bundle];
            std::size_t selected = partitions.size();
            for (std::size_t p = partitions.size(); p > 0; --p) {
                const auto& bins = partitions[p - 1];
                bool fit = true;
                for (std::size_t field = 0; field < profile.felts_per_item; ++field)
                    fit = fit && bins[base + field].size() + 1 < profile.max_items_per_bin;
                if (fit) { selected = p - 1; break; }
            }
            if (selected == partitions.size())
                partitions.emplace_back(profile.degree);
            for (std::size_t field = 0; field < profile.felts_per_item; ++field)
                partitions[selected][base + field].push_back(felts[field]);
        }
    }
    std::size_t partition_count = 0;
    for (const auto& bundle : bundles)
        partition_count = std::max(partition_count, bundle.size());
    require(partition_count > 0, "Trident sender database is empty");
    TridentSenderPolynomials result;
    result.coefficients.resize(partition_count,
        std::vector<CoefficientMatrix>(bundle_count));
    result.occupied.resize(partition_count, std::vector<bool>(bundle_count));
    result.roots.resize(partition_count,
        std::vector<FeltBins>(bundle_count));
    for (std::size_t bundle = 0; bundle < bundle_count; ++bundle) {
        for (std::size_t p = 0; p < bundles[bundle].size(); ++p) {
            const auto& bins = bundles[bundle][p];
            result.occupied[p][bundle] = true;
            result.roots[p][bundle] = bins;
            std::size_t degree = 0;
            for (const auto& roots : bins)
                degree = std::max(degree, roots.size());
            result.maximum_degree = std::max(result.maximum_degree, degree);
            auto& matrix = result.coefficients[p][bundle];
            matrix.assign(degree + 1, std::vector<std::uint64_t>(profile.degree));
            for (std::size_t slot = 0; slot < profile.degree; ++slot) {
                const auto coefficients = root_polynomial(bins[slot], plain_modulus);
                for (std::size_t c = 0; c < coefficients.size(); ++c)
                    matrix[c][slot] = coefficients[c];
            }
        }
    }
    return result;
}

std::vector<std::uint64_t> trident_cpu_reference(
    const QueryLayout& layout, const TridentSenderPolynomials& sender,
    const Profile& profile, std::uint32_t bits_per_felt, std::size_t items_per_bundle)
{
    std::vector<std::uint64_t> matches;
    for (std::size_t location = 0; location < layout.table.size(); ++location) {
        const auto& entry = layout.table[location];
        if (!entry.occupied) continue;
        const auto bundle = location / items_per_bundle;
        const auto base = (location % items_per_bundle) * profile.felts_per_item;
        const auto felts = algebraize(entry.token, profile.felts_per_item, bits_per_felt);
        for (std::size_t p = 0; p < sender.roots.size(); ++p) {
            if (!sender.occupied[p][bundle]) continue;
            bool match = true;
            for (std::size_t field = 0; field < profile.felts_per_item; ++field) {
                const auto& roots = sender.roots[p][bundle][base + field];
                match = match && std::find(roots.begin(), roots.end(), felts[field]) != roots.end();
            }
            if (match) { matches.push_back(entry.item); break; }
        }
    }
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    return matches;
}

FullTokenFelts full_token_felts(const Token& token, std::uint32_t bits_per_felt)
{
    require(bits_per_felt >= 16 && bits_per_felt <= 18,
            "exact APSI token encoding needs eight 16-to-18-bit felts");
    const auto values = algebraize(token, 8, bits_per_felt);
    FullTokenFelts result{};
    std::copy(values.begin(), values.end(), result.begin());
    return result;
}

// For distinct first felts x_i, L_j(x_i) equals the j-th felt of the SAME
// sender token. The matching polynomial F(x) gates the interpolation: without
// F(x)=0, arbitrary interpolated values must never be reported as a match.
std::array<std::vector<std::uint64_t>, 7> interpolate_token_felts(
    const std::vector<FullTokenFelts>& entries,
    const std::vector<std::uint64_t>& matching_polynomial,
    std::uint64_t modulus)
{
    const auto count = entries.size();
    std::array<std::vector<std::uint64_t>, 7> result;
    for (auto& polynomial : result) polynomial.assign(count, 0);
    for (std::size_t item = 0; item < count; ++item) {
        const auto x = entries[item][0];
        std::vector<std::uint64_t> quotient(count);
        quotient[count - 1] = matching_polynomial[count];
        for (std::size_t degree = count - 1; degree > 0; --degree)
            quotient[degree - 1] = add_mod(matching_polynomial[degree],
                multiply_mod(x, quotient[degree], modulus), modulus);
        std::uint64_t denominator = 0;
        for (auto coefficient = quotient.rbegin(); coefficient != quotient.rend(); ++coefficient)
            denominator = add_mod(multiply_mod(denominator, x, modulus),
                                  *coefficient, modulus);
        require(denominator != 0,
                "sender partition contains duplicate first-felt roots");
        const auto inverse = power_mod(denominator,
            static_cast<std::uint32_t>(modulus - 2), modulus);
        for (std::size_t label = 0; label < result.size(); ++label) {
            const auto weight = multiply_mod(entries[item][label + 1], inverse, modulus);
            for (std::size_t degree = 0; degree < count; ++degree)
                result[label][degree] = add_mod(result[label][degree],
                    multiply_mod(weight, quotient[degree], modulus), modulus);
        }
    }
    return result;
}

ExactSenderPolynomials build_exact_sender_coefficients(
    const Dataset& dataset, const Profile& profile, std::uint64_t plain_modulus,
    std::uint32_t bits_per_felt, std::size_t bundle_count, std::size_t items_per_bundle)
{
    require(profile.felts_per_item >= 5,
            "exact APSI matching needs at least five batch lanes per table item");
    // A separate partition is needed when first-felt roots collide or a bin
    // reaches the selected APSI degree. This also supports the reference
    // database's multiple BinBundles at the 1M-item target size.
    std::vector<std::vector<std::vector<FullTokenFelts>>> at_location(profile.table_size);
    for (const auto item : dataset.sender) {
        const auto felts = full_token_felts(test_oprf(item), bits_per_felt);
        for (const auto location : all_locations(test_oprf(item), profile)) {
            auto& partitions = at_location[location];
            auto fit = std::find_if(partitions.begin(), partitions.end(), [&](const auto& part) {
                return part.size() < profile.max_items_per_bin &&
                    std::none_of(part.begin(), part.end(), [&](const auto& other) {
                        return other[0] == felts[0];
                    });
            });
            if (fit == partitions.end()) {
                partitions.emplace_back();
                fit = std::prev(partitions.end());
            }
            fit->push_back(felts);
        }
    }
    std::size_t partition_count = 0;
    for (const auto& partitions : at_location)
        partition_count = std::max(partition_count, partitions.size());
    require(partition_count > 0, "exact APSI sender database is empty");

    ExactSenderPolynomials result;
    result.coefficients.resize(partition_count);
    result.occupied.resize(partition_count, std::vector<bool>(bundle_count, false));
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        result.coefficients[partition].resize(bundle_count);
        for (std::size_t bundle = 0; bundle < bundle_count; ++bundle) {
            std::array<std::vector<std::vector<std::uint64_t>>, 2> per_slot;
            for (auto& group : per_slot) group.resize(profile.degree);
            std::size_t bundle_degree = 0;
            for (std::size_t local = 0; local < items_per_bundle; ++local) {
                const auto location = bundle * items_per_bundle + local;
                const auto base = local * profile.felts_per_item;
                const auto& partitions = at_location[location];
                const std::vector<FullTokenFelts> empty;
                const auto& entries = partition < partitions.size()
                    ? partitions[partition] : empty;
                if (!entries.empty()) result.occupied[partition][bundle] = true;
                std::vector<std::uint64_t> roots;
                roots.reserve(entries.size());
                for (const auto& entry : entries) roots.push_back(entry[0]);
                auto matching = root_polynomial(roots, plain_modulus);
                bundle_degree = std::max(bundle_degree, roots.size());
                per_slot[0][base] = matching;
                // An otherwise constant label-only group still has a valid
                // nonconstant dummy slot for the standard HPU evaluator.
                per_slot[1][base + profile.felts_per_item - 1] = std::move(matching);
                if (entries.empty()) continue;
                auto labels = interpolate_token_felts(entries,
                    per_slot[0][base], plain_modulus);
                for (std::size_t field = 1; field <= 4; ++field)
                    per_slot[0][base + field] = std::move(labels[field - 1]);
                for (std::size_t field = 5; field <= 7; ++field)
                    per_slot[1][base + field - 5] = std::move(labels[field - 1]);
            }
            result.maximum_degree = std::max(result.maximum_degree, bundle_degree);
            if (!result.occupied[partition][bundle]) continue;
            for (std::size_t group = 0; group < 2; ++group) {
                auto& matrix = result.coefficients[partition][bundle][group];
                matrix.assign(bundle_degree + 1,
                              std::vector<std::uint64_t>(profile.degree));
                for (std::size_t slot = 0; slot < profile.degree; ++slot)
                    for (std::size_t coefficient = 0;
                         coefficient < per_slot[group][slot].size(); ++coefficient)
                        matrix[coefficient][slot] = per_slot[group][slot][coefficient];
            }
        }
    }
    return result;
}

class GraphRecorder {
public:
    GraphRecorder(hpu::seal_adapter::Evaluator evaluator, const ::seal::RelinKeys& keys,
                  OpCounts& counts)
        : evaluator_(std::move(evaluator)), keys_(keys), counts_(counts)
    {}

    hpu::seal_adapter::Ciphertext multiply(const hpu::seal_adapter::Ciphertext& left,
                                           const hpu::seal_adapter::Ciphertext& right)
    {
        hpu::seal_adapter::Ciphertext result;
        evaluator_.multiply(left, right, result);
        ++counts_.ciphertext_multiply;
        return result;
    }

    hpu::seal_adapter::Ciphertext square(const hpu::seal_adapter::Ciphertext& input)
    {
        hpu::seal_adapter::Ciphertext result;
        evaluator_.square(input, result);
        ++counts_.square;
        return result;
    }

    void relinearize(hpu::seal_adapter::Ciphertext& value)
    {
        evaluator_.relinearize_inplace(value, keys_);
        ++counts_.relinearize;
    }

    hpu::seal_adapter::Ciphertext multiply_plain(
        const hpu::seal_adapter::Ciphertext& input,
        const hpu::seal_adapter::Plaintext& plain)
    {
        hpu::seal_adapter::Ciphertext result;
        evaluator_.multiply_plain(input, plain, result);
        ++counts_.multiply_plain;
        return result;
    }

    void add(hpu::seal_adapter::Ciphertext& left,
             const hpu::seal_adapter::Ciphertext& right)
    {
        evaluator_.add_inplace(left, right);
        ++counts_.add;
    }

    void sub(hpu::seal_adapter::Ciphertext& left,
             const hpu::seal_adapter::Ciphertext& right)
    {
        evaluator_.sub_inplace(left, right);
        ++counts_.sub;
    }

    void add_plain(hpu::seal_adapter::Ciphertext& value,
                   const hpu::seal_adapter::Plaintext& plain)
    {
        evaluator_.add_plain_inplace(value, plain);
        ++counts_.add_plain;
    }

    void mod_switch(hpu::seal_adapter::Ciphertext& value)
    {
        evaluator_.mod_switch_to_next_inplace(value);
        ++counts_.mod_switch;
    }

private:
    hpu::seal_adapter::Evaluator evaluator_;
    const ::seal::RelinKeys& keys_;
    OpCounts& counts_;
};

hpu::seal_adapter::Plaintext import_slots(hpu::seal_adapter::ApplicationBuilder& app,
    ::seal::BatchEncoder& encoder, const std::string& id,
    const std::vector<std::uint64_t>& slots)
{
    ::seal::Plaintext encoded;
    encoder.encode(slots, encoded);
    return app.import_plaintext(id, encoded);
}

hpu::seal_adapter::Ciphertext sum_terms(GraphRecorder& graph,
    std::vector<hpu::seal_adapter::Ciphertext> terms)
{
    require(!terms.empty(), "APSI polynomial evaluation produced no terms");
    auto result = terms.front();
    for (std::size_t i = 1; i < terms.size(); ++i) graph.add(result, terms[i]);
    return result;
}

void mod_switch_to_chain(GraphRecorder& graph, hpu::seal_adapter::Ciphertext& value,
                         const ::seal::SEALContext& context,
                         std::size_t target_chain_index)
{
    auto level = context.get_context_data(value.parms_id());
    require(static_cast<bool>(level), "APSI ciphertext has an unknown modulus level");
    require(level->chain_index() >= target_chain_index,
            "APSI ciphertext is already below its requested modulus level");
    while (level->chain_index() > target_chain_index) {
        graph.mod_switch(value);
        level = context.get_context_data(value.parms_id());
        require(static_cast<bool>(level), "APSI ModSwitch produced an unknown modulus level");
    }
}

hpu::seal_adapter::Ciphertext evaluate_standard(
    hpu::seal_adapter::ApplicationBuilder& app, GraphRecorder& graph,
    ::seal::BatchEncoder& encoder,
    const std::map<std::uint32_t, hpu::seal_adapter::Ciphertext>& powers,
    const std::vector<std::vector<std::uint64_t>>& coefficients,
    std::size_t bundle)
{
    std::vector<hpu::seal_adapter::Ciphertext> terms;
    for (std::size_t degree = 1; degree < coefficients.size(); ++degree) {
        const auto plain = import_slots(app, encoder,
            "sender/bundle_" + std::to_string(bundle) + "/coefficient_" +
                std::to_string(degree), coefficients[degree]);
        terms.push_back(graph.multiply_plain(powers.at(static_cast<std::uint32_t>(degree)), plain));
    }
    require(!terms.empty(), "standard APSI evaluation requires a non-constant polynomial");
    auto result = sum_terms(graph, std::move(terms));
    const auto constant = import_slots(app, encoder,
        "sender/bundle_" + std::to_string(bundle) + "/coefficient_0", coefficients[0]);
    graph.add_plain(result, constant);
    graph.mod_switch(result);
    return result;
}

hpu::seal_adapter::Ciphertext evaluate_patstock(
    hpu::seal_adapter::ApplicationBuilder& app, GraphRecorder& graph,
    ::seal::BatchEncoder& encoder,
    const std::map<std::uint32_t, hpu::seal_adapter::Ciphertext>& powers,
    const std::vector<std::vector<std::uint64_t>>& coefficients,
    std::uint32_t low_degree, std::size_t bundle)
{
    const std::size_t degree = coefficients.size() - 1;
    require(low_degree > 1 && low_degree < degree,
            "invalid Paterson-Stockmeyer split");
    const std::size_t stride = low_degree + 1;
    const std::size_t high_count = degree / stride;
    auto coefficient = [&](std::size_t index) {
        return import_slots(app, encoder,
            "sender/bundle_" + std::to_string(bundle) + "/coefficient_" +
                std::to_string(index), coefficients[index]);
    };

    std::vector<hpu::seal_adapter::Ciphertext> high_terms;
    for (std::size_t block = 1; block <= high_count; ++block) {
        const auto last = block == high_count && degree % stride
            ? degree % stride
            : (block == high_count ? std::size_t{0} : static_cast<std::size_t>(low_degree));
        if (!last) continue;
        std::vector<hpu::seal_adapter::Ciphertext> inner_terms;
        for (std::size_t j = 1; j <= last; ++j)
            inner_terms.push_back(graph.multiply_plain(
                powers.at(static_cast<std::uint32_t>(j)), coefficient(block * stride + j)));
        auto inner = sum_terms(graph, std::move(inner_terms));
        graph.mod_switch(inner);
        auto term = graph.multiply(
            inner, powers.at(static_cast<std::uint32_t>(block * stride)));
        // The pre-PSI HPU API only adds two-component BFV ciphertexts. APSI's
        // mathematically equivalent immediate-relinearization form keeps every
        // product two-component before the additive reduction.
        graph.relinearize(term);
        high_terms.push_back(term);
    }

    hpu::seal_adapter::Ciphertext result;
    bool initialized = false;
    if (!high_terms.empty()) {
        result = sum_terms(graph, std::move(high_terms));
        initialized = true;
    }

    std::vector<hpu::seal_adapter::Ciphertext> low_terms;
    for (std::size_t j = 1; j < stride && j <= degree; ++j) {
        auto term = graph.multiply_plain(powers.at(static_cast<std::uint32_t>(j)), coefficient(j));
        graph.mod_switch(term);
        low_terms.push_back(term);
    }
    if (!low_terms.empty()) {
        auto low = sum_terms(graph, std::move(low_terms));
        if (initialized) graph.add(result, low);
        else { result = low; initialized = true; }
    }

    for (std::size_t block = 1; block <= high_count; ++block) {
        auto term = graph.multiply_plain(
            powers.at(static_cast<std::uint32_t>(block * stride)),
            coefficient(block * stride));
        if (initialized) graph.add(result, term);
        else { result = term; initialized = true; }
    }
    require(initialized, "Paterson-Stockmeyer evaluation produced no ciphertext");
    graph.add_plain(result, coefficient(0));
    graph.mod_switch(result);
    return result;
}

std::string json_array(const std::vector<std::uint64_t>& values)
{
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << values[i];
    }
    out << ']';
    return out.str();
}

void write_delivery_files(const Options& options, const Dataset& dataset,
                          const std::vector<std::uint64_t>& actual,
                          const std::vector<std::uint64_t>& reference,
                          const std::vector<PackageSummary>& packages,
                          const OpCounts& counts, std::size_t bundle_count,
                          std::size_t partition_count,
                          std::size_t max_degree, int minimum_noise,
                          double compile_seconds, std::uint64_t plain_modulus,
                          std::size_t query_ciphertexts)
{
    if (!options.evaluator.delivery.emit_directory) return;
    const auto& root = *options.evaluator.delivery.emit_directory;
    std::filesystem::create_directories(root);
    std::size_t total_nodes = 0, total_instructions = 0, total_dma = 0;
    std::uint64_t total_lines = 0;
    for (const auto& package : packages) {
        total_nodes += package.nodes;
        total_instructions += package.instructions;
        total_dma += package.dma_spans;
        total_lines += package.memory_lines;
    }
    std::ofstream test_data(root / "set_test_data.json");
    test_data << "{\n  \"sender\": " << json_array(dataset.sender)
              << ",\n  \"receiver\": " << json_array(dataset.receiver)
              << ",\n  \"expected_intersection\": "
              << json_array(dataset.expected_intersection)
              << ",\n  \"apsi_reference_matches\": " << json_array(reference)
              << ",\n  \"recovered_intersection\": " << json_array(actual) << "\n}\n";
    std::ofstream manifest(root / "pipeline_manifest.json");
    manifest << "{\n"
             << "  \"application\": \"APSI concentrated PSI HPU workload\",\n"
             << "  \"match_mode\": \"" << options.match_mode << "\",\n"
             << "  \"implementation_scope\": \"cropped protocol with deterministic OPRF-output fixture; mode-specific matching described in match_strategy\",\n"
             << "  \"profile\": \"" << options.profile.name << "\",\n"
             << "  \"scenario\": \"" << options.scenario << "\",\n"
             << "  \"parameter_origin\": \"" << options.profile.parameter_origin << "\",\n"
             << "  \"oprf_mode\": \"deterministic CPU test fixture; FourQ network OPRF not embedded\",\n"
             << "  \"bfv_context_source\": \"application-owned SEAL EncryptionParameters/SEALContext; no HPU context API extension\",\n"
             << "  \"patstock_relinearization\": \"immediate after each ciphertext product, before two-component Add reduction\",\n"
             << "  \"sender_size\": " << dataset.sender.size() << ",\n"
             << "  \"receiver_size\": " << dataset.receiver.size() << ",\n"
             << "  \"poly_modulus_degree\": " << options.profile.degree << ",\n"
             << "  \"plain_modulus\": " << plain_modulus << ",\n"
             << "  \"coefficient_modulus_bits\": [31,31,31,31,31],\n"
             << "  \"felts_per_item\": " << options.profile.felts_per_item << ",\n"
             << "  \"full_token_felts\": "
             << (options.match_mode == "trident" ? options.profile.felts_per_item : 8)
             << ",\n"
             << "  \"match_strategy\": \""
             << (options.match_mode == "trident"
                 ? "Trident APSI independent per-felt root polynomials and same-BinBundle all-zero recovery"
                 : "first-felt matching polynomial plus seven same-sender interpolated full-token felts")
             << "\",\n"
             << "  \"hash_function_count\": " << options.profile.hash_function_count << ",\n"
             << "  \"table_size\": " << options.profile.table_size << ",\n"
             << "  \"bundle_count\": " << bundle_count << ",\n"
             << "  \"sender_partition_count\": " << partition_count << ",\n"
             << "  \"max_items_per_bin\": " << options.profile.max_items_per_bin << ",\n"
             << "  \"observed_polynomial_degree\": " << max_degree << ",\n"
             << "  \"ps_low_degree\": " << options.profile.ps_low_degree << ",\n"
             << "  \"client_query_ciphertexts\": " << query_ciphertexts << ",\n"
             << "  \"hpu_application_packages\": " << packages.size() << ",\n"
             << "  \"graph_nodes\": " << total_nodes << ",\n"
             << "  \"encoded_instructions\": " << total_instructions << ",\n"
             << "  \"dma_spans\": " << total_dma << ",\n"
             << "  \"hpu_mem_lines\": " << total_lines << ",\n"
             << "  \"compile_and_software_verify_seconds\": " << std::fixed
             << std::setprecision(3) << compile_seconds << ",\n"
             << "  \"minimum_output_noise_budget_bits\": " << minimum_noise << ",\n"
             << "  \"resource_optimization\": "
             << (options.evaluator.compiler.optimize_resources ? "true" : "false") << ",\n"
             << "  \"operator_counts\": {\"ciphertext_multiply\":"
             << counts.ciphertext_multiply << ",\"square\":" << counts.square
             << ",\"relinearize\":" << counts.relinearize
             << ",\"multiply_plain\":" << counts.multiply_plain
             << ",\"add\":" << counts.add << ",\"sub\":" << counts.sub
             << ",\"add_plain\":"
             << counts.add_plain << ",\"mod_switch\":" << counts.mod_switch << "},\n"
             << "  \"cpu_steps\": [\"set preparation\",\"deterministic OPRF-output fixture\",\"cuckoo layout\",\"sender database polynomial preprocessing\",\"query source-power encoding/encryption\",\"reply decryption/intersection recovery\"],\n"
             << "  \"hpu_steps\": [\"PowersDag missing-power generation\",\"ciphertext multiplication\",\"relinearization\",\"power modulus switching\",\"mode-specific matching polynomial evaluation\",\"Paterson-Stockmeyer when selected\",\"final modulus switching\"],\n"
             << "  \"execution_pipeline\": [{\"step\":\"cpu_prepare_query_and_database\"}";
    for (const auto& package : packages)
        manifest << ", {\"step\":\"hpu_concentrated_query\",\"package\":\""
                 << package.directory << "\"}";
    manifest << ", {\"step\":\"cpu_decrypt_and_recover\",\"depends_on\":\"SEAL oracle ciphertext accepted only after coefficient-exact HPU software and instruction-model verification\"}],\n"
             << "  \"expected_intersection\": " << json_array(dataset.expected_intersection) << ",\n"
             << "  \"apsi_reference_matches\": " << json_array(reference) << ",\n"
             << "  \"recovered_intersection\": " << json_array(actual) << ",\n"
             << "  \"intersection_match\": "
             << (actual == reference ? "true" : "false") << ",\n"
             << "  \"apsi_reference_match\": "
             << (actual == reference ? "true" : "false") << ",\n"
             << "  \"raw_intersection_match\": "
             << (actual == dataset.expected_intersection ? "true" : "false") << ",\n"
             << "  \"seal_oracle_per_node_verified\": true,\n"
             << "  \"hpu_software_executor_verified\": true,\n"
             << "  \"host_instruction_model_verified\": true,\n"
             << "  \"real_hardware_verified\": false,\n"
             << "  \"packages\": [";
    for (std::size_t index = 0; index < packages.size(); ++index) {
        if (index) manifest << ',';
        const auto& package = packages[index];
        manifest << "{\"name\":\"apsi_query_" << index << "\",\"directory\":\""
                 << package.directory << "\",\"graph_nodes\":" << package.nodes
                 << ",\"encoded_instructions\":" << package.instructions
                 << ",\"dma_spans\":" << package.dma_spans
                 << ",\"hpu_mem_lines\":" << package.memory_lines << '}';
    }
    manifest << "]\n}\n";
    std::ofstream readme(root / "README.txt");
    readme << "Generate:\n  hpu_bfv_apsi_concentrated_example --profile "
           << options.profile.name << " --scenario " << options.scenario
           << " --match-mode " << options.match_mode
            << " --emit-dir <dir>\n\nValidate every package directory in pipeline_manifest.json.\n\n"
            << "Each package contains the HPU graph for one or more sender partitions.\n"
           << "Its CPU OPRF step is a deterministic correctness fixture, not APSI's FourQ OPRF.\n"
           << "The trident mode uses independent felt roots; exact adds same-token interpolation.\n"
           << "Software and host instruction-model checks do not constitute RTL/HPU hardware ST.\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parse_options(argc, argv);
        if (options.evaluator.delivery.poly_modulus_degree &&
            *options.evaluator.delivery.poly_modulus_degree != options.profile.degree)
            throw std::invalid_argument("--degree must match the selected APSI profile");

        // Build the exact APSI/Trident plaintext fields with SEAL's existing
        // public parameter API, then pass the resulting context directly to
        // the pre-existing ApplicationBuilder constructor. No HPU context API
        // or compiler path is extended for this application.
        ::seal::EncryptionParameters parameters(::seal::scheme_type::bfv);
        parameters.set_poly_modulus_degree(options.profile.degree);
        // Four data Q primes plus one special P preserve APSI's low-power
        // chain-index 2 / high-power chain-index 1 split while keeping every
        // HPU-visible modulus within the existing 31-bit ABI.
        parameters.set_coeff_modulus(::seal::CoeffModulus::Create(
            options.profile.degree, {31,31,31,31,31}));
        parameters.set_plain_modulus(options.profile.plain_modulus);
        ::seal::prng_seed_type seed{};
        const std::array<std::uint64_t, 8> seed_words{
            kSealSeed, 1, 2, 3, 4, 5, 6, 7};
        std::copy(seed_words.begin(), seed_words.end(), seed.begin());
        parameters.set_random_generator(
            std::make_shared<::seal::Blake2xbPRNGFactory>(seed));
        const auto context_pointer = std::make_shared<::seal::SEALContext>(
            std::move(parameters), true, ::seal::sec_level_type::none);
        require(context_pointer->parameters_set(),
                std::string("SEAL rejected the APSI-compatible BFV parameters: ") +
                    context_pointer->parameter_error_message());
        const auto context_data = context_pointer->first_context_data();
        require(context_data && context_data->qualifiers().using_batching,
                "APSI plaintext field does not support SEAL batching");
        const auto& context = *context_pointer;
        const auto plain_modulus = context_data->parms().plain_modulus().value();
        require((plain_modulus - 1) % (2 * options.profile.degree) == 0,
                "HPU-adapted APSI plaintext modulus does not support batching");
        const auto bits_per_felt = static_cast<std::uint32_t>(
            ::seal::util::get_significant_bit_count(plain_modulus) - 1);
        require(bits_per_felt * 7 < 128 && bits_per_felt * 8 >= 128,
                "selected APSI field cannot encode a full token in eight felts");
        const auto items_per_bundle = options.profile.degree / options.profile.felts_per_item;
        require(items_per_bundle && options.profile.table_size % items_per_bundle == 0,
                "APSI table_size is not a multiple of items_per_bundle");
        const auto bundle_count = options.profile.table_size / items_per_bundle;

        auto dataset = build_dataset(options);
        const bool trident_mode = options.match_mode == "trident";
        std::sort(dataset.sender.begin(), dataset.sender.end());
        std::sort(dataset.receiver.begin(), dataset.receiver.end());
        std::sort(dataset.expected_intersection.begin(), dataset.expected_intersection.end());
        const auto layout = trident_mode
            ? build_trident_cuckoo_layout(dataset.receiver, options.profile)
            : build_cuckoo_layout(dataset.receiver, options.profile);

        if (options.profile.name == "target")
            std::cerr << "PSI_PROGRESS sender_preprocessing_begin items="
                      << dataset.sender.size() << '\n';
        ExactSenderPolynomials exact_sender;
        TridentSenderPolynomials trident_sender;
        if (trident_mode)
            trident_sender = build_trident_sender_coefficients(
                dataset, options.profile, plain_modulus, bits_per_felt,
                bundle_count, items_per_bundle);
        else
            exact_sender = build_exact_sender_coefficients(
                dataset, options.profile, plain_modulus, bits_per_felt,
                bundle_count, items_per_bundle);
        const auto partition_count = trident_mode
            ? trident_sender.coefficients.size() : exact_sender.coefficients.size();
        const auto observed_degree = trident_mode
            ? trident_sender.maximum_degree : exact_sender.maximum_degree;
        const auto reference_intersection = trident_mode
            ? trident_cpu_reference(layout, trident_sender, options.profile,
                                    bits_per_felt, items_per_bundle)
            : dataset.expected_intersection;
        if (options.profile.name == "target")
            std::cerr << "PSI_PROGRESS sender_preprocessing_done partitions="
                      << partition_count << '\n';
        require(observed_degree > 0 && observed_degree <= options.profile.max_items_per_bin,
                "sender preprocessing produced an invalid polynomial degree");

        const auto targets = create_powers_set(
            options.profile.ps_low_degree,
            static_cast<std::uint32_t>(options.profile.max_items_per_bin));
        const auto dag = configure_powers_dag(options.profile.query_powers, targets);
        if (g_trace_vectors && trident_mode) {
            auto print_token = [&](char side, std::uint64_t item,
                                   std::size_t table_location) {
                const auto token = test_oprf(item);
                const auto felts = algebraize(token, options.profile.felts_per_item,
                                              bits_per_felt);
                std::cerr << "VECTOR_TOKEN " << side << ' ' << item << ' '
                          << token.low << ' ' << token.high << " felts";
                for (const auto felt : felts) std::cerr << ' ' << felt;
                std::cerr << " locations";
                for (const auto location : trident_locations(token, options.profile))
                    std::cerr << ' ' << location;
                std::cerr << " placed " << table_location << '\n';
            };
            for (std::size_t i = 0; i < std::min<std::size_t>(3, dataset.sender.size()); ++i)
                print_token('S', dataset.sender[i], options.profile.table_size);
            for (std::size_t i = 0; i < std::min<std::size_t>(3, dataset.receiver.size()); ++i)
                print_token('R', dataset.receiver[i], layout.item_to_table[i]);
            if (dataset.sender.size() >= 2) {
                const auto a = algebraize(test_oprf(dataset.sender[0]),
                    options.profile.felts_per_item, bits_per_felt);
                const auto b = algebraize(test_oprf(dataset.sender[1]),
                    options.profile.felts_per_item, bits_per_felt);
                for (std::size_t field = 0; field < options.profile.felts_per_item; ++field) {
                    const auto coefficients = root_polynomial({a[field], b[field]},
                                                               plain_modulus);
                    std::cerr << "VECTOR_SYNTH_POLY " << field << " roots "
                              << a[field] << ' ' << b[field] << " coefficients";
                    for (const auto coefficient : coefficients)
                        std::cerr << ' ' << coefficient;
                    std::cerr << '\n';
                }
            }
            for (const auto& node : dag)
                std::cerr << "VECTOR_POWER " << node.power << ' ' << node.depth
                          << ' ' << node.parents.first << ' ' << node.parents.second << '\n';
            std::size_t printed = 0;
            for (std::size_t p = 0; p < trident_sender.roots.size() && printed < 8; ++p)
                for (std::size_t b = 0; b < bundle_count && printed < 8; ++b) {
                    if (!trident_sender.occupied[p][b]) continue;
                    for (std::size_t slot = 0; slot < options.profile.degree && printed < 8;
                         ++slot) {
                        const auto& roots = trident_sender.roots[p][b][slot];
                        if (roots.empty()) continue;
                        std::cerr << "VECTOR_POLY " << p << ' ' << b << ' ' << slot
                                  << " roots";
                        for (const auto root : roots) std::cerr << ' ' << root;
                        std::cerr << " coefficients";
                        for (const auto& row : trident_sender.coefficients[p][b])
                            std::cerr << ' ' << row[slot];
                        std::cerr << '\n';
                        ++printed;
                    }
                }
        }

        ::seal::KeyGenerator keygen(context);
        ::seal::RelinKeys relin_keys;
        keygen.create_relin_keys(relin_keys);
        ::seal::BatchEncoder encoder(context);
        ::seal::Encryptor encryptor(context, keygen.secret_key());
        ::seal::Decryptor decryptor(context, keygen.secret_key());

        std::vector<std::vector<std::uint64_t>> query_slots(
            bundle_count, std::vector<std::uint64_t>(options.profile.degree));
        std::vector<std::array<std::vector<std::uint64_t>, 2>> expected_slots(bundle_count);
        for (auto& bundle : expected_slots)
            for (auto& group : bundle) group.resize(options.profile.degree);
        for (std::size_t location = 0; location < layout.table.size(); ++location) {
            if (!layout.table[location].occupied) continue;
            const auto felts = trident_mode
                ? algebraize(layout.table[location].token,
                             options.profile.felts_per_item, bits_per_felt)
                : algebraize(layout.table[location].token, 8, bits_per_felt);
            const auto bundle = location / items_per_bundle;
            const auto local = location % items_per_bundle;
            const auto base = local * options.profile.felts_per_item;
            for (std::size_t lane = 0; lane < options.profile.felts_per_item; ++lane)
                query_slots[bundle][base + lane] = trident_mode ? felts[lane] : felts[0];
            if (!trident_mode) {
                for (std::size_t field = 1; field <= 4; ++field)
                    expected_slots[bundle][0][base + field] = felts[field];
                for (std::size_t field = 5; field <= 7; ++field)
                    expected_slots[bundle][1][base + field - 5] = felts[field];
            }
        }

        std::map<std::pair<std::size_t, std::uint32_t>, ::seal::Ciphertext> encrypted_sources;
        std::vector<std::array<::seal::Ciphertext, 2>> encrypted_expected(bundle_count);
        for (std::size_t bundle = 0; bundle < bundle_count; ++bundle) {
            for (const auto source : options.profile.query_powers) {
                std::vector<std::uint64_t> powered(options.profile.degree);
                std::transform(query_slots[bundle].begin(), query_slots[bundle].end(),
                    powered.begin(), [&](std::uint64_t value) {
                        return power_mod(value, source, plain_modulus);
                    });
                ::seal::Plaintext plain;
                encoder.encode(powered, plain);
                encryptor.encrypt_symmetric(plain, encrypted_sources[{bundle, source}]);
            }
            for (std::size_t group = 0; group < (trident_mode ? 0U : 2U); ++group) {
                ::seal::Plaintext plain;
                encoder.encode(expected_slots[bundle][group], plain);
                encryptor.encrypt_symmetric(plain, encrypted_expected[bundle][group]);
            }
        }
        if (options.profile.name == "target")
            std::cerr << "PSI_PROGRESS query_encryption_done\n";

        // At 1M sender items, the reference database has multiple BinBundles.
        // Compile and verify one sender partition at a time so the HPU graph,
        // oracle state and resource allocator remain bounded. Smaller profiles
        // retain one package across all partitions for their existing tests.
        const auto per_package = options.profile.name == "target"
            ? std::size_t{1} : partition_count;
        std::vector<PackageSummary> package_summaries;
        OpCounts total_counts;
        double compile_seconds = 0;
        int minimum_noise = std::numeric_limits<int>::max();
        std::vector<std::uint64_t> actual_intersection;
        for (std::size_t first = 0; first < partition_count; first += per_package) {
            const auto last = std::min(first + per_package, partition_count);
            if (options.profile.name == "target")
                std::cerr << "PSI_PROGRESS graph partition=" << first << '/'
                          << partition_count << '\n';
            hpu::seal_adapter::ApplicationBuilder app(context);
            OpCounts counts;
            GraphRecorder graph(app.evaluator(), relin_keys, counts);
            std::vector<std::vector<std::array<hpu::seal_adapter::Ciphertext, 2>>> outputs(
                last - first, std::vector<std::array<hpu::seal_adapter::Ciphertext, 2>>(
                                  bundle_count));
            for (std::size_t bundle = 0; bundle < bundle_count; ++bundle) {
                std::map<std::uint32_t, hpu::seal_adapter::Ciphertext> powers;
                for (const auto source : options.profile.query_powers)
                    powers[source] = app.import_ciphertext(
                        "query/bundle_" + std::to_string(bundle) + "/power_" +
                            std::to_string(source), encrypted_sources.at({bundle, source}));
                for (const auto& node : dag) {
                    if (node.source) continue;
                    auto value = node.parents.first == node.parents.second
                        ? graph.square(powers.at(node.parents.first))
                        : graph.multiply(powers.at(node.parents.first),
                                         powers.at(node.parents.second));
                    graph.relinearize(value);
                    powers[node.power] = value;
                }
                for (auto& entry : powers) {
                    const auto target_chain = options.profile.ps_low_degree &&
                                              entry.first <= options.profile.ps_low_degree
                        ? std::size_t{2} : std::size_t{1};
                    mod_switch_to_chain(graph, entry.second, context, target_chain);
                }
                std::array<hpu::seal_adapter::Ciphertext, 2> expected;
                for (std::size_t group = 0; group < (trident_mode ? 0U : 2U); ++group) {
                    expected[group] = app.import_ciphertext(
                        "query/bundle_" + std::to_string(bundle) + "/expected_" +
                            std::to_string(group), encrypted_expected[bundle][group]);
                    mod_switch_to_chain(graph, expected[group], context, 0);
                }
                for (std::size_t partition = first; partition < last; ++partition) {
                    const bool occupied = trident_mode
                        ? trident_sender.occupied[partition][bundle]
                        : exact_sender.occupied[partition][bundle];
                    if (!occupied) continue;
                    for (std::size_t group = 0; group < (trident_mode ? 1U : 2U); ++group) {
                        const auto& coefficients = trident_mode
                            ? trident_sender.coefficients[partition][bundle]
                            : exact_sender.coefficients[partition][bundle][group];
                        const bool use_ps = options.profile.ps_low_degree > 1 &&
                            options.profile.ps_low_degree < coefficients.size() - 1;
                        const auto name = partition * bundle_count * 2 + bundle * 2 + group;
                        auto result = use_ps
                            ? evaluate_patstock(app, graph, encoder, powers, coefficients,
                                                options.profile.ps_low_degree, name)
                            : evaluate_standard(app, graph, encoder, powers, coefficients, name);
                        mod_switch_to_chain(graph, result, context, 0);
                        if (!trident_mode) {
                            require(result.parms_id() == expected[group].parms_id(),
                                    "exact APSI label and query fields have different BFV levels");
                            graph.sub(result, expected[group]);
                        }
                        app.mark_output("psi_partition_" + std::to_string(partition) +
                            "_bundle_" + std::to_string(bundle) + "_group_" +
                            std::to_string(group), result);
                        outputs[partition - first][bundle][group] = result;
                    }
                }
            }
            require(counts.total() > 0, "APSI HPU graph is empty");
            if (options.profile.name == "target")
                std::cerr << "PSI_PROGRESS compile partition=" << first << '\n';
            const auto started = std::chrono::steady_clock::now();
            const auto name = options.profile.name == "target"
                ? "bfv_apsi_concentrated_application_partition_" + std::to_string(first)
                : std::string("bfv_apsi_concentrated_application");
            const auto compiled = app.compile(
                delivery_artifact_stem(name.c_str(), options.evaluator.delivery,
                                       options.profile.degree),
                options.evaluator.compiler);
            compile_seconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            require(compiled.node_count() == counts.total(),
                    "APSI operator accounting differs from the compiled graph");
            require(compiled.verified(),
                    "APSI HPU software execution did not match the SEAL oracle coefficients");
            require(compiled.instruction_verified(),
                    "APSI encoded instruction model verification failed");

            std::vector<std::vector<std::array<std::vector<std::uint64_t>, 2>>> decoded(
                last - first,
                std::vector<std::array<std::vector<std::uint64_t>, 2>>(bundle_count));
            for (std::size_t partition = first; partition < last; ++partition)
                for (std::size_t bundle = 0; bundle < bundle_count; ++bundle) {
                    const bool occupied = trident_mode
                        ? trident_sender.occupied[partition][bundle]
                        : exact_sender.occupied[partition][bundle];
                    if (!occupied) continue;
                    for (std::size_t group = 0; group < (trident_mode ? 1U : 2U); ++group) {
                        const auto& oracle =
                            compiled.oracle_result(outputs[partition - first][bundle][group]);
                        minimum_noise = std::min(minimum_noise,
                            decryptor.invariant_noise_budget(oracle));
                        ::seal::Plaintext oracle_plain;
                        decryptor.decrypt(oracle, oracle_plain);
                        encoder.decode(oracle_plain,
                            decoded[partition - first][bundle][group]);
                    }
                }
            require(minimum_noise > 0, "APSI reply exhausted the BFV noise budget");

            std::vector<std::uint64_t> package_matches;
            for (std::size_t location = 0; location < layout.table.size(); ++location) {
                const auto& entry = layout.table[location];
                if (!entry.occupied) continue;
                const auto bundle = location / items_per_bundle;
                const auto local = location % items_per_bundle;
                const auto base = local * options.profile.felts_per_item;
                for (std::size_t partition = first; partition < last; ++partition) {
                    const bool occupied = trident_mode
                        ? trident_sender.occupied[partition][bundle]
                        : exact_sender.occupied[partition][bundle];
                    if (!occupied) continue;
                    const auto& groups = decoded[partition - first][bundle];
                    bool match = true;
                    if (trident_mode) {
                        for (std::size_t field = 0; field < options.profile.felts_per_item;
                             ++field)
                            match = match && groups[0][base + field] == 0;
                    } else {
                        match = groups[0][base] == 0;
                        for (std::size_t field = 1; field <= 4; ++field)
                            match = match && groups[0][base + field] == 0;
                        for (std::size_t field = 5; field <= 7; ++field)
                            match = match && groups[1][base + field - 5] == 0;
                    }
                    if (match) { package_matches.push_back(entry.item); break; }
                }
            }
            std::sort(package_matches.begin(), package_matches.end());
            package_matches.erase(std::unique(package_matches.begin(),
                                              package_matches.end()), package_matches.end());
            actual_intersection.insert(actual_intersection.end(),
                                       package_matches.begin(), package_matches.end());

            const auto& package = compiled.package();
            const auto directory = per_package == partition_count
                ? std::string("hpu_query")
                : "hpu_query_partition_" + std::to_string(first);
            package_summaries.push_back({directory, compiled.node_count(),
                package.program.instructions.size(),
                package.program.ordered_dma_spans.size(),
                package.initial_image->used_lines()});
            accumulate(total_counts, counts);
            auto package_options = options.evaluator;
            if (package_options.delivery.emit_directory)
                package_options.delivery.emit_directory =
                    *package_options.delivery.emit_directory / directory;
            evaluator_example::publish(package_options, compiled,
                hpu::seal_adapter::integer_delivery_semantics(package_matches));
            if (options.profile.name == "target")
                std::cerr << "PSI_PROGRESS verified partition=" << first
                          << " matches=" << package_matches.size() << '\n';
        }
        std::sort(actual_intersection.begin(), actual_intersection.end());
        actual_intersection.erase(std::unique(actual_intersection.begin(),
                                             actual_intersection.end()), actual_intersection.end());
        std::size_t total_nodes = 0, total_instructions = 0, total_dma = 0;
        std::uint64_t total_memory_lines = 0;
        for (const auto& package : package_summaries) {
            total_nodes += package.nodes;
            total_instructions += package.instructions;
            total_dma += package.dma_spans;
            total_memory_lines += package.memory_lines;
        }
        if (g_test_input) {
            std::cerr << "AUDIT_TEST_ONLY input=token_override"
                      << " seal_oracle=PASS hpu_software=PASS instruction_model=PASS"
                      << " nodes=" << total_nodes
                      << " instructions=" << total_instructions
                      << " dma=" << total_dma
                      << " hpu_mem_lines=" << total_memory_lines
                      << " noise=" << minimum_noise
                      << " compile_seconds=" << compile_seconds << '\n';
            std::cerr << "AUDIT_EXPECTED";
            for (const auto item : dataset.expected_intersection) std::cerr << ' ' << item;
            std::cerr << "\nAUDIT_APSI_REFERENCE";
            for (const auto item : reference_intersection) std::cerr << ' ' << item;
            std::cerr << "\nAUDIT_RECOVERED";
            for (const auto item : actual_intersection) std::cerr << ' ' << item;
            std::cerr << '\n';
        }
        if (g_test_input && g_test_publish_mismatch &&
            actual_intersection != reference_intersection &&
            !options.evaluator.delivery.emit_directory)
            throw std::invalid_argument("--test-publish-mismatch needs --emit-dir");
        write_delivery_files(options, dataset, actual_intersection, reference_intersection,
            package_summaries, total_counts, bundle_count, partition_count,
            observed_degree, minimum_noise, compile_seconds, plain_modulus,
            bundle_count * (options.profile.query_powers.size() +
                            (trident_mode ? 0U : 2U)));
        if (g_test_input && g_test_publish_mismatch &&
            actual_intersection != reference_intersection)
            std::cerr << "AUDIT_SEMANTIC_MISMATCH_PACKAGE_PUBLISHED\n";
        require(actual_intersection == reference_intersection,
                "APSI client recovery differs from the CPU APSI reference");
        std::cout << "APSI-style concentrated PSI application: PASS\n"
                  << "Scope: cropped protocol with deterministic CPU OPRF-output fixture; not full network APSI\n"
                  << "Match mode: " << options.match_mode << '\n'
                  << "Profile/scenario: " << options.profile.name << '/' << options.scenario << '\n'
                  << "Raw set intersection/APSI reference matches: "
                  << dataset.expected_intersection.size() << '/'
                  << reference_intersection.size() << '\n'
                  << "Sender/receiver/intersection: " << dataset.sender.size() << '/'
                  << dataset.receiver.size() << '/' << actual_intersection.size() << '\n'
                  << "N/t/felts/hash/table/bundles: " << options.profile.degree << '/'
                  << plain_modulus << '/' << options.profile.felts_per_item << '/'
                  << options.profile.hash_function_count << '/' << options.profile.table_size
                  << '/' << bundle_count << " (sender partitions: " << partition_count << ")\n"
                  << "Max configured/observed polynomial degree: "
                  << options.profile.max_items_per_bin << '/' << observed_degree << '\n'
                  << "PowersDag source/target powers: " << options.profile.query_powers.size()
                  << '/' << targets.size() << '\n'
                   << "Packages/nodes/instructions/DMA/HPU_MEM lines: "
                   << package_summaries.size() << '/' << total_nodes << '/'
                   << total_instructions << '/' << total_dma << '/'
                   << total_memory_lines << '\n'
                  << "Minimum output noise budget: " << minimum_noise << " bits\n"
                  << "Compile plus software verification: " << std::fixed << std::setprecision(3)
                  << compile_seconds << " seconds\n"
                  << "SEAL per-node oracle comparison: PASS\n"
                  << "HPU software execution and final intersection: PASS\n"
                  << "Host encoded-instruction model: PASS\n"
                  << "Real HPU/RTL execution: NOT RUN\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Concentrated APSI application failed: " << error.what() << '\n';
        return 1;
    }
}
