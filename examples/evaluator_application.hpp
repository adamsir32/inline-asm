#pragma once

#include "delivery_options.hpp"
#include "hpu/seal/application_delivery.hpp"
#include "hpu/seal/bfv_context.hpp"
#include "hpu/seal/ckks_context.hpp"
#include "hpu/seal/evaluator.hpp"
#include "scheme/bfv/galois.hpp"
#include "scheme/ckks/galois.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

// Shared fixture setup only. The application graph is supplied by each .cpp.
namespace evaluator_example {

struct Options {
    DeliveryOptions delivery;
    hpu::seal_adapter::CompileOptions compiler;
};

inline Options options(int argc, char** argv)
{
    Options result;
    std::vector<char*> remaining{argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--no-opt") result.compiler.optimize_resources = false;
        else if (arg == "--require-instruction-verification") result.compiler.require_instruction_verification = true;
        else if (arg == "--allow-unverified-instruction-model") result.compiler.require_instruction_verification = false;
        else if (arg == "--instruction-ntt-contract") {
            if (++i >= argc) throw std::invalid_argument("--instruction-ntt-contract requires a value");
            const std::string contract(argv[i]);
            if (contract == "legacy-raw-stage")
                result.compiler.instruction_ntt_contract = hpu::model::InstructionNttContract::legacy_raw_stage;
            else if (contract == "natural-coefficient-boundaries")
                result.compiler.instruction_ntt_contract =
                    hpu::model::InstructionNttContract::natural_coefficient_boundaries;
            else throw std::invalid_argument("unknown instruction NTT contract: " + contract);
        }
        else remaining.push_back(argv[i]);
    }
    result.delivery = parse_delivery_options(static_cast<int>(remaining.size()), remaining.data(), "--print-asm");
    return result;
}

inline void publish(const Options& options, const hpu::seal_adapter::CompiledApplication& compiled,
                    hpu::delivery::SemanticReport semantics)
{
    auto package = compiled.package();
    package.semantic_report = std::move(semantics);
    if (options.delivery.emit_directory)
        hpu::delivery::write_application_package(*options.delivery.emit_directory, package);
    std::cout << "Functional verification: PASS\nGraph nodes: " << compiled.node_count()
        << "\nEncoded instructions: " << package.program.instructions.size()
        << "\nDMA count: " << package.program.ordered_dma_spans.size()
        << "\nHPU_MEM lines: " << package.initial_image->used_lines()
        << "\nHost instruction comparison: " << (compiled.instruction_verified() ? "PASS" : "NOT VERIFIED")
        << "\nHost NTT contract: " << hpu::model::instruction_ntt_contract_name(options.compiler.instruction_ntt_contract)
        << "\nHardware contract verification: NOT VERIFIED"
        << "\nResource and verification details: oracle/report.json\n";
    if (options.delivery.print_program) std::cout << package.program.assembly;
}

inline std::shared_ptr<seal::SEALContext> integer_context(seal::scheme_type scheme, std::size_t degree)
{
    if (scheme == seal::scheme_type::bfv) {
        hpu::seal_adapter::BfvContextSpec spec;
        spec.poly_modulus_degree = degree;
        spec.coeff_modulus_bits = degree >= 4096 ? std::vector<int>{31,31,31,31}
                                               : std::vector<int>{20,20,20,20};
        spec.plain_modulus_bits = degree >= 4096 ? 20 : 17;
        return hpu::seal_adapter::create_bfv_context(spec).context;
    }
    seal::EncryptionParameters parameters(scheme);
    parameters.set_poly_modulus_degree(degree);
    parameters.set_coeff_modulus({seal::Modulus(2013265921U), seal::Modulus(1811939329U),
        seal::Modulus(469762049U), seal::Modulus(1224736769U)});
    parameters.set_plain_modulus(degree > 32768 ? 786433 : 65537);
    auto context = std::make_shared<seal::SEALContext>(parameters, true, seal::sec_level_type::none);
    if (!context->parameters_set()) throw std::runtime_error("invalid evaluator example context");
    return context;
}

template<class BuildGraph>
int integer_main(seal::scheme_type scheme, const char* stem_base, int argc, char** argv,
                 BuildGraph build_graph)
{
    try {
        const auto opts = options(argc, argv);
        const auto degree = opts.delivery.poly_modulus_degree.value_or(128);
        const auto context = integer_context(scheme, degree);
        seal::KeyGenerator keygen(*context);
        seal::RelinKeys relin;
        seal::GaloisKeys galois;
        keygen.create_relin_keys(relin);
        keygen.create_galois_keys(std::vector<std::uint32_t>{
            hpu::scheme::bfv::row_rotation_galois_element(degree, 1),
            hpu::scheme::bfv::column_rotation_galois_element(degree)}, galois);
        seal::BatchEncoder encoder(*context);
        std::vector<std::uint64_t> input(degree), expected(degree);
        for (std::size_t i = 0; i < degree; ++i) input[i] = i % 3 + 1;
        const auto half = degree / 2;
        for (std::size_t i = 0; i < degree; ++i)
            expected[i] = input[i] * (input[i / half * half + (i % half + 1) % half]
                                  + input[(i + half) % degree]) + 7;
        seal::Plaintext encoded, native_bias;
        encoder.encode(input, encoded);
        encoder.encode(std::vector<std::uint64_t>(degree, 7), native_bias);
        seal::Ciphertext encrypted;
        seal::Encryptor(*context, keygen.secret_key()).encrypt_symmetric(encoded, encrypted);
        hpu::seal_adapter::ApplicationBuilder app(*context);
        auto evaluator = app.evaluator();
        const auto x = app.import_ciphertext("input/x", encrypted);
        const auto bias = app.import_plaintext("constant/bias", native_bias);
        const auto y = build_graph(app, evaluator, x, bias, relin, galois);
        app.mark_output("y", y);
        const auto compiled = app.compile(delivery_artifact_stem(stem_base, opts.delivery), opts.compiler);
        seal::Plaintext decrypted;
        seal::Decryptor(*context, keygen.secret_key()).decrypt(compiled.oracle_result(y), decrypted);
        std::vector<std::uint64_t> decoded;
        encoder.decode(decrypted, decoded);
        if (decoded != expected) throw std::runtime_error("evaluator example decoded result differs from expression");
        publish(opts, compiled, hpu::seal_adapter::integer_delivery_semantics(decoded));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Evaluator example failed: " << error.what() << '\n';
        return 1;
    }
}

} // namespace evaluator_example
