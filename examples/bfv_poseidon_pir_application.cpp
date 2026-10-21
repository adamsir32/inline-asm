#include "evaluator_application.hpp"

#include "hpu/seal/bfv_context.hpp"

#include <seal/seal.h>
#include <seal/util/numth.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Traceable application scale: SealPIR test/expand_test.cpp, which is the
// implementation family named by Poseidon's PIR documentation and historical
// Trident/pir_bfv graph artifacts.
constexpr std::size_t kElementCount = 2048;
constexpr std::size_t kElementBytes = 288;
constexpr std::size_t kDegree = 4096;
constexpr std::size_t kDimensions = 1;
constexpr std::size_t kTargetElement = 1234;
constexpr std::uint32_t kUsablePlainBits = 20;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint8_t> database_element(std::size_t index)
{
    std::vector<std::uint8_t> result(kElementBytes);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::uint8_t>((index * 131 + i * 17 + 11) & 0xffU);
    return result;
}

std::vector<std::uint64_t> bytes_to_coefficients(const std::vector<std::uint8_t>& bytes)
{
    const std::size_t count = (bytes.size() * 8 + kUsablePlainBits - 1) / kUsablePlainBits;
    std::vector<std::uint64_t> result(count);
    std::size_t out = 0;
    std::uint32_t room = kUsablePlainBits;
    for (std::uint8_t byte : bytes) {
        std::uint8_t source = byte;
        std::uint32_t remaining = 8;
        while (remaining) {
            if (!room) { ++out; room = kUsablePlainBits; }
            const auto take = std::min(room, remaining);
            result[out] = (result[out] << take) | (source >> (8 - take));
            source = static_cast<std::uint8_t>(source << take);
            room -= take;
            remaining -= take;
        }
    }
    result[out] <<= room;
    return result;
}

std::vector<std::uint8_t> coefficients_to_bytes(const std::vector<std::uint64_t>& coefficients)
{
    std::vector<std::uint8_t> result(kElementBytes);
    std::size_t out = 0;
    std::uint32_t room = 8;
    std::uint32_t bits_left = static_cast<std::uint32_t>(kElementBytes * 8);
    for (std::uint64_t source : coefficients) {
        std::uint32_t remaining = std::min(kUsablePlainBits, bits_left);
        while (remaining && out < result.size()) {
            const auto take = std::min(room, remaining);
            result[out] = static_cast<std::uint8_t>(
                (result[out] << take) | (source >> (kUsablePlainBits - take)));
            source <<= take;
            room -= take;
            remaining -= take;
            bits_left -= take;
            if (!room) { ++out; room = 8; }
        }
    }
    return result;
}

::seal::Plaintext monomial(std::size_t degree, std::uint32_t exponent,
                            std::uint64_t plain_modulus)
{
    (void)plain_modulus;
    exponent %= static_cast<std::uint32_t>(degree);
    ::seal::Plaintext result(degree);
    result.set_zero();
    result[exponent] = 1;
    return result;
}

std::size_t integer_log2_ceil(std::size_t value)
{
    std::size_t result = 0;
    for (std::size_t power = 1; power < value; power <<= 1) ++result;
    return result;
}

int rotate_rows_steps_for_galois_element(std::size_t degree,
                                         std::uint32_t galois_element)
{
    require(degree && !(degree & (degree - 1)),
            "BFV row rotation requires a power-of-two degree");
    const auto mask = static_cast<std::uint32_t>(2 * degree - 1);
    std::uint32_t candidate = 1;
    for (std::size_t steps = 1; steps < degree / 2; ++steps) {
        candidate = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(candidate) * 3) & mask);
        if (candidate == galois_element) return static_cast<int>(steps);
    }
    throw std::invalid_argument(
        "SealPIR Galois element is not representable by the existing BFV rotate_rows API");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = evaluator_example::options(argc, argv);
        if (options.delivery.poly_modulus_degree && *options.delivery.poly_modulus_degree != kDegree)
            throw std::invalid_argument("Poseidon PIR port fixes --degree to the source scale N=4096");

        // HPU compatibility adaptation: BFVDefault(4096) uses >31-bit primes,
        // while the current HPU ABI requires every Q/P prime to fit 31 bits.
        // Four 30-bit primes preserve a comparable aggregate modulus budget.
        hpu::seal_adapter::BfvContextSpec spec;
        spec.poly_modulus_degree = kDegree;
        spec.coeff_modulus_bits = {30, 30, 30, 30};
        spec.plain_modulus_bits = 21;
        const auto bundle = hpu::seal_adapter::create_bfv_context(spec);
        const auto& context = *bundle.context;
        const auto plain_modulus = bundle.plain_modulus;
        require(static_cast<std::uint32_t>(std::floor(std::log2(plain_modulus))) ==
                    kUsablePlainBits,
                "PIR plaintext modulus does not provide SealPIR's 20 usable bits");

        const std::size_t coefficients_per_element =
            (kElementBytes * 8 + kUsablePlainBits - 1) / kUsablePlainBits;
        const std::size_t elements_per_plaintext = kDegree / coefficients_per_element;
        const std::size_t plaintext_count =
            (kElementCount + elements_per_plaintext - 1) / elements_per_plaintext;
        // SealPIR get_dimensions increments an exact one-dimensional root.
        const std::size_t expanded_query_size = plaintext_count + 1;
        static_assert(kDimensions == 1);
        require(coefficients_per_element == 116 && elements_per_plaintext == 35 &&
                    plaintext_count == 59 && expanded_query_size == 60,
                "PIR packing no longer matches the source scale");

        // Layout adaptation: SealPIR feeds the 20-bit value vector through
        // BatchEncoder::encode. This runnable HPU kernel writes those values
        // directly as polynomial coefficients so the expanded coefficient
        // selector and the database share one explicit layout. The query
        // expansion and dot product are preserved; the database layout is not
        // yet a bit-for-bit reproduction of SealPIR's batched layout.
        std::vector<::seal::Plaintext> database(expanded_query_size);
        for (auto& plain : database) { plain.resize(kDegree); plain.set_zero(); }
        for (std::size_t item = 0; item < kElementCount; ++item) {
            const auto coefficients = bytes_to_coefficients(database_element(item));
            const auto plaintext_index = item / elements_per_plaintext;
            const auto offset = (item % elements_per_plaintext) * coefficients_per_element;
            std::copy(coefficients.begin(), coefficients.end(), database[plaintext_index].data() + offset);
        }
        // The modified-SEAL build enables transparent-ciphertext exceptions.
        // SealPIR pads 59 database plaintexts to a one-dimensional size of 60;
        // a non-zero sentinel in the unreachable padding plaintext prevents a
        // zero-plaintext product without changing any real database record.
        database.back()[0] = 1;

        const std::size_t selected_plaintext = kTargetElement / elements_per_plaintext;
        const std::size_t selected_offset =
            (kTargetElement % elements_per_plaintext) * coefficients_per_element;
        const auto logm = integer_log2_ceil(expanded_query_size);
        const auto query_scale = std::uint64_t{1} << logm;
        ::seal::Plaintext query_plain(kDegree);
        query_plain.set_zero();
        std::uint64_t inverse_query_scale = 0;
        require(::seal::util::try_invert_uint_mod(query_scale, plain_modulus,
                                                  inverse_query_scale),
                "PIR query scale is not invertible modulo t");
        query_plain[selected_plaintext] = inverse_query_scale;

        ::seal::KeyGenerator keygen(context);
        ::seal::PublicKey public_key;
        keygen.create_public_key(public_key);
        std::vector<std::uint32_t> galois_elements;
        for (std::size_t i = 0; i < logm; ++i)
            galois_elements.push_back(static_cast<std::uint32_t>(kDegree / (std::size_t{1} << i) + 1));
        ::seal::GaloisKeys galois_keys;
        keygen.create_galois_keys(galois_elements, galois_keys);
        ::seal::Ciphertext encrypted_query;
        ::seal::Encryptor(context, public_key).encrypt(query_plain, encrypted_query);

        hpu::seal_adapter::ApplicationBuilder app(context);
        auto evaluator = app.evaluator();
        std::vector<hpu::seal_adapter::Plaintext> db_handles;
        db_handles.reserve(database.size());
        for (std::size_t i = 0; i < database.size(); ++i)
            db_handles.push_back(app.import_plaintext("database/plain_" + std::to_string(i), database[i]));
        std::map<std::uint32_t, hpu::seal_adapter::Plaintext> monomials;
        auto monomial_handle = [&](std::uint32_t exponent) -> const hpu::seal_adapter::Plaintext& {
            exponent %= static_cast<std::uint32_t>(kDegree);
            auto found = monomials.find(exponent);
            if (found == monomials.end()) {
                found = monomials.emplace(
                    exponent, app.import_plaintext(
                        "constant/monomial_x_" + std::to_string(exponent),
                        monomial(kDegree, exponent, plain_modulus))).first;
            }
            return found->second;
        };
        auto multiply_monomial = [&](const hpu::seal_adapter::Ciphertext& input,
                                     std::uint32_t exponent) {
            exponent %= static_cast<std::uint32_t>(2 * kDegree);
            const bool negative = exponent >= kDegree;
            hpu::seal_adapter::Ciphertext output;
            evaluator.multiply_plain(input, monomial_handle(exponent % kDegree), output);
            if (negative) evaluator.negate_inplace(output);
            return output;
        };
        ::seal::Plaintext two_native("2");
        const auto two = app.import_plaintext("constant/two", two_native);
        std::vector<hpu::seal_adapter::Ciphertext> expanded{
            app.import_ciphertext("input/compressed_query", encrypted_query)};

        // Faithful translation of SealPIR/Poseidon PIRServer::expand_query.
        // Its Galois elements belong to BFV's row-rotation subgroup, so the
        // application maps them to steps and uses the original public
        // rotate_rows API instead of requiring an arbitrary-automorphism API.
        for (std::size_t level = 0; level < logm; ++level) {
            const auto old_size = expanded.size();
            std::vector<hpu::seal_adapter::Ciphertext> next(old_size * 2);
            const auto raw = static_cast<std::uint32_t>(2 * kDegree - (std::size_t{1} << level));
            const auto element = galois_elements[level];
            const auto shifted = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(raw) * element) % (2 * kDegree));
            for (std::size_t a = 0; a < old_size; ++a) {
                const bool corner = level + 1 == logm &&
                    a >= expanded_query_size - (std::size_t{1} << (logm - 1));
                if (corner) {
                    evaluator.multiply_plain(expanded[a], two, next[a]);
                    continue;
                }
                hpu::seal_adapter::Ciphertext rotated;
                evaluator.rotate_rows(
                    expanded[a],
                    rotate_rows_steps_for_galois_element(kDegree, element),
                    galois_keys, rotated);
                evaluator.add(expanded[a], rotated, next[a]);
                auto left_shifted = multiply_monomial(expanded[a], raw);
                auto right_shifted = multiply_monomial(rotated, shifted);
                evaluator.add(left_shifted, right_shifted, next[a + old_size]);
            }
            expanded = std::move(next);
        }
        expanded.resize(expanded_query_size);

        hpu::seal_adapter::Ciphertext reply;
        evaluator.multiply_plain(expanded[0], db_handles[0], reply);
        for (std::size_t i = 1; i < expanded_query_size; ++i) {
            hpu::seal_adapter::Ciphertext term;
            evaluator.multiply_plain(expanded[i], db_handles[i], term);
            evaluator.add_inplace(reply, term);
        }
        app.mark_output("pir_reply", reply);
        const auto compiled = app.compile(
            delivery_artifact_stem("bfv_poseidon_pir_application", options.delivery), options.compiler);
        require(compiled.verified(),
                "PIR HPU software execution did not match the SEAL oracle coefficients");
        require(compiled.instruction_verified(),
                "PIR encoded instruction model verification failed");

        ::seal::Plaintext decrypted;
        ::seal::Decryptor(context, keygen.secret_key()).decrypt(compiled.oracle_result(reply), decrypted);
        std::vector<std::uint64_t> selected_coefficients(coefficients_per_element);
        std::copy_n(decrypted.data() + selected_offset, coefficients_per_element,
                    selected_coefficients.begin());
        const auto recovered = coefficients_to_bytes(selected_coefficients);
        require(recovered == database_element(kTargetElement),
                "PIR reply did not recover the selected 288-byte database element");

        std::vector<std::uint64_t> semantic_words(recovered.begin(), recovered.end());
        evaluator_example::publish(options, compiled,
            hpu::seal_adapter::integer_delivery_semantics(semantic_words));
        std::cout << "Poseidon/SealPIR BFV application: PASS\n"
                  << "Source scale: 2048 records x 288 bytes, N=4096, d=1\n"
                  << "Packed database: 35 records/plaintext, 59 used + 1 padding plaintext\n"
                  << "Deterministic selected record: " << kTargetElement << "\n"
                  << "Layout adaptation: database uses direct polynomial coefficients, not SealPIR BatchEncoder slots\n"
                  << "Build adaptation: unreachable padding plaintext uses a non-zero sentinel\n"
                  << "HPU adaptation: BFVDefault replaced by four 30-bit Q/P primes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Poseidon PIR application failed: " << error.what() << '\n';
        return 1;
    }
}
