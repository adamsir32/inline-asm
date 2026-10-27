#include "evaluator_application.hpp"

int main(int argc, char** argv)
{
    try {
        const auto opts = evaluator_example::options(argc, argv);
        hpu::seal_adapter::CkksContextSpec spec;
        spec.poly_modulus_degree = opts.delivery.poly_modulus_degree.value_or(128);
        spec.coeff_modulus_bits = std::vector<int>(opts.data_moduli + 1,
            spec.poly_modulus_degree >= 4096 ? 31 : 20);
        const auto bundle = hpu::seal_adapter::create_ckks_context(spec);
        const auto context_ptr = evaluator_example::fixture_context(bundle.context, opts);
        const auto& context = *context_ptr;
        seal::KeyGenerator keygen(context);
        seal::RelinKeys relin;
        seal::GaloisKeys galois;
        keygen.create_relin_keys(relin);
        keygen.create_galois_keys(std::vector<std::uint32_t>{
            hpu::scheme::ckks::rotation_galois_element(spec.poly_modulus_degree, 1),
            hpu::scheme::ckks::conjugation_galois_element(spec.poly_modulus_degree)}, galois);
        seal::CKKSEncoder encoder(context);
        std::vector<double> input(encoder.slot_count());
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = (i % 3 + 1) * 0.25;
        seal::Plaintext encoded;
        // Match the existing composed example's deployment scale. The larger
        // degree and two Galois key switches need enough precision to keep the
        // maximum error across every slot below the unchanged 1e-2 tolerance.
        encoder.encode(input, std::ldexp(1.0, spec.poly_modulus_degree >= 4096 ? 35 : 20), encoded);
        seal::Ciphertext encrypted;
        seal::Encryptor(context, keygen.secret_key()).encrypt_symmetric(encoded, encrypted);

        hpu::seal_adapter::ApplicationBuilder app(context);
        auto evaluator = app.evaluator();
        const auto x = app.import_ciphertext("input/x", encrypted);
        hpu::seal_adapter::Ciphertext rotated, conjugated, mixed, y;
        evaluator.rotate_vector(x, 1, galois, rotated);
        evaluator.complex_conjugate(x, galois, conjugated);
        evaluator.add(rotated, conjugated, mixed);
        evaluator.multiply(x, mixed, y);
        evaluator.relinearize_inplace(y, relin);
        evaluator.rescale_to_next_inplace(y);
        seal::Plaintext native_bias;
        encoder.encode(std::vector<double>(input.size(), 1.0), y.parms_id(), y.scale(), native_bias);
        evaluator.add_plain_inplace(y, app.import_plaintext("constant/bias", native_bias));
        app.mark_output("y", y);
        const auto compiled = app.compile(evaluator_example::artifact_stem("ckks_evaluator_application", opts), opts.compiler);
        seal::Plaintext decrypted;
        seal::Decryptor(context, keygen.secret_key()).decrypt(compiled.oracle_result(y), decrypted);
        std::vector<double> decoded;
        encoder.decode(decrypted, decoded);
        double error = 0;
        for (std::size_t i = 0; i < input.size(); ++i)
            error = std::max(error, std::abs(decoded[i] - (input[i] * (input[(i + 1) % input.size()] + input[i]) + 1)));
        if (error >= 1e-2) throw std::runtime_error(
            "CKKS evaluator decoded tolerance exceeded: maximum error=" + std::to_string(error));
        evaluator_example::publish(opts, compiled,
            hpu::seal_adapter::ckks_delivery_semantics(decoded, decoded.size(), error, 1e-2));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CKKS evaluator example failed: " << error.what() << '\n';
        return 1;
    }
}
