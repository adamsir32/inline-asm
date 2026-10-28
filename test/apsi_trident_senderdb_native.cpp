// Build only in an isolated checkout of Trident e3f819f845f3cee7425fa08a3aa64879cfd5b3c0.
// SenderDB performs its original OPRF, Kuku preprocessing, insertion, and cache generation.
#include <apsi/oprf/oprf_sender.h>
#include <apsi/powers.h>
#include <apsi/sender_db.h>
#include <apsi/util/db_encoding.h>
#include <apsi/util/utils.h>
#include <kuku/kuku.h>
#include <kuku/locfunc.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using Ids = std::vector<std::uint64_t>;

Ids read_ids(std::istream &input, char wanted)
{
    Ids result;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        char side;
        std::uint64_t id;
        std::string extra;
        if (!(row >> side >> id) || (side != 'S' && side != 'R'))
            throw std::runtime_error("invalid raw input row");
        std::uint64_t low, high;
        if (row >> low) {
            if (side != 'R' || !(row >> high) || (row >> extra))
                throw std::runtime_error("only receiver rows may override an OPRF token");
        }
        if (side == wanted) result.push_back(id);
    }
    std::sort(result.begin(), result.end());
    if (std::adjacent_find(result.begin(), result.end()) != result.end())
        throw std::runtime_error("duplicate raw input item");
    return result;
}

std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>>
receiver_overrides(const char *path)
{
    std::ifstream input(path);
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> result;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        char side;
        std::uint64_t id, low, high;
        row >> side >> id;
        if (side == 'R' && (row >> low >> high))
            result.emplace(id, std::make_pair(low, high));
    }
    return result;
}

std::vector<apsi::Item> raw_items(const Ids &ids)
{
    std::vector<apsi::Item> result;
    result.reserve(ids.size());
    for (auto id : ids) result.emplace_back(id, UINT64_C(0));
    return result;
}

std::vector<std::uint64_t> felts(const apsi::HashedItem &item,
                                  const apsi::PSIParams &params)
{
    auto bytes = item.value();
    apsi::BitstringView<const unsigned char> bits(bytes.data(), params.item_bit_count());
    return apsi::util::bits_to_field_elts(bits, params.poseidon_params().plain_modulus());
}

std::vector<std::size_t> locations(const apsi::HashedItem &item,
                                    const std::vector<kuku::LocFunc> &functions)
{
    std::unordered_set<std::size_t> unique;
    const auto words = item.get_as<std::uint64_t>();
    auto kuku_item = kuku::make_item(words[0], words[1]);
    for (const auto &function : functions) unique.insert(function(kuku_item));
    std::vector<std::size_t> result(unique.begin(), unique.end());
    std::sort(result.begin(), result.end());
    return result;
}

std::uint64_t eval(const std::vector<std::uint64_t> &coefficients,
                   std::uint64_t x, std::uint64_t modulus)
{
    std::uint64_t value = 0;
    for (auto it = coefficients.rbegin(); it != coefficients.rend(); ++it)
        value = static_cast<std::uint64_t>(
            (static_cast<__uint128_t>(value) * x + *it) % modulus);
    return value;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        if (argc == 5 && std::string(argv[1]) == "--catalog") {
            std::ifstream params_file(argv[2]);
            std::ofstream catalog(argv[4]);
            if (!params_file || !catalog) throw std::runtime_error("catalog file unavailable");
            std::string json(std::istreambuf_iterator<char>{params_file}, {});
            auto params = apsi::PSIParams::Load(json);
            std::array<unsigned char, apsi::oprf::oprf_key_size> key_bytes{};
            key_bytes[0] = 1;
            apsi::oprf::OPRFKey key;
            key.load(apsi::oprf::oprf_key_span_const_type(
                key_bytes.data(), key_bytes.size()));
            std::vector<kuku::LocFunc> functions;
            for (std::uint32_t i = 0; i < params.table_params().hash_func_count; ++i)
                functions.emplace_back(params.table_params().table_size,
                                       kuku::make_item(i, 0));
            const auto count = std::stoull(argv[3]);
            for (std::uint64_t first = 1; first <= count; first += 512) {
                Ids ids;
                for (std::uint64_t id = first; id < std::min<std::uint64_t>(first + 512, count + 1);
                     ++id) ids.push_back(id);
                auto hashes = apsi::oprf::OPRFSender::ComputeHashes(raw_items(ids), key);
                for (std::size_t i = 0; i < ids.size(); ++i) {
                    catalog << ids[i] << " felts";
                    for (auto felt : felts(hashes[i], params)) catalog << ' ' << felt;
                    catalog << " locations";
                    for (auto location : locations(hashes[i], functions)) catalog << ' ' << location;
                    catalog << '\n';
                }
            }
            if (!catalog) throw std::runtime_error("catalog write failed");
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--cross") {
            std::ifstream params_file(argv[2]);
            std::ifstream input_file(argv[3]);
            std::ofstream output(argv[4]);
            if (!params_file || !input_file || !output)
                throw std::runtime_error("cross-field case file unavailable");
            std::string json(std::istreambuf_iterator<char>{params_file}, {});
            auto params = apsi::PSIParams::Load(json);
            auto ids = read_ids(input_file, 'S');
            if (ids.size() < 2) throw std::runtime_error("cross-field case requires two senders");
            std::array<unsigned char, apsi::oprf::oprf_key_size> key_bytes{};
            key_bytes[0] = 1;
            apsi::oprf::OPRFKey key;
            key.load(apsi::oprf::oprf_key_span_const_type(
                key_bytes.data(), key_bytes.size()));
            auto hashes = apsi::oprf::OPRFSender::ComputeHashes(raw_items(ids), key);
            auto first = felts(hashes[0], params);
            auto second = felts(hashes[1], params);
            first[0] = second[0]; // mixed item takes one felt from B and the rest from A
            const auto bits_per_felt = params.item_bit_count_per_felt();
            __uint128_t packed = 0;
            for (std::size_t i = 0; i < first.size(); ++i)
                packed |= static_cast<__uint128_t>(first[i]) << (i * bits_per_felt);
            const auto low = static_cast<std::uint64_t>(packed);
            const auto high_base = static_cast<std::uint64_t>(packed >> 64);
            std::vector<kuku::LocFunc> functions;
            for (std::uint32_t i = 0; i < params.table_params().hash_func_count; ++i)
                functions.emplace_back(params.table_params().table_size,
                                       kuku::make_item(i, 0));
            auto a_locations = locations(hashes[0], functions);
            auto b_locations = locations(hashes[1], functions);
            std::vector<std::size_t> common;
            std::set_intersection(a_locations.begin(), a_locations.end(),
                                  b_locations.begin(), b_locations.end(),
                                  std::back_inserter(common));
            if (common.empty()) throw std::runtime_error("senders share no Kuku bin");
            std::uint64_t high = 0;
            for (std::uint64_t nonce = 1; nonce < UINT64_C(1000000); ++nonce) {
                const auto candidate = high_base | (nonce << (params.item_bit_count() - 64));
                apsi::HashedItem mixed(low, candidate);
                auto candidate_locations = locations(mixed, functions);
                if (!std::binary_search(candidate_locations.begin(), candidate_locations.end(),
                                        common.front())) continue;
                kuku::KukuTable trial(params.table_params().table_size, 0,
                                      params.table_params().hash_func_count,
                                      kuku::make_item(0, 0), 500, kuku::make_item(0, 0));
                auto item = kuku::make_item(low, candidate);
                if (trial.insert(item) && trial.query(item).location() == common.front()) {
                    high = candidate;
                    break;
                }
            }
            if (!high) throw std::runtime_error("cross-field Kuku nonce search exhausted");
            output << "# synthetic OPRF-boundary receiver, native sender OPRF scalar 1\n";
            for (auto id : ids) output << "S " << id << '\n';
            output << "R 999999 " << low << ' ' << high << '\n';
            return 0;
        }
        if (argc != 5) throw std::runtime_error(
            "usage: apsi_native_senderdb PARAMS_JSON RAW_INPUT HPU_FIXTURE NATIVE_DUMP"
            " | --catalog PARAMS_JSON COUNT CATALOG_OUT");
        std::ifstream params_file(argv[1]);
        std::ifstream input_file(argv[2]);
        if (!params_file || !input_file) throw std::runtime_error("cannot open input file");
        std::string params_json(std::istreambuf_iterator<char>{params_file}, {});
        auto params = apsi::PSIParams::Load(params_json);
        auto sender_ids = read_ids(input_file, 'S');
        input_file.clear();
        input_file.seekg(0);
        auto receiver_ids = read_ids(input_file, 'R');
        if (sender_ids.empty() || receiver_ids.empty())
            throw std::runtime_error("both sides require at least one item");

        std::array<unsigned char, apsi::oprf::oprf_key_size> key_bytes{};
        key_bytes[0] = 1; // fixed, valid nonzero FourQ scalar; audit fixture only
        apsi::oprf::OPRFKey key;
        key.load(apsi::oprf::oprf_key_span_const_type(
            key_bytes.data(), key_bytes.size()));
        auto sender_items = raw_items(sender_ids);
        auto receiver_items = raw_items(receiver_ids);
        auto sender_hashes = apsi::oprf::OPRFSender::ComputeHashes(sender_items, key);
        auto receiver_hashes = apsi::oprf::OPRFSender::ComputeHashes(receiver_items, key);
        auto overrides = receiver_overrides(argv[2]);
        for (std::size_t i = 0; i < receiver_ids.size(); ++i) {
            auto found = overrides.find(receiver_ids[i]);
            if (found != overrides.end())
                receiver_hashes[i] = apsi::HashedItem(found->second.first,
                                                        found->second.second);
        }
        apsi::sender::SenderDB database(params, key, 0, 16, false);
        database.insert_or_assign(sender_items);

        std::vector<kuku::LocFunc> functions;
        for (std::uint32_t i = 0; i < params.table_params().hash_func_count; ++i)
            functions.emplace_back(params.table_params().table_size,
                                   kuku::make_item(i, 0));
        kuku::KukuTable table(params.table_params().table_size, 0,
                             params.table_params().hash_func_count,
                             kuku::make_item(0, 0), 500, kuku::make_item(0, 0));
        for (const auto &item : receiver_hashes) {
            const auto words = item.get_as<std::uint64_t>();
            if (!table.insert(kuku::make_item(words[0], words[1])))
                throw std::runtime_error("receiver Kuku insertion failed");
        }

        std::ofstream fixture(argv[3]);
        std::ofstream dump(argv[4]);
        if (!fixture || !dump) throw std::runtime_error("cannot create output files");
        fixture << "# native Trident e3f819f fixed OPRF scalar 1\n";
        dump << "META trident=e3f819f845f3cee7425fa08a3aa64879cfd5b3c0"
             << " sender=" << sender_ids.size() << " receiver=" << receiver_ids.size()
             << " table=" << params.table_params().table_size
             << " bins_per_bundle=" << params.bins_per_bundle()
             << " plain_modulus=" << params.poseidon_params().plain_modulus().value()
             << " bits_per_felt=" << params.item_bit_count_per_felt() << '\n';
        for (int side = 0; side < 2; ++side) {
            const auto &ids = side ? receiver_ids : sender_ids;
            const auto &hashes = side ? receiver_hashes : sender_hashes;
            const char label = side ? 'R' : 'S';
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const auto words = hashes[i].get_as<std::uint64_t>();
                fixture << label << ' ' << ids[i] << ' ' << words[0] << ' ' << words[1] << '\n';
                dump << "TOKEN " << label << ' ' << ids[i] << " felts";
                for (auto felt : felts(hashes[i], params)) dump << ' ' << felt;
                dump << " locations";
                for (auto location : locations(hashes[i], functions)) dump << ' ' << location;
                if (side) {
                    auto token = kuku::make_item(words[0], words[1]);
                    dump << " placed " << table.query(token).location();
                }
                dump << '\n';
            }
        }

        for (std::uint32_t bundle_idx = 0; bundle_idx < params.bundle_idx_count(); ++bundle_idx) {
            const auto &bundles = database.audit_bundles_at(bundle_idx);
            for (std::size_t instance = 0; instance < bundles.size(); ++instance) {
                const auto &bins = bundles[instance].get_item_bins();
                const auto &polyns = bundles[instance].get_cache().felt_matching_polyns;
                if (bins.size() != polyns.size())
                    throw std::runtime_error("native BinBundle bin/cache size mismatch");
                std::size_t degree = 0, nonempty = 0;
                for (std::size_t slot = 0; slot < bins.size(); ++slot) {
                    degree = std::max(degree, bins[slot].size());
                    if (bins[slot].empty()) {
                        if (polyns[slot] != std::vector<std::uint64_t>{1})
                            throw std::runtime_error("native empty bin polynomial differs from one");
                        continue;
                    }
                    ++nonempty;
                    dump << "BIN " << instance << ' ' << bundle_idx << ' ' << slot << " roots";
                    for (auto root : bins[slot]) dump << ' ' << root;
                    dump << " coefficients";
                    for (auto coefficient : polyns[slot]) dump << ' ' << coefficient;
                    dump << '\n';
                }
                dump << "BUNDLE " << instance << ' ' << bundle_idx << " degree " << degree
                     << " nonempty " << nonempty << " slots " << bins.size() << '\n';
            }
        }

        apsi::PowersDag dag;
        if (!dag.configure(params.query_params().query_powers,
                           apsi::util::create_powers_set(
                               params.query_params().ps_low_degree,
                               params.table_params().max_items_per_bin)))
            throw std::runtime_error("native PowersDag configuration failed");
        dag.apply([&](const auto &node) {
            dump << "POWER " << node.power << ' ' << node.depth << ' '
                 << node.parents.first << ' ' << node.parents.second << '\n';
        });

        const auto modulus = params.poseidon_params().plain_modulus().value();
        for (std::size_t i = 0; i < receiver_ids.size(); ++i) {
            const auto words = receiver_hashes[i].get_as<std::uint64_t>();
            const auto location = table.query(kuku::make_item(words[0], words[1])).location();
            const auto bundle_idx = location / params.items_per_bundle();
            const auto base = (location % params.items_per_bundle()) *
                              params.item_params().felts_per_item;
            const auto fields = felts(receiver_hashes[i], params);
            bool matched = false;
            for (const auto &bundle : database.audit_bundles_at(
                     static_cast<std::uint32_t>(bundle_idx))) {
                const auto &polyns = bundle.get_cache().felt_matching_polyns;
                bool zeros = true;
                for (std::size_t field = 0; field < fields.size(); ++field)
                    zeros &= eval(polyns.at(base + field), fields[field], modulus) == 0;
                matched |= zeros;
            }
            if (matched) dump << "MATCH " << receiver_ids[i] << '\n';
        }
    } catch (const std::exception &error) {
        std::cerr << "native SenderDB audit failed: " << error.what() << '\n';
        return 1;
    }
}
