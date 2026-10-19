#include "hpu/seal/evaluator.hpp"
#include "hpu/seal/bfv_context.hpp"
#include "hpu/seal/ckks_context.hpp"
#include "scheme/ckks/galois.hpp"

#include <seal/seal.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace front = hpu::seal_adapter;

void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

front::CompileOptions diagnostic_options(bool optimize)
{
    front::CompileOptions result;
    result.optimize_resources = optimize;
    // This coverage test checks frontend resource preparation and both
    // functional models. The encoded NTT baseline has a separate diagnostic.
    result.require_instruction_verification = false;
    return result;
}

void check_pinned_writes(const front::CompiledApplication& compiled)
{
    const auto& package = compiled.package();
    std::map<std::uint64_t, std::size_t> producers;
    for (const auto& output : package.outputs)
        producers.emplace(output.destination.line_offset, std::stoull(output.object_id.substr(5)));
    std::istringstream csv(package.program.resolved_dma_manifest);
    std::string line;
    std::getline(csv, line);
    while (std::getline(csv, line)) {
        if (line.empty()) continue;
        // These leading columns contain only restricted identifier paths.
        std::istringstream row(line);
        std::vector<std::string> fields(12);
        for (auto& field : fields) std::getline(row, field, ',');
        if (fields[5] != "dstore") continue;
        const auto found = producers.find(std::stoull(fields[10]));
        if (found != producers.end())
            require(!fields[2].empty() && found->second == std::stoull(fields[2]),
                "a downstream operation overwrites a pinned predecessor result");
    }
}

void check_compilations(front::ApplicationBuilder& application,
                        const front::Ciphertext& output, const std::string& stem,
                        const std::filesystem::path* emit_directory = nullptr)
{
    const auto baseline = application.compile(stem + "_baseline", diagnostic_options(false));
    const auto optimized = application.compile(stem + "_optimized", diagnostic_options(true));
    require(baseline.verified() && optimized.verified(), "frontend functional verification failed");
    check_pinned_writes(baseline);
    check_pinned_writes(optimized);
    require(baseline.node_count() == optimized.node_count(), "optimization changed graph node count");
    const auto& a = baseline.package().outputs;
    const auto& b = optimized.package().outputs;
    require(a.size() == b.size(), "optimization changed golden limb count");
    for (std::size_t i = 0; i < a.size(); ++i)
        require(a[i].object_id == b[i].object_id && a[i].component == b[i].component &&
            a[i].modulus_id == b[i].modulus_id && a[i].golden_words == b[i].golden_words,
            "optimization changed a pinned node result");
    require(baseline.oracle_result(output).parms_id() == output.parms_id(),
        "compiled output oracle has wrong level");
    require(optimized.package().initial_image->used_lines() <= baseline.package().initial_image->used_lines(),
        "optimization increased DDR footprint");
    // CKKS/BFV prepare one key identity for each type, level, and Galois
    // element despite repeated uses and independently copied native keys.
    if (baseline.package().scheme != hpu::delivery::SchemeKind::bgv) {
        std::set<std::string> keys;
        for (const auto& allocation : baseline.package().initial_image->allocations()) {
            if (allocation.kind != hpu::runtime::AllocationKind::evaluation_key) continue;
            const auto digit = allocation.id.rfind("/d");
            require(digit != std::string::npos, "prepared evaluation key lacks digit identity");
            keys.insert(allocation.id.substr(0, digit));
        }
        const auto expected = baseline.package().scheme == hpu::delivery::SchemeKind::bfv ? 7U : 6U;
        require(keys.size() == expected, "frontend duplicated or omitted per-level key resources");
    }
    if (emit_directory) baseline.write(*emit_directory);
}

void exercise_ckks(const std::filesystem::path* emit_directory)
{
    front::CkksContextSpec spec;
    spec.poly_modulus_degree = 128;
    spec.coeff_modulus_bits = {20, 20, 20, 20};
    const auto bundle = front::create_ckks_context(spec);
    const auto& context = *bundle.context;
    seal::KeyGenerator generator(context);
    seal::RelinKeys relin;
    seal::GaloisKeys galois;
    generator.create_relin_keys(relin);
    generator.create_galois_keys(std::vector<std::uint32_t>{
        hpu::scheme::ckks::rotation_galois_element(128, 1),
        hpu::scheme::ckks::conjugation_galois_element(128)}, galois);
    seal::CKKSEncoder encoder(context);
    seal::Plaintext encoded;
    encoder.encode(std::vector<double>{0.25, -0.5, 0.75}, 1048576.0, encoded);
    seal::Ciphertext encrypted;
    seal::Encryptor(context, generator.secret_key()).encrypt_symmetric(encoded, encrypted);
    front::ApplicationBuilder application(context);
    auto evaluator = application.evaluator();
    const auto x = application.import_ciphertext("input/x", encrypted);
    const auto plain = application.import_plaintext("constant/p", encoded);
    front::Ciphertext directly_rescaled;
    evaluator.rescale_to_next(x, directly_rescaled);
    // Imported tensors are read-only too; relinearization must not write
    // its intermediate coefficient representation back into their limbs.
    seal::Ciphertext native_tensor;
    seal::Evaluator(context).square(encrypted, native_tensor);
    const auto imported_tensor = application.import_ciphertext("input/tensor", native_tensor);
    front::Ciphertext imported_relinearized;
    evaluator.relinearize(imported_tensor, relin, imported_relinearized);
    front::Ciphertext row, repeated_row, column, mixed, difference, negated;
    evaluator.rotate_vector(x, 1, galois, row);
    const auto copied_galois = galois;
    evaluator.rotate_vector(x, 1, copied_galois, repeated_row);
    evaluator.complex_conjugate(x, galois, column);
    evaluator.add(row, column, mixed);
    evaluator.sub(mixed, repeated_row, difference);
    evaluator.negate(difference, negated);
    front::Ciphertext added, subtracted, multiplied_plain;
    evaluator.add_plain(x, plain, added);
    evaluator.sub_plain(added, plain, subtracted);
    evaluator.multiply_plain(subtracted, plain, multiplied_plain);
    front::Ciphertext tensor, product, duplicate_product, switched, switched_tensor;
    evaluator.square(x, tensor);
    evaluator.relinearize(tensor, relin, product);
    const auto copied_relin = relin;
    evaluator.relinearize(tensor, copied_relin, duplicate_product);
    evaluator.rescale_to_next(product, switched);
    // Both shapes share the same level's constants/workspace resource.
    evaluator.rescale_to_next(tensor, switched_tensor);
    require(switched_tensor.size() == 3 && switched.size() == 2,
        "frontend tensor rescale lost its component count");
    const auto original_scale = switched;
    evaluator.set_scale(switched, 262144.0);
    require(original_scale.value_id() != switched.value_id(), "set_scale failed to create an SSA view");
    require(original_scale.scale() != switched.scale(), "set_scale changed an alias's metadata");
    front::Ciphertext lower_row, lower_column, lower_tensor, lower_product, output;
    evaluator.rotate_vector(switched, 1, galois, lower_row);
    evaluator.complex_conjugate(switched, galois, lower_column);
    evaluator.multiply(lower_row, lower_column, lower_tensor);
    evaluator.relinearize(lower_tensor, relin, lower_product);
    seal::Plaintext lower_encoded;
    encoder.encode(std::vector<double>{1.0, 1.0, 1.0}, lower_product.parms_id(),
        lower_product.scale(), lower_encoded);
    const auto lower_plain = application.import_plaintext("constant/lower", lower_encoded);
    evaluator.add_plain(lower_product, lower_plain, output);
    application.mark_output("first_branch", row);
    application.mark_output("scale_view", switched);
    application.mark_output("result", output);
    check_compilations(application, output, "ckks_frontend_resources", emit_directory);
}

void exercise_integer(const seal::SEALContext& context, const std::string& stem)
{
    const bool bfv = context.key_context_data()->parms().scheme() == seal::scheme_type::bfv;
    seal::KeyGenerator generator(context);
    seal::RelinKeys relin;
    seal::GaloisKeys galois;
    generator.create_relin_keys(relin);
    std::vector<std::uint32_t> galois_elements{
        hpu::scheme::ckks::rotation_galois_element(128, 1),
        hpu::scheme::ckks::conjugation_galois_element(128)};
    constexpr std::uint32_t pir_expand_element = 129;
    if (bfv) galois_elements.push_back(pir_expand_element);
    generator.create_galois_keys(galois_elements, galois);
    seal::BatchEncoder encoder(context);
    seal::Plaintext encoded;
    std::vector<std::uint64_t> slots(128, 2);
    slots[3] = 3;
    encoder.encode(slots, encoded);
    seal::Ciphertext encrypted;
    seal::Encryptor(context, generator.secret_key()).encrypt_symmetric(encoded, encrypted);
    front::ApplicationBuilder application(context);
    auto evaluator = application.evaluator();
    const auto x = application.import_ciphertext("input/x", encrypted);
    const auto plain = application.import_plaintext("constant/p", encoded);
    front::Ciphertext row, duplicate_row, column, mixed, difference;
    evaluator.rotate_rows(x, 1, galois, row);
    const auto copied_galois = galois;
    evaluator.rotate_rows(x, 1, copied_galois, duplicate_row);
    evaluator.rotate_columns(x, galois, column);
    front::Ciphertext generic_galois;
    if (bfv) evaluator.apply_galois(x, pir_expand_element, galois, generic_galois);
    evaluator.add(row, column, mixed);
    evaluator.sub(mixed, duplicate_row, difference);
    if (bfv) evaluator.negate_inplace(difference);
    front::Ciphertext added, subtracted, multiplied_plain;
    evaluator.add_plain(x, plain, added);
    evaluator.sub_plain(added, plain, subtracted);
    evaluator.multiply_plain(subtracted, plain, multiplied_plain);
    front::Ciphertext tensor, product, duplicate_product, switched;
    evaluator.square(x, tensor);
    evaluator.relinearize(tensor, relin, product);
    const auto copied_relin = relin;
    evaluator.relinearize(tensor, copied_relin, duplicate_product);
    evaluator.mod_switch_to_next(product, switched);
    front::Ciphertext lower_row, lower_column, lower_tensor, lower_product, output;
    evaluator.rotate_rows(switched, 1, galois, lower_row);
    evaluator.rotate_columns(switched, galois, lower_column);
    evaluator.multiply(lower_row, lower_column, lower_tensor);
    evaluator.relinearize(lower_tensor, relin, lower_product);
    evaluator.add_plain(lower_product, plain, output);
    evaluator.sub_plain_inplace(output, plain);
    evaluator.multiply_plain_inplace(output, plain);
    application.mark_output("first_branch", row);
    application.mark_output("result", output);
    check_compilations(application, output, stem);
}
} // namespace

int main(int argc, char** argv)
{
    try {
        std::filesystem::path emit_directory;
        if (argc != 1) {
            if (argc != 3 || std::string(argv[1]) != "--emit-test-package")
                throw std::invalid_argument("usage: hpu_seal_evaluator_coverage_test [--emit-test-package PATH]");
            emit_directory = argv[2];
        }
        exercise_ckks(argc == 1 ? nullptr : &emit_directory);
        front::BfvContextSpec spec;
        spec.poly_modulus_degree = 128;
        spec.coeff_modulus_bits = {20, 20, 20, 20};
        const auto bfv = front::create_bfv_context(spec);
        exercise_integer(*bfv.context, "bfv_frontend_resources");
        seal::EncryptionParameters params(seal::scheme_type::bgv);
        params.set_poly_modulus_degree(128);
        params.set_coeff_modulus({seal::Modulus(2013265921U), seal::Modulus(1811939329U),
            seal::Modulus(469762049U), seal::Modulus(1224736769U)});
        params.set_plain_modulus(65537);
        seal::SEALContext bgv(params, true, seal::sec_level_type::none);
        exercise_integer(bgv, "bgv_frontend_resources");
        std::cout << "Frontend operator/resource coverage PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Frontend operator/resource coverage failed: " << error.what() << '\n';
        return 1;
    }
}
