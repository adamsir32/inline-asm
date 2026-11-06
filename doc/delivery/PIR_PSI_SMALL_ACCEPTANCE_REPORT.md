# PIR/PSI Small complete software acceptance

## Scope and source

- Producer baseline `8c66404bfa765cbea94339fbfd720f718572904b`; tested code `de9fdfbbdb092933ed4f315da8094a0fb68a2546`.
- Nexus-AM baseline `9cd7a544bcc17cddcc374e3fb7e2f18f3ae167fd`; tested code `688a16b442f051ce9ff00e34a4f21443a9e3aa9b`.
- Server `zhaozhengyi@10.102.104.22:9322`; fresh evidence `/home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a`.
- Overall Small software acceptance: **PASS**. Target-level simulation, RTL and physical HPU: **NOT RUN**.
- Both source trees were clean before and after the run. New build/output directories, exact HEAD checks, CMake source and embedded commit checks, and per-package provenance checks passed.

## Parameters and validation levels

- PIR: N=4096, 128 records × 1024 bytes, index 8, database seed `6000273874561356338`. Actual stage0 encoded execution changed 5200636 non-Golden bytes; 16 output limbs and 96 physical stage1 limbs checked. Two databases replayed without recompiling stage1: **PASS**.
- PIR CPU decomposition: 32 x 4096 coefficients exact; encoding: 96 x 4096 words exact. Independent record oracle, SEAL coefficient comparison, HPU software and encoded instruction model: **PASS**.
- PSI Small: N=8192, plaintext modulus 65537, 8 felts/item, 3 hash functions, table size 4096. Sender=10, Receiver=5. OPRF values are deterministic test fixtures; network FourQ OPRF is not embedded.
- PSI application algorithm, per-node SEAL oracle, HPU software execution, encoded instruction model, decryption and CPU APSI-reference intersection: **PASS** in all four scenarios.
- Nexus-AM four ELF/BIN builds and static checks: **PASS**. Real target C main and native image/check code ran with fresh model DDR in host software for PIR and PSI: **PASS**.
- PSI target C checks image preparation, HPU issue/completion, Golden output words and read-only guards. Client decryption/intersection recovery is in the producer application.

| PSI scenario | Recovered intersection | Instructions | DMA | SEAL | HPU software | Encoded model |
|---|---|---:|---:|---|---|---|
| empty | `[]` | 2891 | 1161 | PASS | PASS | PASS |
| single | `[1]` | 2891 | 1161 | PASS | PASS | PASS |
| multi | `[1, 4]` | 2891 | 1161 | PASS | PASS | PASS |
| boundary | `[2003199, 2004587]` | 2891 | 1161 | PASS | PASS | PASS |

## Application packages

Every package passed the upstream validator and full-commit application-level provenance check. Size is the sum of regular files.

| Case | Fresh package | Bytes |
|---|---|---:|
| pir | `pir/packages/stage0_concentrated_000` | 17700898 |
| pir | `pir/packages/stage1_concentrated_000` | 26492779 |
| pir | `pir/replay/changed-stage0-packages/stage0_concentrated_000` | 17700898 |
| empty | `psi/empty/hpu_query` | 8538618 |
| single | `psi/single/hpu_query` | 8534139 |
| multi | `psi/multi/hpu_query` | 8538621 |
| boundary | `psi/boundary/hpu_query` | 8534153 |

PIR stage0: 17,740 encoded instructions / 6,434 DMA spans. PIR stage1: 26,700 / 9,642. The unified ELF embeds both exact streams. PSI single: 2,891 / 1,161.

## Target ELF/BIN

| Case | ELF bytes | ELF SHA-256 | BIN bytes | BIN SHA-256 |
|---|---:|---|---:|---|
| 08_01_psi_single | 6299880 | `4159b347d773f99b6282ce4b152e099f88a3fe453097213ed838744f4db8be91` | 6227256 | `2eb7cdabad9d3c227e83da3393f30a216457caea07847fc53721f1d8f45a799d` |
| 08_02_pir_stage0 | 8609152 | `e82da5bd3eaeb24dbc7d6e5f0fb095112f72c60695c4a66086da2f5dfeb164b0` | 8283400 | `f7768d157323f126209c53ca3f1f4dbf612a02e148b96e179889489f7f6a94a0` |
| 08_03_pir_stage1 | 12891840 | `81c863a593f6975f4338f01dd26f7b8e8fac134b1b2a21d1be58169fd1c6ae22` | 12412104 | `ebf227a7e878e0cae6d0774eb5cd010c400b350e6da1ec9cf6f2e5bb85251eb0` |
| 08_04_pir_unified | 21526632 | `c14ddd06087b65e1ac2f4a27c2a5cc1fd6d1d0a3dd483972d9ea72bb03f859c9` | 23159856 | `b1c61fd52dae6c2653676e15291eae2543d7800cd42760965760ae54d97619b3` |

## Negative and boundary checks

- PIR stage0 readback/metadata: corrupted-output, missing-output, truncated, wrong-bit-order, wrong-run, missing-dynamic-limb, noncanonical-dynamic-limb, duplicate-output-metadata, missing-output-metadata, wrong-modulus-metadata, missing-output-object-metadata: **PASS**, all rejected.
- PIR CPU layout/encoding: duplicate-output-limb, overlapping-output-limb, short-output-window, noncanonical-stage0-output, noncanonical-plaintext, duplicate-input-limb, out-of-window-input-limb: **PASS**, all rejected.
- PSI package corruption and dynamic input format: missing file, ABI, instruction, DMA, window, Golden, image and input map: **8/8 PASS**, all rejected.
- PSI empty intersection, single, multi, boundary, duplicate raw item and invalid/empty set: **PASS**.
- PSI target C host faults: output corruption, read-only memory corruption, missing completion: **3/3 PASS**, all rejected. PIR target C faults: changed dynamic input and output corruption: **2/2 PASS**.
- Invalid PIR record index, invalid database seed syntax and invalid PSI scenario: **3/3 PASS**, expected return code 1 with diagnostic. A 1 ms acceptance-runner budget rejected both app processes as TIMEOUT (**2/2 PASS**). This tests runner timeout enforcement, not HPU hardware timing.
- Producer related CTest: **22/22 PASS**, including ambiguous physical mapping and release provenance refusal.

## Exact commands, return codes and logs

The runner recorded all 21 steps. Nonzero codes for malformed inputs and TIMEOUT for budget checks are expected rejection results. `summary.json` is the machine-readable record.

1. **configure** — exit `0`; log `logs/configure.log`.

   ```sh
   cmake -S /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-producer -B /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DHPU_ENABLE_SEAL_INTEGRATION=ON -DHPU_ENABLE_TRIDENT_PSI=ON -DHPU_SEAL_SOURCE_DIR=/home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-producer/third_party/modified-SEAL -DHPU_TEST_CONFIG=/home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-producer/config/fhe_test.conf -DCMAKE_PREFIX_PATH=/home/zhaozhengyi/independent-pir-psi-audit-20261026/trident-faithful-reference/reference-prefix -DFETCHCONTENT_SOURCE_DIR_MSGSL=/home/zhaozhengyi/independent-pir-psi-audit-20261026/deps/GSL-152d6eb989a1ecd23fe9c9cfb2fb8cfc7c0cd0c1 -DFETCHCONTENT_SOURCE_DIR_ZLIB=/home/zhaozhengyi/independent-pir-psi-audit-20261026/deps/zlib-da607da739fa6047df13e66a2af6b8bec7c2a498 -DFETCHCONTENT_SOURCE_DIR_ZSTD=/home/zhaozhengyi/independent-pir-psi-audit-20261026/deps/zstd-f8745da6ff1ad1e7bab384bd1f9d742439278e99
   ```

2. **build** — exit `0`; log `logs/build.log`.

   ```sh
   cmake --build /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build -j 4
   ```

3. **producer_related_ctest** — exit `0`; log `logs/producer_related_ctest.log`.

   ```sh
   ctest --test-dir /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build --output-on-failure -R 'hpu_bfv_sealpir_2d_concentrated_small_record_(0|8|9|64|127)$|bfv_sealpir_2d_concentrated_small_delivery_test$|hpu_bfv_sealpir_stage0_readback_adapter$|hpu_bfv_apsi_concentrated_small_(empty|single|multi|boundary)$|bfv_apsi_concentrated_small_delivery_test$|hpu_bfv_apsi_trident_small_(multi|truncation|cross_felt)$|hpu_bfv_apsi_trident_native_cross_felt$|hpu_bfv_apsi_trident_duplicate_input$|hpu_bfv_apsi_senderdb_driver_failures$|bfv_sealpir_2d_concentrated_seed_manifest_test$|pir_physical_binding_test$|verified_pir_psi_delivery_guard_test$|^application_package_test$'
   ```

4. **pir_dynamic** — exit `0`; log `logs/pir_dynamic.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-producer/test/pir_dynamic_stage_handoff_test.py --example /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_bfv_sealpir_2d_concentrated_example --stage1-patch-script /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus/tests/hputest/scripts/stage1_patch.py --output /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir
   ```

5. **psi_empty** — exit `0`; log `logs/psi_empty.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-producer/test/apsi_concentrated_delivery_test.py --example /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_bfv_apsi_concentrated_example --validator /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package --output /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/empty --profile small --scenario empty
   ```

6. **psi_single** — exit `0`; log `logs/psi_single.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-producer/test/apsi_concentrated_delivery_test.py --example /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_bfv_apsi_concentrated_example --validator /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package --output /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/single --profile small --scenario single --executed-image /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/single-executed.u32.bin
   ```

7. **psi_multi** — exit `0`; log `logs/psi_multi.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-producer/test/apsi_concentrated_delivery_test.py --example /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_bfv_apsi_concentrated_example --validator /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package --output /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/multi --profile small --scenario multi
   ```

8. **psi_boundary** — exit `0`; log `logs/psi_boundary.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-producer/test/apsi_concentrated_delivery_test.py --example /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_bfv_apsi_concentrated_example --validator /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package --output /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/boundary --profile small --scenario boundary
   ```

9. **build_08_01_psi_single** — exit `0`; log `logs/build_08_01_psi_single.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus/tests/hputest/scripts/build-pir-psi.py --producer-commit de9fdfbbdb092933ed4f315da8094a0fb68a2546 --package-validator /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package --output /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/targets --am-home /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus --cross /opt/riscv/bin/riscv64-unknown-elf- --case 08_01_psi_single --package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/single/hpu_query
   ```

10. **build_08_02_pir_stage0** — exit `0`; log `logs/build_08_02_pir_stage0.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus/tests/hputest/scripts/build-pir-psi.py --producer-commit de9fdfbbdb092933ed4f315da8094a0fb68a2546 --package-validator /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package --output /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/targets --am-home /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus --cross /opt/riscv/bin/riscv64-unknown-elf- --case 08_02_pir_stage0 --package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir/packages/stage0_concentrated_000
   ```

11. **build_08_03_pir_stage1** — exit `0`; log `logs/build_08_03_pir_stage1.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus/tests/hputest/scripts/build-pir-psi.py --producer-commit de9fdfbbdb092933ed4f315da8094a0fb68a2546 --package-validator /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package --output /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/targets --am-home /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus --cross /opt/riscv/bin/riscv64-unknown-elf- --case 08_03_pir_stage1 --package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir/packages/stage1_concentrated_000
   ```

12. **build_08_04_pir_unified** — exit `0`; log `logs/build_08_04_pir_unified.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus/tests/hputest/scripts/build-pir-psi.py --producer-commit de9fdfbbdb092933ed4f315da8094a0fb68a2546 --package-validator /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package --output /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/targets --am-home /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus --cross /opt/riscv/bin/riscv64-unknown-elf- --case 08_04_pir_unified --stage0-package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir/packages/stage0_concentrated_000 --stage1-package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir/packages/stage1_concentrated_000 --cpu-parameters /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir/replay/cpu-fixture/parameters.json --dynamic-input-map /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir/replay/baseline/dynamic/dynamic_inputs.tsv
   ```

13. **pir_cpu_encoded** — exit `0`; log `logs/pir_cpu_encoded.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus/tests/hputest/scripts/test_pir_cpu_algorithms.py --fixture /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir --output /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir-cpu --dynamic-input-map /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir/replay/baseline/dynamic/dynamic_inputs.tsv --replay-executable /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/pir_cpu_encoded_replay
   ```

14. **pir_target_main_host** — exit `0`; log `logs/pir_target_main_host.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus/tests/hputest/scripts/test_pir_unified_main_integration.py --delivery /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/targets/imported/08_04_pir_unified --fixture /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir
   ```

15. **psi_target_main_host** — exit `0`; log `logs/psi_target_main_host.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus/tests/hputest/scripts/test_psi_single_main_integration.py --delivery /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/targets/imported/08_01_psi_single --executed-image /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/single-executed.u32.bin
   ```

16. **package_and_dynamic_negatives** — exit `0`; log `logs/package_and_dynamic_negatives.log`.

   ```sh
   /home/zhaozhengyi/miniconda3/bin/python3 /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-nexus/tests/hputest/scripts/test-pir-psi-negative.py --psi-package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/single/hpu_query --stage1-package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir/packages/stage1_concentrated_000 --validator /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package
   ```

17. **pir_bad_query** — exit `1`; log `logs/pir_bad_query.log`.

   ```sh
   /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_bfv_sealpir_2d_concentrated_example --profile small --record-index 128
   ```

18. **pir_bad_database_seed** — exit `1`; log `logs/pir_bad_database_seed.log`.

   ```sh
   /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_bfv_sealpir_2d_concentrated_example --profile small --record-index 8 --test-database-seed 123x
   ```

19. **psi_bad_scenario** — exit `1`; log `logs/psi_bad_scenario.log`.

   ```sh
   /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_bfv_apsi_concentrated_example --profile small --scenario invalid
   ```

20. **pir_execution_timeout** — exit `TIMEOUT`; log `logs/pir_execution_timeout.log`.

   ```sh
   /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_bfv_sealpir_2d_concentrated_example --profile small --record-index 8
   ```

21. **psi_execution_timeout** — exit `TIMEOUT`; log `logs/psi_execution_timeout.log`.

   ```sh
   /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_bfv_apsi_concentrated_example --profile small --scenario single
   ```

## Additional package validator commands

The runner also called the validator separately on each freshly generated package; all seven subprocesses returned 0 and their provenance checks passed.

```sh
/home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir/packages/stage0_concentrated_000  # exit 0
/home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir/packages/stage1_concentrated_000  # exit 0
/home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/pir/replay/changed-stage0-packages/stage0_concentrated_000  # exit 0
/home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/empty/hpu_query  # exit 0
/home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/single/hpu_query  # exit 0
/home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/multi/hpu_query  # exit 0
/home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/build/hpu_validate_package /home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/boundary/hpu_query  # exit 0
```
## Per-file Git diff review

| File | Reason and effect |
|---|---|
| `doc/delivery/PIR_PSI_SMALL_TEST_GAP_ANALYSIS.md` | File-by-file gap analysis made before implementation. |
| `doc/delivery/PIR_PSI_SMALL_ACCEPTANCE_REPORT.md` | This report records server commands, return codes, artifacts and limitations. |
| `examples/bfv_apsi_concentrated_application.cpp` | Optional Small executed DDR export via existing public encoded instruction executor, with complete Golden comparison. PSI graph and matching semantics unchanged. |
| `test/apsi_concentrated_delivery_test.py` | Optional executed-image argument, verifies the requested image exists. |
| `tests/hputest/scripts/test_psi_single_main_integration.py` | Runs existing 08_01 target C, image.c and check.c with model DDR and three faults. |
| `tests/hputest/scripts/run-pir-psi-small-acceptance.py` | Clean cross-repository build, provenance, packages, four targets, CPU linkage, negatives, logs and hashes. |

On the tested producer commit, `git diff --exit-code cfee951e4056660e521d55a3535d82335a49bb08 HEAD -- include/ src/` and `git diff --exit-code 8c66404bfa765cbea94339fbfd720f718572904b HEAD -- encode/` both returned 0. No existing Nexus C source or native runtime was modified.

## Artifacts and limitations

- Fresh artifacts and logs: `/home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a`. `summary.json` lists commands and hashes; `checksums.sha256` covers 910 retained application, CPU, log and target artifact files; `sha256sum -c` verified 910/910 (exit 0), see `checksums-verify.log`.
- PSI actual encoded execution DDR: `/home/zhaozhengyi/independent-pir-psi-audit-20261026/small-acceptance-run-de9f-688a/psi/single-executed.u32.bin`. Its content is not reconstructed from Golden.
- Full network APSI/FourQ OPRF, target-level simulation, RTL and physical HPU: **NOT RUN**. There is no hardware timing claim.
- No unresolved Small software failure was observed. Final PSI intersection recovery remains a producer/client operation; target C checks encrypted output and memory guards.

## Audit-fixes parameter clarification (2026-11-05)

This historical report records the Small acceptance baseline. The follow-up
`feature/pir-psi-audit-fixes` work uses the same dimensions. PIR has N=4096,
128 real records of 1024 bytes, 9 records per plaintext, 15 populated
plaintexts plus one zero padding plaintext in a 4 x 4 matrix (16 total).
Stage 0 has 17,740 HPU instructions; Stage 1 has 26,700. The 32 CPU
decomposition plaintexts become 96 three-modulus physical inputs.

PSI Small has N=8192, 10 real Sender items and 5 real Receiver items in
each of empty/single/multi/boundary. The real intersections have 0/1/2/2
items respectively. Kuku has 4096 buckets; one HPU package covers a
contiguous 1024-bucket window. The fixed source config sets
`max_items_per_bin=16`; the independent PDF's value of 2 is inconsistent
with the checked source. These counts describe effective sets, not any
synthetic padding or formal-scale dataset. Target multi-window scheduling
(F-04) and generalized PIR CPU parameters (F-05) remain outside Small scope.
