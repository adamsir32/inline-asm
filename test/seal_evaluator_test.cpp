#include "hpu/seal/evaluator.hpp"
#include "hpu/seal/bfv_context.hpp"
#include "hpu/seal/ckks_context.hpp"
#include "scheme/bfv/galois.hpp"
#include "scheme/ckks/galois.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using hpu::seal_adapter::ApplicationBuilder;
using hpu::seal_adapter::Ciphertext;
using hpu::seal_adapter::CompileOptions;

void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

template<class Callable> void rejects(Callable call, const char* message)
{
    try { call(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

bool identical(const seal::Ciphertext& a, const seal::Ciphertext& b)
{
    const auto count = a.size() * a.poly_modulus_degree() * a.coeff_modulus_size();
    return a.parms_id() == b.parms_id() && a.size() == b.size() && a.scale() == b.scale() &&
        a.correction_factor() == b.correction_factor() && a.is_ntt_form() == b.is_ntt_form() &&
        std::equal(a.data(), a.data() + count, b.data());
}

std::shared_ptr<seal::SEALContext> make_context(seal::scheme_type scheme)
{
    if (scheme == seal::scheme_type::ckks) {
        hpu::seal_adapter::CkksContextSpec spec;
        spec.poly_modulus_degree = 128; spec.coeff_modulus_bits = {20,20,20,20};
        return hpu::seal_adapter::create_ckks_context(spec).context;
    }
    if (scheme == seal::scheme_type::bfv) {
        hpu::seal_adapter::BfvContextSpec spec;
        spec.poly_modulus_degree = 128; spec.coeff_modulus_bits = {20,20,20,20};
        return hpu::seal_adapter::create_bfv_context(spec).context;
    }
    seal::EncryptionParameters parameters(scheme);
    parameters.set_poly_modulus_degree(128);
    parameters.set_coeff_modulus({seal::Modulus(2013265921U), seal::Modulus(1811939329U),
        seal::Modulus(469762049U), seal::Modulus(1224736769U)});
    parameters.set_plain_modulus(65537);
    return std::make_shared<seal::SEALContext>(parameters, true, seal::sec_level_type::none);
}

void test_scheme(seal::scheme_type scheme, const char* package_directory = nullptr)
{
    const auto context = make_context(scheme);
    seal::KeyGenerator keygen(*context);
    seal::RelinKeys relin;
    seal::GaloisKeys galois;
    keygen.create_relin_keys(relin);
    keygen.create_galois_keys(std::vector<std::uint32_t>{
        hpu::scheme::bfv::row_rotation_galois_element(128, 1),
        hpu::scheme::bfv::column_rotation_galois_element(128)}, galois);
    seal::Plaintext encoded, native_plain;
    if (scheme == seal::scheme_type::ckks) {
        seal::CKKSEncoder encoder(*context);
        encoder.encode(std::vector<double>{0.25,0.5,0.75}, std::ldexp(1.0,20), encoded);
        encoder.encode(std::vector<double>{1.0,1.0,1.0}, std::ldexp(1.0,20), native_plain);
    } else {
        seal::BatchEncoder encoder(*context);
        encoder.encode(std::vector<std::uint64_t>(128,2), encoded);
        encoder.encode(std::vector<std::uint64_t>(128,3), native_plain);
    }
    seal::Ciphertext native_input;
    seal::Encryptor(*context, keygen.secret_key()).encrypt_symmetric(encoded, native_input);
    const auto saved_input = native_input;
    const auto saved_plain = native_plain;
    seal::Evaluator oracle(*context);

    ApplicationBuilder app(*context);
    auto evaluator = app.evaluator();
    auto x = app.import_ciphertext("input/x", native_input);
    const auto saved_handle = x;
    const auto p = app.import_plaintext("plain/p", native_plain);
    // Mutating caller-owned buffers must not mutate the recorded graph.
    native_input.data()[0] ^= 1;
    native_plain.data()[0] ^= 1;
    evaluator.add_inplace(x, x);
    Ciphertext branch, joined, added, subtracted, multiplied;
    evaluator.sub(x, saved_handle, branch);
    evaluator.add(branch, saved_handle, joined);
    evaluator.add_plain(joined, p, added);
    evaluator.sub_plain(added, p, subtracted);
    evaluator.multiply_plain(subtracted, p, multiplied);
    app.mark_output("branch", branch);
    app.mark_output("branch_alias", branch);
    app.mark_output("result", multiplied);
    seal::Ciphertext host_doubled, host_branch, host_joined, host_added, host_subtracted, host_multiplied;
    oracle.add(saved_input, saved_input, host_doubled);
    oracle.sub(host_doubled, saved_input, host_branch);
    oracle.add(host_branch, saved_input, host_joined);
    oracle.add_plain(host_joined, saved_plain, host_added);
    oracle.sub_plain(host_added, saved_plain, host_subtracted);
    oracle.multiply_plain(host_subtracted, saved_plain, host_multiplied);
    CompileOptions no_opt; no_opt.optimize_resources = false; no_opt.require_instruction_verification = false;
    const auto baseline = app.compile("frontend_baseline", no_opt);
    CompileOptions diagnostic; diagnostic.require_instruction_verification = false;
    const auto optimized = app.compile("frontend_optimized", diagnostic);
    require(baseline.verified() && optimized.verified(), "functional verification did not run");
    require(identical(optimized.oracle_result(saved_handle), saved_input), "inplace changed aliased input");
    require(identical(optimized.oracle_result(multiplied), host_multiplied), "frontend expression differs from independent oracle");
    require(baseline.package().outputs.size() == optimized.package().outputs.size(), "optimization lost golden results");
    require(optimized.package().outputs.size() == 2 * host_branch.size() * host_branch.coeff_modulus_size(),
        "marked output aliases or unmarked nodes produced extra deployment goldens");
    require(optimized.package().operation_graph_json.find("\"exported\":false") != std::string::npos,
        "logical intermediate nodes disappeared from graph metadata");
    for (std::size_t i = 0; i < baseline.package().outputs.size(); ++i)
        require(baseline.package().outputs[i].golden_words == optimized.package().outputs[i].golden_words,
                "optimization changed node values");
    const auto frozen_words = optimized.package().initial_image->words();
    const auto frozen_graph = optimized.package().operation_graph_json;
    Ciphertext later;
    evaluator.add(x, saved_handle, later);
    require(optimized.package().initial_image->words() == frozen_words &&
        optimized.package().operation_graph_json == frozen_graph, "compiled application changed after builder mutation");
    rejects([&] { optimized.oracle_result(later); }, "compiled snapshot accepted future graph value");

    ApplicationBuilder foreign(*context);
    const auto other = foreign.import_ciphertext("input/other", saved_input);
    rejects([&] { evaluator.add(x, other, later); }, "cross-builder handles accepted");
    rejects([&] { app.import_ciphertext("input/x", saved_input); }, "duplicate import id accepted");
    rejects([&] { evaluator.add(Ciphertext{}, x, later); }, "empty handle accepted");
    rejects([&] { evaluator.relinearize(x, relin, later); }, "two-component relinearization accepted");

    ApplicationBuilder nonlinear(*context);
    auto operations = nonlinear.evaluator();
    const auto a = nonlinear.import_ciphertext("input/a", saved_input);
    Ciphertext square, product, relined, rotated, flipped, changed;
    operations.square(a, square);
    operations.multiply(a, a, product);
    operations.relinearize(square, relin, relined);
    if (scheme == seal::scheme_type::ckks) {
        operations.rotate_vector(a, 1, galois, rotated);
        operations.complex_conjugate(a, galois, flipped);
        operations.rescale_to_next(relined, changed);
        Ciphertext three_component;
        operations.rescale_to_next(product, three_component);
        operations.negate(a, later);
        auto view = changed;
        const auto old_scale = view.scale();
        operations.set_scale(view, old_scale * 1.001);
        require(changed.scale() == old_scale && view.scale() != old_scale, "scale view mutated source");
        nonlinear.mark_output("view", view);
        rejects([&] { operations.mod_switch_to_next(a, later); }, "unsupported CKKS modswitch accepted");
        rejects([&] { operations.set_scale(view, 0); }, "invalid scale accepted");
    } else {
        operations.rotate_rows(a, 1, galois, rotated);
        operations.rotate_columns(a, galois, flipped);
        operations.mod_switch_to_next(relined, changed);
        rejects([&] { operations.set_scale(changed, 2); }, "integer scale mutation accepted");
        rejects([&] { operations.rescale_to_next(a, later); }, "integer rescale accepted");
        if (scheme == seal::scheme_type::bfv) operations.negate(a, later);
        else rejects([&] { operations.negate(a, later); }, "unsupported BGV negate accepted");
    }
    nonlinear.mark_output("changed", changed);
    // Key data is also captured by the graph before caller mutation.
    relin = seal::RelinKeys{};
    galois = seal::GaloisKeys{};
    const auto full = nonlinear.compile("frontend_nonlinear", diagnostic);
    require(full.verified(), "nonlinear graph software verification failed");
    require(full.package().outputs.size() == changed.size() * context->get_context_data(changed.parms_id())->parms().coeff_modulus().size(),
        "unmarked nonlinear nodes or duplicate metadata views were exported");
    require(full.instruction_verified(), "BR coefficient nonlinear graph instruction verification failed");
    require(full.oracle_result(square).size() == 3 && full.oracle_result(relined).size() == 2,
        "tensor/relinearization shapes were lost");
    const auto repeat = nonlinear.compile("frontend_nonlinear", diagnostic);
    require(full.package().program.instructions.size() == repeat.package().program.instructions.size() &&
        full.package().initial_image->words() == repeat.package().initial_image->words(), "compile is not deterministic");
    for (std::size_t i = 0; i < full.package().program.instructions.size(); ++i)
        require(full.package().program.instructions[i].word == repeat.package().program.instructions[i].word,
            "instruction scheduling is not deterministic");
    {
        CompileOptions strict_nonlinear;
        const auto strict_result = nonlinear.compile("frontend_nonlinear", strict_nonlinear);
        require(strict_result.instruction_verified(), "strict nonlinear graph verification failed");
        CompileOptions natural = diagnostic;
        natural.instruction_ntt_contract = hpu::model::InstructionNttContract::natural_coefficient_boundaries;
        const auto raw = nonlinear.compile("frontend_nonlinear", natural);
        require(!raw.instruction_verified(), "natural replay silently accepted explicit BR coefficient boundaries");
        require(raw.package().oracle_report_json.find("\"ntt_contract\":\"natural_coefficient_boundaries\"")
                    != std::string::npos
                && raw.package().oracle_report_json.find("\"hardware_contract_verified\":false")
                    != std::string::npos,
                "raw instruction result lost its contract or claimed hardware verification");
        require(raw.package().initial_image->words() == full.package().initial_image->words()
                && raw.package().program.instructions.size() == full.package().program.instructions.size(),
                "selecting a host NTT contract changed the deployment program");
        for (std::size_t i = 0; i < full.package().program.instructions.size(); ++i)
            require(raw.package().program.instructions[i].word == full.package().program.instructions[i].word,
                    "selecting a host NTT contract rewrote an instruction word");
    }
    if (package_directory) full.write(package_directory);

    // A pointwise graph provides a strict encoded-instruction verification gate.
    ApplicationBuilder pointwise(*context);
    auto pointwise_evaluator = pointwise.evaluator();
    const auto input = pointwise.import_ciphertext("input/p", saved_input);
    Ciphertext sum;
    pointwise_evaluator.add(input, input, sum);
    // With no marks, export the last computation even if another input was
    // imported later. A read-only input is never an implicit output root.
    pointwise.import_ciphertext("input/late", saved_input);
    CompileOptions strict; strict.require_instruction_verification = true;
    const auto checked = pointwise.compile("frontend_strict", strict);
    require(checked.instruction_verified(), "encoded pointwise oracle comparison was not verified");
    require(checked.package().outputs.size() == sum.size() * context->get_context_data(sum.parms_id())->parms().coeff_modulus().size(),
        "implicit output did not select the last computation");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc <= 2, "usage: hpu_seal_evaluator_test [PACKAGE_DIRECTORY]");
        for (auto scheme : {seal::scheme_type::ckks, seal::scheme_type::bfv, seal::scheme_type::bgv})
            test_scheme(scheme, scheme == seal::scheme_type::ckks && argc == 2 ? argv[1] : nullptr);
        std::cout << "Three-scheme evaluator graph tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Evaluator test failed: " << error.what() << '\n';
        return 1;
    }
}
