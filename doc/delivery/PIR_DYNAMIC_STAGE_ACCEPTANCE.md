# PIR dynamic stage handoff: acceptance, 2026-10-29

## Revisions and evidence location

- Producer branch: `feature/pir-dynamic-stage-handoff`, fixed base
  `22c08dcdf08b57fcb6505ff6b7942ffad5f42849`; implementation commits
  `0f53a743df231b0fb54f71c3b149d06163c99afe` and
  `436e83d9480cd9561bd9518edb7a850977fa7b53`.
- Nexus-AM branch: `feature/pir-dynamic-stage-handoff`, fixed base
  `bb1f11cb1436abbc3b27fc4d28792443dfacb809`; patch tool commit
  `ce1d4ffd48f4c14d55aca008c845ac4c6a8a43c3`.
- Server root: `/home/zhaozhengyi/independent-pir-psi-audit-20261026`.
  Producer checkout: `$A/trident-alignment-20261026/repo`, build:
  `$A/trident-alignment-20261026/build`, Nexus-AM checkout:
  `$A/nexus-am-pir-psi`. `$A` denotes the server root in the commands below.
- Final evidence: `$A/trident-alignment-20261026/pir-dynamic-acceptance-v2`
  (`handoff_summary.json`, `main.log`, two packages, final DDR images,
  plaintext patch files, patch manifests, replay summaries and records).
  Aggregate command log: `$A/trident-alignment-20261026/pir-dynamic-acceptance-v2.log`.
  `handoff_summary.json` SHA-256:
  `15cee2ec90a60cb9322685189c8be57a059e149f4410983987043a85317688e4`.

No merge or push was made to the delivered branches or either upstream.

## Results

| Check | Actual command / evidence | Exit | Status |
| --- | --- | ---: | --- |
| Full real two-stage handoff and two fixed-instruction replays | From producer checkout: `PYTHONDONTWRITEBYTECODE=1 python3 test/pir_dynamic_stage_handoff_test.py --example ../build/hpu_bfv_sealpir_2d_concentrated_example --stage1-patch-script ../../nexus-am-pir-psi/tests/hputest/scripts/stage1_patch.py --output ../pir-dynamic-acceptance-v2` | 0 | **PASS** |
| Stage0 actual final DDR export | Final image has 30,701 × 256 bytes, 16 complete output limbs. `stage0-executed.u32.bin` SHA-256 `b7d940244270adbda45733bf90694e09753012a8c7b2a3b1ce6a7c957aee53a9`. 5,200,636 non-output bytes differ from the initial image. | 0 | **PASS** |
| Real readback, bit order and decomposition | Both stage0 runs use their own execution words, verify every SEAL ciphertext coefficient, and compare all coefficients of all 32 decomposed plaintexts with the same-run native SEAL reference. See `main.log` and `handoff_summary.json`. | 0 | **PASS** |
| Physical input encoding and patch | All 96 dynamic limbs match compiler-produced baseline encoding word for word. `stage1_patch.py materialize` retains six fixed plaintext limbs and validates package, Q bounds and SHA-256. Baseline patched image equals source image; changed image differs only in dynamic spans. | 0 | **PASS** |
| Fixed instruction numeric replay | Baseline and changed input use the same retained compiled stage1 object, original DMA spans and 26,700 encoded instructions. Changed input modifies 393,216 coefficients. Every replay output coefficient equals a **new** native SEAL calculation. Both recursive decryptions recover the independent expected 1,024-byte record. | 0 | **PASS** |
| Same `.inst32` | SHA-256 `d541de297369100e44c725468753d6148454b6e74cfa4979e582882056fe60e1` for both replays. Baseline/changed patched image hashes are `d7ce4d4fb9af69726f03a5f04d9edeb0061e2de6d55d97e90b413a9169d81caa` / `a7f0012090a0e9d0c7d7354fb2511effbfc96daebb422b55315ff70f9a7a4675`. | 0 | **PASS** |
| Abnormal stage0 and stage1 input | Five file cases: truncation, output corruption, wrong bit order, missing output, wrong-run image. Four same-run metadata cases: duplicate limb, missing limb, wrong modulus, missing output object. Two patch cases: missing dynamic limb and noncanonical residue. Each rejected; logs at evidence root `negative-*.log`, metadata markers in `main.log`. | 0 | **PASS** |
| New stage0/stage1 package format | `../build/hpu_validate_package ../pir-dynamic-acceptance-v2/packages/stage0_concentrated_000` and same for stage1; logs `pir-dynamic-validate-stage{0,1}-v2.log`. Final packages share the unchanged v1 schema. | 0 / 0 | **PASS** |
| Existing PIR/PSI regression | The scoped `ctest` command below: 18/18. Log `$A/trident-alignment-20261026/pir-dynamic-ctest.log`. | 0 | **PASS** |
| Existing Nexus-AM negative delivery cases | `test-pir-psi-negative.py` against prior fixed packages; all eight corrupt cases rejected. Log `$A/trident-alignment-20261026/pir-dynamic-nexus-negative.log`. | 0 | **PASS** |
| Nexus-AM PSI, PIR stage0, PIR stage1 ELF/BIN | `build-pir-psi.py` for cases `08_01_psi_single`, `08_02_pir_stage0`, `08_03_pir_stage1`, using the fixed prior source packages and fresh output `$A/pir-dynamic-nexus-regression`. Logs `$A/pir-dynamic-nexus-{psi,stage0,stage1}.log`. All receipts report `elf_instruction_words_verified=true`. | 0 / 0 / 0 | **PASS** |
| Complete 109-case Nexus-AM suite | Existing case catalog unchanged; not required for this scoped software handoff. | — | **NOT RUN** |
| RTL and physical HPU execution | No RTL/target DDR readback, interrupt timing or device execution performed. | — | **NOT RUN** |

Scoped regression command, from the producer checkout:

```sh
ctest --test-dir ../build --output-on-failure -R 'hpu_bfv_sealpir_2d_concentrated_small_record_(0|8|9|64|127)$|bfv_sealpir_2d_concentrated_small_delivery_test$|hpu_bfv_sealpir_stage0_readback_adapter$|hpu_bfv_apsi_concentrated_small_(empty|single|multi|boundary)$|bfv_apsi_concentrated_small_delivery_test$|hpu_bfv_apsi_trident_small_(multi|truncation|cross_felt)$|hpu_bfv_apsi_trident_native_cross_felt$|hpu_bfv_apsi_trident_duplicate_input$|hpu_bfv_apsi_senderdb_driver_failures$'
```

The new producer `execution_memory_words()` capture flag and plaintext binding
getter are read-only observation interfaces. `compile_application_resources`
retains the already executed final memory only when asked. The model arithmetic,
DMA semantics, ISA, resource schedule and package wire format were not changed.
Nexus-AM adds only the checked `materialize` action to the patch tool.

This establishes the software data contract needed to design a unified PIR CPU
program. A target implementation still needs CPU-side decomposition/encoding
on its runtime, dynamic query ciphertext patching for a changed second-dimension
query, HPU/SoC RTL execution, real DDR readback, interrupt integration and
physical-hardware validation.
