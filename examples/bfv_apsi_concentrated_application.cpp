#include "evaluator_application.hpp"

#include "hpu/seal/bfv_context.hpp"

#include <seal/seal.h>
#include <seal/util/numth.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
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
    int plain_modulus_bits;
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
    std::size_t add_plain = 0;
    std::size_t mod_switch = 0;

    std::size_t total() const noexcept
    {
        return ciphertext_multiply + square + relinearize + multiply_plain + add +
               add_plain + mod_switch;
    }
};

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
    // This deliberately deterministic fixture represents the output boundary of
    // APSI's FourQ OPRF. It is not a replacement for the cryptographic OPRF.
    const auto low = splitmix64(item ^ kOprfFixtureKey);
    const auto high = splitmix64(low ^ (item << 1) ^ UINT64_C(0xd6e8feb86659fd93));
    return {low ? low : 1, high};
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

std::vector<std::uint64_t> algebraize(const Token& token, std::size_t felt_count,
                                      std::uint32_t bits_per_felt)
{
    require(felt_count * bits_per_felt <= 128,
            "APSI item algebraization exceeds the 128-bit OPRF token");
    std::vector<std::uint64_t> result(felt_count);
    for (std::size_t felt = 0; felt < felt_count; ++felt) {
        std::uint64_t value = 0;
        for (std::uint32_t bit = 0; bit < bits_per_felt; ++bit) {
            const auto source = felt * bits_per_felt + bit;
            const auto set = source < 64
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
        bool found = false;
        for (const auto left : targets) {
            if (left >= power) break;
            const auto right = power - left;
            if (!targets.count(right) || !nodes.count(left) || !nodes.count(right)) continue;
            const auto depth = std::max(nodes.at(left).depth, nodes.at(right).depth) + 1;
            if (!found || depth < optimal_depth) {
                found = true;
                optimal_depth = depth;
                optimal = {left, right};
            }
        }
        require(found, "APSI PowersDag target cannot be decomposed");
        nodes[power] = {power, optimal_depth, false, optimal};
    }
    std::vector<PowersNode> result;
    for (const auto& entry : nodes) result.push_back(entry.second);
    return result;
}

Profile small_profile()
{
    return {"small", 10, 5, 2, 8192, 17, 8, 3, 4096, 16, 0, {1,3,5},
            "Poseidon Trident create_params1 / APSI UnlabeledSmall1"};
}

Profile medium_profile()
{
    return {"medium", 8192, 512, 32, 4096, 19, 5, 3, 1638, 125, 5,
            {1,2,3,4,5,6,18,30,42,54,60},
            "Microsoft APSI 1M-1024-com parameter shape; sender/receiver counts reduced"};
}

Options parse_options(int argc, char** argv)
{
    Profile profile = small_profile();
    std::string scenario = "multi";
    bool optimize = false;
    std::vector<char*> remaining{argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--profile") {
            if (++i >= argc) throw std::invalid_argument("--profile requires small or medium");
            const std::string value(argv[i]);
            if (value == "small") profile = small_profile();
            else if (value == "medium") profile = medium_profile();
            else throw std::invalid_argument("--profile must be small or medium");
        } else if (argument == "--scenario") {
            if (++i >= argc) throw std::invalid_argument("--scenario requires a value");
            scenario = argv[i];
        } else if (argument == "--optimize-resources") {
            optimize = true;
        } else {
            remaining.push_back(argv[i]);
        }
    }
    if (profile.name == "medium" && scenario != "multi")
        throw std::invalid_argument("the medium profile supports the multi scenario only");
    if (profile.name == "small" && scenario != "empty" && scenario != "single" &&
        scenario != "multi" && scenario != "boundary")
        throw std::invalid_argument("small --scenario must be empty, single, multi, or boundary");
    auto evaluator = evaluator_example::options(
        static_cast<int>(remaining.size()), remaining.data());
    evaluator.compiler.optimize_resources = optimize;
    return {std::move(profile), std::move(scenario), std::move(evaluator)};
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
    if (profile.name == "medium") {
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

std::vector<std::vector<std::vector<std::uint64_t>>> build_sender_coefficients(
    const Dataset& dataset, const Profile& profile, std::uint64_t plain_modulus,
    std::uint32_t bits_per_felt, std::size_t bundle_count, std::size_t items_per_bundle,
    std::size_t& maximum_degree)
{
    std::vector<std::vector<std::vector<std::uint64_t>>> roots(
        bundle_count, std::vector<std::vector<std::uint64_t>>(profile.degree));
    for (const auto item : dataset.sender) {
        const auto token = test_oprf(item);
        const auto felts = algebraize(token, profile.felts_per_item, bits_per_felt);
        for (const auto location : all_locations(token, profile)) {
            const auto bundle = location / items_per_bundle;
            const auto local = location % items_per_bundle;
            for (std::size_t felt = 0; felt < felts.size(); ++felt)
                roots[bundle][local * profile.felts_per_item + felt].push_back(felts[felt]);
        }
    }
    maximum_degree = 0;
    std::vector<std::vector<std::vector<std::uint64_t>>> result(bundle_count);
    for (std::size_t bundle = 0; bundle < bundle_count; ++bundle) {
        std::vector<std::vector<std::uint64_t>> per_slot(profile.degree);
        std::size_t bundle_degree = 0;
        for (std::size_t slot = 0; slot < profile.degree; ++slot) {
            require(roots[bundle][slot].size() <= profile.max_items_per_bin,
                    "sender bin exceeds the selected APSI max_items_per_bin");
            per_slot[slot] = root_polynomial(roots[bundle][slot], plain_modulus);
            bundle_degree = std::max(bundle_degree, per_slot[slot].size() - 1);
        }
        maximum_degree = std::max(maximum_degree, bundle_degree);
        result[bundle].assign(bundle_degree + 1,
                              std::vector<std::uint64_t>(profile.degree));
        for (std::size_t slot = 0; slot < profile.degree; ++slot)
            for (std::size_t coefficient = 0; coefficient < per_slot[slot].size(); ++coefficient)
                result[bundle][coefficient][slot] = per_slot[slot][coefficient];
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

    std::vector<hpu::seal_adapter::Ciphertext> tensor_terms;
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
        tensor_terms.push_back(graph.multiply(
            inner, powers.at(static_cast<std::uint32_t>(block * stride))));
    }

    hpu::seal_adapter::Ciphertext result;
    bool initialized = false;
    if (!tensor_terms.empty()) {
        result = sum_terms(graph, std::move(tensor_terms));
        graph.relinearize(result);
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
                          const hpu::seal_adapter::CompiledApplication& compiled,
                          const OpCounts& counts, std::size_t bundle_count,
                          std::size_t max_degree, int minimum_noise,
                          double compile_seconds, std::uint64_t plain_modulus,
                          std::size_t query_ciphertexts)
{
    if (!options.evaluator.delivery.emit_directory) return;
    const auto& root = *options.evaluator.delivery.emit_directory;
    std::filesystem::create_directories(root);
    const auto& package = compiled.package();
    std::ofstream test_data(root / "set_test_data.json");
    test_data << "{\n  \"sender\": " << json_array(dataset.sender)
              << ",\n  \"receiver\": " << json_array(dataset.receiver)
              << ",\n  \"expected_intersection\": "
              << json_array(dataset.expected_intersection)
              << ",\n  \"recovered_intersection\": " << json_array(actual) << "\n}\n";
    std::ofstream manifest(root / "pipeline_manifest.json");
    manifest << "{\n"
             << "  \"application\": \"APSI-compatible concentrated unlabeled PSI HPU workload\",\n"
             << "  \"implementation_scope\": \"explicitly cropped APSI protocol: deterministic OPRF-output fixture, APSI-style cuckoo/batching/PowersDag/polynomial evaluation\",\n"
             << "  \"profile\": \"" << options.profile.name << "\",\n"
             << "  \"scenario\": \"" << options.scenario << "\",\n"
             << "  \"parameter_origin\": \"" << options.profile.parameter_origin << "\",\n"
             << "  \"oprf_mode\": \"deterministic CPU test fixture; FourQ network OPRF not embedded\",\n"
             << "  \"sender_size\": " << dataset.sender.size() << ",\n"
             << "  \"receiver_size\": " << dataset.receiver.size() << ",\n"
             << "  \"poly_modulus_degree\": " << options.profile.degree << ",\n"
             << "  \"plain_modulus\": " << plain_modulus << ",\n"
             << "  \"coefficient_modulus_bits\": [31,31,31,31,31],\n"
             << "  \"felts_per_item\": " << options.profile.felts_per_item << ",\n"
             << "  \"hash_function_count\": " << options.profile.hash_function_count << ",\n"
             << "  \"table_size\": " << options.profile.table_size << ",\n"
             << "  \"bundle_count\": " << bundle_count << ",\n"
             << "  \"max_items_per_bin\": " << options.profile.max_items_per_bin << ",\n"
             << "  \"observed_polynomial_degree\": " << max_degree << ",\n"
             << "  \"ps_low_degree\": " << options.profile.ps_low_degree << ",\n"
             << "  \"client_query_ciphertexts\": " << query_ciphertexts << ",\n"
             << "  \"hpu_application_packages\": 1,\n"
             << "  \"graph_nodes\": " << compiled.node_count() << ",\n"
             << "  \"encoded_instructions\": " << package.program.instructions.size() << ",\n"
             << "  \"dma_spans\": " << package.program.ordered_dma_spans.size() << ",\n"
             << "  \"hpu_mem_lines\": " << package.initial_image->used_lines() << ",\n"
             << "  \"compile_and_software_verify_seconds\": " << std::fixed
             << std::setprecision(3) << compile_seconds << ",\n"
             << "  \"minimum_output_noise_budget_bits\": " << minimum_noise << ",\n"
             << "  \"resource_optimization\": "
             << (options.evaluator.compiler.optimize_resources ? "true" : "false") << ",\n"
             << "  \"operator_counts\": {\"ciphertext_multiply\":"
             << counts.ciphertext_multiply << ",\"square\":" << counts.square
             << ",\"relinearize\":" << counts.relinearize
             << ",\"multiply_plain\":" << counts.multiply_plain
             << ",\"add\":" << counts.add << ",\"add_plain\":"
             << counts.add_plain << ",\"mod_switch\":" << counts.mod_switch << "},\n"
             << "  \"cpu_steps\": [\"set preparation\",\"deterministic OPRF-output fixture\",\"cuckoo layout\",\"sender database polynomial preprocessing\",\"query source-power encoding/encryption\",\"reply decryption/intersection recovery\"],\n"
             << "  \"hpu_steps\": [\"PowersDag missing-power generation\",\"ciphertext multiplication\",\"relinearization\",\"power modulus switching\",\"batched polynomial MultiplyPlain/Add\",\"Paterson-Stockmeyer when selected\",\"final modulus switching\"],\n"
             << "  \"execution_pipeline\": [{\"step\":\"cpu_prepare_query_and_database\"},{\"step\":\"hpu_concentrated_query\",\"package\":\"hpu_query\"},{\"step\":\"cpu_decrypt_and_recover\",\"depends_on\":\"HPU software-result memory, not SEAL oracle\"}],\n"
             << "  \"expected_intersection\": " << json_array(dataset.expected_intersection) << ",\n"
             << "  \"recovered_intersection\": " << json_array(actual) << ",\n"
             << "  \"intersection_match\": "
             << (actual == dataset.expected_intersection ? "true" : "false") << ",\n"
             << "  \"seal_oracle_per_node_verified\": true,\n"
             << "  \"hpu_software_executor_verified\": true,\n"
             << "  \"host_instruction_model_verified\": "
             << (compiled.instruction_verified() ? "true" : "false") << ",\n"
             << "  \"real_hardware_verified\": false,\n"
             << "  \"packages\": [{\"name\":\"apsi_query\",\"directory\":\"hpu_query\"}]\n"
             << "}\n";
    std::ofstream readme(root / "README.txt");
    readme << "Generate:\n  hpu_bfv_apsi_concentrated_example --profile "
           << options.profile.name << " --scenario " << options.scenario
           << " --emit-dir <dir>\n\nValidate:\n  hpu_validate_package <dir>/hpu_query\n\n"
           << "The package contains one concentrated HPU graph for all bundle indices.\n"
           << "Its CPU OPRF step is a deterministic correctness fixture, not APSI's FourQ OPRF.\n"
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

        hpu::seal_adapter::BfvContextSpec spec;
        spec.poly_modulus_degree = options.profile.degree;
        // Four data Q primes plus one special P. This preserves APSI's explicit
        // low-power chain-index 2 / high-power chain-index 1 split while keeping
        // every HPU-visible modulus within the 31-bit ABI.
        spec.coeff_modulus_bits = {31,31,31,31,31};
        spec.plain_modulus_bits = options.profile.plain_modulus_bits;
        spec.plain_modulus = options.profile.name == "small"
            ? std::optional<std::uint32_t>{65537}
            : std::optional<std::uint32_t>{270337};
        spec.random_seed = std::array<std::uint64_t, 8>{
            kSealSeed, 1, 2, 3, 4, 5, 6, 7};
        const auto context_bundle = hpu::seal_adapter::create_bfv_context(spec);
        const auto& context = *context_bundle.context;
        const auto plain_modulus = static_cast<std::uint64_t>(context_bundle.plain_modulus);
        require((plain_modulus - 1) % (2 * options.profile.degree) == 0,
                "HPU-adapted APSI plaintext modulus does not support batching");
        const auto bits_per_felt = static_cast<std::uint32_t>(
            ::seal::util::get_significant_bit_count(plain_modulus) - 1);
        require(bits_per_felt * options.profile.felts_per_item <= 128,
                "selected APSI item parameters exceed one OPRF token");
        const auto items_per_bundle = options.profile.degree / options.profile.felts_per_item;
        require(items_per_bundle && options.profile.table_size % items_per_bundle == 0,
                "APSI table_size is not a multiple of items_per_bundle");
        const auto bundle_count = options.profile.table_size / items_per_bundle;

        auto dataset = build_dataset(options);
        std::sort(dataset.sender.begin(), dataset.sender.end());
        std::sort(dataset.receiver.begin(), dataset.receiver.end());
        std::sort(dataset.expected_intersection.begin(), dataset.expected_intersection.end());
        const auto layout = build_cuckoo_layout(dataset.receiver, options.profile);

        std::size_t observed_degree = 0;
        const auto coefficients = build_sender_coefficients(
            dataset, options.profile, plain_modulus, bits_per_felt,
            bundle_count, items_per_bundle, observed_degree);
        require(observed_degree > 0 && observed_degree <= options.profile.max_items_per_bin,
                "sender preprocessing produced an invalid polynomial degree");

        const auto targets = create_powers_set(
            options.profile.ps_low_degree,
            static_cast<std::uint32_t>(options.profile.max_items_per_bin));
        const auto dag = configure_powers_dag(options.profile.query_powers, targets);

        ::seal::KeyGenerator keygen(context);
        ::seal::RelinKeys relin_keys;
        keygen.create_relin_keys(relin_keys);
        ::seal::BatchEncoder encoder(context);
        ::seal::Encryptor encryptor(context, keygen.secret_key());
        ::seal::Decryptor decryptor(context, keygen.secret_key());

        std::vector<std::vector<std::uint64_t>> query_slots(
            bundle_count, std::vector<std::uint64_t>(options.profile.degree));
        for (std::size_t location = 0; location < layout.table.size(); ++location) {
            if (!layout.table[location].occupied) continue;
            const auto felts = algebraize(layout.table[location].token,
                options.profile.felts_per_item, bits_per_felt);
            const auto bundle = location / items_per_bundle;
            const auto local = location % items_per_bundle;
            std::copy(felts.begin(), felts.end(),
                query_slots[bundle].begin() + static_cast<std::ptrdiff_t>(
                    local * options.profile.felts_per_item));
        }

        std::map<std::pair<std::size_t, std::uint32_t>, ::seal::Ciphertext> encrypted_sources;
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
        }

        hpu::seal_adapter::ApplicationBuilder app(context);
        OpCounts counts;
        GraphRecorder graph(app.evaluator(), relin_keys, counts);
        std::vector<hpu::seal_adapter::Ciphertext> outputs;
        outputs.reserve(bundle_count);
        for (std::size_t bundle = 0; bundle < bundle_count; ++bundle) {
            std::map<std::uint32_t, hpu::seal_adapter::Ciphertext> powers;
            for (const auto source : options.profile.query_powers) {
                powers[source] = app.import_ciphertext(
                    "query/bundle_" + std::to_string(bundle) + "/power_" +
                        std::to_string(source), encrypted_sources.at({bundle, source}));
            }
            for (const auto& node : dag) {
                if (node.source) continue;
                auto value = node.parents.first == node.parents.second
                    ? graph.square(powers.at(node.parents.first))
                    : graph.multiply(powers.at(node.parents.first), powers.at(node.parents.second));
                graph.relinearize(value);
                powers[node.power] = value;
            }
            for (auto& entry : powers) {
                const auto target_chain = options.profile.ps_low_degree &&
                                          entry.first <= options.profile.ps_low_degree
                    ? std::size_t{2}
                    : std::size_t{1};
                mod_switch_to_chain(graph, entry.second, context, target_chain);
            }
            const bool use_ps = options.profile.ps_low_degree > 1 &&
                                options.profile.ps_low_degree < coefficients[bundle].size() - 1;
            auto result = use_ps
                ? evaluate_patstock(app, graph, encoder, powers, coefficients[bundle],
                                    options.profile.ps_low_degree, bundle)
                : evaluate_standard(app, graph, encoder, powers, coefficients[bundle], bundle);
            app.mark_output("psi_result_bundle_" + std::to_string(bundle), result);
            outputs.push_back(result);
        }
        require(counts.total() > 0, "APSI HPU graph is empty");

        const auto started = std::chrono::steady_clock::now();
        const auto compiled = app.compile(
            delivery_artifact_stem("bfv_apsi_concentrated_application",
                                   options.evaluator.delivery, options.profile.degree),
            options.evaluator.compiler);
        const auto compile_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        require(compiled.node_count() == counts.total(),
                "APSI operator accounting differs from the compiled graph");

        std::vector<std::vector<std::uint64_t>> decoded_outputs(bundle_count);
        int minimum_noise = std::numeric_limits<int>::max();
        for (std::size_t bundle = 0; bundle < bundle_count; ++bundle) {
            const auto& software = compiled.software_result(outputs[bundle]);
            const auto& oracle = compiled.oracle_result(outputs[bundle]);
            minimum_noise = std::min(minimum_noise, decryptor.invariant_noise_budget(software));
            ::seal::Plaintext software_plain, oracle_plain;
            decryptor.decrypt(software, software_plain);
            decryptor.decrypt(oracle, oracle_plain);
            std::vector<std::uint64_t> oracle_slots;
            encoder.decode(software_plain, decoded_outputs[bundle]);
            encoder.decode(oracle_plain, oracle_slots);
            require(decoded_outputs[bundle] == oracle_slots,
                    "APSI HPU software output differs from the SEAL oracle slots");
        }
        require(minimum_noise > 0, "APSI reply exhausted the BFV noise budget");

        std::vector<std::uint64_t> actual_intersection;
        for (std::size_t location = 0; location < layout.table.size(); ++location) {
            const auto& entry = layout.table[location];
            if (!entry.occupied) continue;
            const auto bundle = location / items_per_bundle;
            const auto local = location % items_per_bundle;
            bool match = true;
            for (std::size_t felt = 0; felt < options.profile.felts_per_item; ++felt)
                match = match && decoded_outputs[bundle][local * options.profile.felts_per_item + felt] == 0;
            if (match) actual_intersection.push_back(entry.item);
        }
        std::sort(actual_intersection.begin(), actual_intersection.end());
        require(actual_intersection == dataset.expected_intersection,
                "APSI client recovery differs from the CPU set intersection");

        auto package_options = options.evaluator;
        if (package_options.delivery.emit_directory)
            package_options.delivery.emit_directory =
                *package_options.delivery.emit_directory / "hpu_query";
        evaluator_example::publish(package_options, compiled,
            hpu::seal_adapter::integer_delivery_semantics(actual_intersection));
        write_delivery_files(options, dataset, actual_intersection, compiled, counts,
            bundle_count, observed_degree, minimum_noise, compile_seconds,
            plain_modulus, bundle_count * options.profile.query_powers.size());

        const auto& package = compiled.package();
        std::cout << "APSI-compatible concentrated unlabeled PSI application: PASS\n"
                  << "Scope: cropped protocol with deterministic CPU OPRF-output fixture; not full network APSI\n"
                  << "Profile/scenario: " << options.profile.name << '/' << options.scenario << '\n'
                  << "Sender/receiver/intersection: " << dataset.sender.size() << '/'
                  << dataset.receiver.size() << '/' << actual_intersection.size() << '\n'
                  << "N/t/felts/hash/table/bundles: " << options.profile.degree << '/'
                  << plain_modulus << '/' << options.profile.felts_per_item << '/'
                  << options.profile.hash_function_count << '/' << options.profile.table_size
                  << '/' << bundle_count << '\n'
                  << "Max configured/observed polynomial degree: "
                  << options.profile.max_items_per_bin << '/' << observed_degree << '\n'
                  << "PowersDag source/target powers: " << options.profile.query_powers.size()
                  << '/' << targets.size() << '\n'
                  << "Packages/nodes/instructions/DMA/HPU_MEM lines: 1/"
                  << compiled.node_count() << '/' << package.program.instructions.size() << '/'
                  << package.program.ordered_dma_spans.size() << '/'
                  << package.initial_image->used_lines() << '\n'
                  << "Minimum output noise budget: " << minimum_noise << " bits\n"
                  << "Compile plus software verification: " << std::fixed << std::setprecision(3)
                  << compile_seconds << " seconds\n"
                  << "SEAL per-node oracle comparison: PASS\n"
                  << "HPU software execution and final intersection: PASS\n"
                  << "Host encoded-instruction model: "
                  << (compiled.instruction_verified() ? "PASS" : "NOT VERIFIED") << '\n'
                  << "Real HPU/RTL execution: NOT RUN\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Concentrated APSI application failed: " << error.what() << '\n';
        return 1;
    }
}
