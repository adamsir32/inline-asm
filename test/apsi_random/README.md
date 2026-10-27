# Medium PSI random differential suite

Run `cmake --build <build> --target bfv_apsi_exact_random_differential`.
The test target generates 20 medium-profile cases from seeds 91827000–91827019,
then runs the HPU application on each with `--test-require-fixture-tokens`.
Case logs and `summary.json` are written under the build directory.

The modes cycle through intersection sizes 0, 1, 32, 256 and 500. Two modes
concentrate sender or receiver candidate positions. `psi_cpu_model.py` models
the old five-felt approximate recovery so its `correct` field is diagnostic;
the pass condition uses the independent original-item set intersection,
the application's decrypted result, and SEAL/HPU/instruction verification.
The deterministic token and position functions are this application's test
fixture and do not claim FourQ or Kuku equivalence.
