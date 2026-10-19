#include "evaluator_application.hpp"

#include "hpu/seal/bfv_context.hpp"

#include <seal/seal.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// Exact APSI/Poseidon small-test scale (create_params1 + UnlabeledSmall1).
constexpr std::size_t kSenderSize = 10;
constexpr std::size_t kReceiverSize = 5;
constexpr std::size_t kIntersectionSize = 2;
constexpr std::size_t kFeltsPerItem = 8;
constexpr std::size_t kHashFunctionCount = 3;
constexpr std::size_t kMaxItemsPerBin = 16;
constexpr std::size_t kTableSize = 4096;
constexpr std::size_t kDegree = 8192;
constexpr std::uint64_t kPoseidonPlainModulus = 65537;
const std::set<std::uint32_t> kQueryPowers{1, 3, 5};
const std::set<std::uint32_t> kHpuInputPowers{1,2,3,4,5,6,7,8,9,10};

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t add_mod(std::uint64_t left, std::uint64_t right, std::uint64_t modulus)
{
    return (left + right) % modulus;
}

std::uint64_t multiply_mod(std::uint64_t left, std::uint64_t right, std::uint64_t modulus)
{
    return (left * right) % modulus;
}

std::uint64_t power_mod(std::uint64_t value, std::uint32_t power, std::uint64_t modulus)
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
    std::vector<std::uint64_t> coefficients{1};
    for (auto root : roots) {
        std::vector<std::uint64_t> next(coefficients.size() + 1);
        const auto negative_root = root ? modulus - root : 0;
        for (std::size_t i = 0; i < coefficients.size(); ++i) {
            next[i] = add_mod(next[i], multiply_mod(coefficients[i], negative_root, modulus), modulus);
            next[i + 1] = add_mod(next[i + 1], coefficients[i], modulus);
        }
        coefficients = std::move(next);
    }
    return coefficients;
}

std::uint64_t evaluate_polynomial(const std::vector<std::uint64_t>& coefficients,
                                  std::uint64_t value, std::uint64_t modulus)
{
    std::uint64_t result = 0;
    for (auto it = coefficients.rbegin(); it != coefficients.rend(); ++it)
        result = add_mod(multiply_mod(result, value, modulus), *it, modulus);
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = evaluator_example::options(argc, argv);
        if (options.delivery.poly_modulus_degree && *options.delivery.poly_modulus_degree != kDegree)
            throw std::invalid_argument("Poseidon PSI port fixes --degree to the source scale N=8192");

        // HPU compatibility adaptation: APSI's BFVDefault(8192) contains
        // >31-bit primes. With ciphertext-power generation moved to the
        // client, four 31-bit primes are sufficient for the remaining
        // MultiplyPlain/Add kernel and satisfy the HPU uint32 ABI.
        hpu::seal_adapter::BfvContextSpec spec;
        spec.poly_modulus_degree = kDegree;
        spec.coeff_modulus_bits = {31, 31, 31, 31};
        spec.plain_modulus_bits = 17;
        const auto bundle = hpu::seal_adapter::create_bfv_context(spec);
        const auto& context = *bundle.context;
        const auto plain_modulus = static_cast<std::uint64_t>(bundle.plain_modulus);

        // Deterministic representative of the exact UnlabeledSmall1 (5,2)
        // scenario. APSI randomly chooses the two matching sender items; fixed
        // values make package goldens reproducible.
        const std::vector<std::uint64_t> sender_items{1,2,3,4,5,6,7,8,9,10};
        const std::vector<std::uint64_t> receiver_items{1,4,101,102,103};
        const auto polynomial = root_polynomial(sender_items, plain_modulus);
        require(polynomial.size() == kSenderSize + 1,
                "PSI sender polynomial has the wrong degree");

        ::seal::KeyGenerator keygen(context);
        ::seal::RelinKeys relin_keys;
        keygen.create_relin_keys(relin_keys);
        ::seal::BatchEncoder encoder(context);
        ::seal::Encryptor encryptor(context, keygen.secret_key());

        std::map<std::uint32_t, ::seal::Ciphertext> encrypted_sources;
        for (auto source_power : kHpuInputPowers) {
            std::vector<std::uint64_t> slots(kDegree);
            for (std::size_t i = 0; i < receiver_items.size(); ++i)
                slots[i] = power_mod(receiver_items[i], source_power, plain_modulus);
            ::seal::Plaintext encoded;
            encoder.encode(slots, encoded);
            // APSI PlaintextPowers::encrypt uses symmetric encryption.
            encryptor.encrypt_symmetric(encoded, encrypted_sources[source_power]);
        }

        hpu::seal_adapter::ApplicationBuilder app(context);
        auto evaluator = app.evaluator();
        std::map<std::uint32_t, hpu::seal_adapter::Ciphertext> powers;
        for (const auto& entry : encrypted_sources)
            powers.emplace(entry.first, app.import_ciphertext(
                "input/query_power_" + std::to_string(entry.first), entry.second));

        // APSI PowersDag::configure policy, with x^2..x^10 supplied as an
        // explicit HPU compatibility input. The stock sender precomputes
        // through max_items_per_bin=16; this HPU graph
        // explicitly prunes x^11..x^16 because the selected sender-size-10
        // scenario produces a degree-10 polynomial and cannot consume them.
        std::map<std::uint32_t, std::uint32_t> depths;
        for (auto power : kHpuInputPowers) depths[power] = 0;
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> parents;
        for (std::uint32_t power = 1; power <= kSenderSize; ++power) {
            if (powers.count(power)) continue;
            std::uint32_t optimal_depth = power - 1;
            std::pair<std::uint32_t, std::uint32_t> optimal{power - 1, 1};
            for (std::uint32_t left = 1; left < power; ++left) {
                const auto right = power - left;
                if (!depths.count(left) || !depths.count(right)) continue;
                const auto depth = std::max(depths.at(left), depths.at(right)) + 1;
                if (depth < optimal_depth) {
                    optimal_depth = depth;
                    optimal = {left, right};
                }
            }
            require(depths.count(optimal.first) && depths.count(optimal.second),
                    "APSI PowersDag target is unreachable");
            depths[power] = optimal_depth;
            parents[power] = optimal;
            hpu::seal_adapter::Ciphertext product;
            if (optimal.first == optimal.second)
                evaluator.square(powers.at(optimal.first), product);
            else
                evaluator.multiply(powers.at(optimal.first), powers.at(optimal.second), product);
            evaluator.relinearize_inplace(product, relin_keys);
            powers[power] = product;
        }

        // Port boundary: APSI switches powers to chain index 1 and converts
        // them to NTT form. HPU MultiplyPlain owns its NTT round trip and the
        // current N=8192 BFV model does not yet differentially match SEAL for
        // these data-dependent ModSwitch inputs, so this runnable port keeps
        // the powers at the top data level. This is an explicit compatibility
        // adaptation, not a claim that Poseidon omits modulus switching.

        std::vector<std::uint64_t> constant_slots(kDegree, polynomial[0]);
        ::seal::Plaintext encoded_constant;
        encoder.encode(constant_slots, encoded_constant);
        const auto constant = app.import_plaintext("sender/coefficient_0", encoded_constant);

        // Equivalent instruction-level lowering of MultiplyPlain(C, scalar):
        // binary double-and-add avoids the current N=8192 prepared-plaintext
        // mismatch for large APSI polynomial coefficients.
        auto multiply_scalar = [&](const hpu::seal_adapter::Ciphertext& input,
                                   std::uint64_t scalar) {
            hpu::seal_adapter::Ciphertext result, multiple = input;
            bool initialized = false;
            while (scalar) {
                if (scalar & 1U) {
                    if (!initialized) { result = multiple; initialized = true; }
                    else evaluator.add_inplace(result, multiple);
                }
                scalar >>= 1U;
                if (scalar) evaluator.add_inplace(multiple, multiple);
            }
            if (!initialized) evaluator.sub(input, input, result);
            return result;
        };

        // APSI BatchedPlaintextPolyn::eval core: sum a_i*C^i and add a_0.
        hpu::seal_adapter::Ciphertext result = multiply_scalar(powers.at(1), polynomial[1]);
        for (std::uint32_t degree = 2; degree <= kSenderSize; ++degree) {
            auto term = multiply_scalar(powers.at(degree), polynomial[degree]);
            evaluator.add_inplace(result, term);
        }
        evaluator.add_plain_inplace(result, constant);
        app.mark_output("psi_zero_test", result);

        const auto compiled = app.compile(
            delivery_artifact_stem("bfv_poseidon_psi_application", options.delivery), options.compiler);
        ::seal::Plaintext decrypted;
        ::seal::Decryptor(context, keygen.secret_key()).decrypt(compiled.oracle_result(result), decrypted);
        std::vector<std::uint64_t> decoded;
        encoder.decode(decrypted, decoded);

        std::vector<std::uint64_t> semantic_values;
        semantic_values.reserve(kReceiverSize);
        std::size_t zero_count = 0;
        for (std::size_t i = 0; i < receiver_items.size(); ++i) {
            const auto expected = evaluate_polynomial(polynomial, receiver_items[i], plain_modulus);
            require(decoded[i] == expected, "PSI polynomial oracle differs from clear evaluation");
            zero_count += decoded[i] == 0;
            semantic_values.push_back(decoded[i]);
        }
        require(zero_count == kIntersectionSize && decoded[0] == 0 && decoded[1] == 0,
                "PSI zero-test did not identify the expected two-item intersection");

        evaluator_example::publish(options, compiled,
            hpu::seal_adapter::integer_delivery_semantics(semantic_values));
        std::cout << "Poseidon/APSI BFV application: PASS\n"
                  << "Source scale: sender=10, receiver=5, intersection=2, N=8192, t="
                  << kPoseidonPlainModulus << "\n"
                  << "create_params1: felts/item=" << kFeltsPerItem
                  << ", hashes=" << kHashFunctionCount << ", table=" << kTableSize
                  << ", max/bin=" << kMaxItemsPerBin << ", query powers={1,3,5}\n"
                  << "Deterministic intersection items: {1,4}\n"
                  << "HPU graph pruning: unused powers x^11..x^16 omitted\n"
                  << "HPU query adaptation: client supplies x^1 through x^10\n"
                  << "HPU lowering: scalar MultiplyPlain expanded to double-and-add\n"
                  << "HPU compatibility: APSI power/result ModSwitch omitted at N=8192\n"
                  << "HPU adaptation: BFVDefault replaced by four 31-bit Q/P primes\n";
        std::cout << "HPU plaintext-modulus adaptation: t=" << plain_modulus << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Poseidon PSI application failed: " << error.what() << '\n';
        return 1;
    }
}
