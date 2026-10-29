# PIR two-stage dynamic handoff: software-model report

## Scope and source

This work starts from `adamsir32/inline-asm` commit
`22c08dcdf08b57fcb6505ff6b7942ffad5f42849` and
`adamsir32/nexus-am` commit `bb1f11cb1436abbc3b27fc4d28792443dfacb809`.
Both checkouts use the independent branch `feature/pir-dynamic-stage-handoff`.
The generated artifacts live on the test server under
`/home/zhaozhengyi/independent-pir-psi-audit-20261026/trident-alignment-20261026/pir-dynamic-acceptance-v2`.
Previously delivered packages were not overwritten.

## A. Final stage0 HPU memory

`ApplicationBuilder::compile` runs a BFV functional `BfvSoftwareExecutor`,
then `finalize_frontend_package` invokes `compile_application_resources`.
That function relocates the resource plan and executes the **final encoded**
`program.instructions` with its `ordered_dma_spans` and relocated initial
`HpuMemImage`. `execute_instructions` copies initial DDR words into its model;
each DLOAD reads that copy and each DSTORE writes it. It returns the final
`InstructionExecutionResult.words`, compares all output limbs with the SEAL
Golden, and previously discarded the words.

The new default-off `CompileOptions::capture_execution_memory` retains those
exact returned words. `CompiledApplication::execution_memory_words()` exposes
them read-only and throws unless capture was requested. An optional output
pointer through `finalize_frontend_package` and `compile_application_resources`
passes the words up after successful encoded-instruction verification. The
`--stage0-executed-image PATH` application option writes the complete final
DDR as little-endian 32-bit words and immediately feeds that file to the
existing `--stage0-readback` importer. The initial package image remains intact.
No arithmetic, ISA, DMA, resource allocation or package schema changed.

The Python validation entry compares all 16 stage0 output limbs with package
Golden **only as a correctness check**. It verifies the final image is
`used_lines * 256` bytes, and compares bytes outside output limbs against the
initial image. In the observed run, 5,200,636 non-output bytes changed, which
rules out a Golden-only output overlay. The provenance file binds the final
image to the package JSON and initial image with SHA-256. The exported image
comes from the `InstructionExecutionResult.words` of the same compile.

## B. CPU readback and decomposition

`import_stage0_readback` traverses the package's verified output metadata,
checks object/component/modulus coverage, lengths, duplicate limbs and modulus
values, and reads every physical output at `line_offset * 256`. Each Q limb is
converted separately with `natural[BR(j)] = physical[j]`. The importer fills
new SEAL ciphertext storage from file words. For the same query, key and
parameters, the application compares **every ciphertext coefficient** with
the independent SEAL result. It then runs `decompose_to_plaintexts` along both
paths and compares all 32 plaintexts, their lengths, order and every
coefficient. The observed two-input run has two stage0 readback and replay
passes. It never reconstructs final DDR from Golden or a SEAL ciphertext.

The validation entry rejected truncated, corrupted, missing-output, wrong-bit
order and wrong-run images. It also mutates the in-memory output metadata of
both compiled stage0 packages and proves that a duplicate limb, missing limb,
wrong modulus and missing output object are rejected. The standalone binary
image has no metadata fields, so these four faults are injected at the
metadata interface within the same C++ process.

## C. Dynamic stage1 input and fixed instruction replay

`CompiledApplication::plaintext_allocation_ids()` exposes the compiler's
logical plaintext to physical Q-limb allocation binding. This is a read-only
mapping populated when the plaintext is prepared; it is needed because the
final relocated image exposes physical names while the CPU decomposition
produces logical `database/output_k_term_j` values. The stage1 baseline has
32 dynamic plaintexts × 3 Q limbs = 96 physical input allocations, plus six
fixed plaintext allocations. Each limb is 4,096 little-endian words. The
application calls the existing `bfv_multiply_plaintext_to_hpu` bridge with
the fixed encrypted query's BFV level, checks that **every baseline encoded
word** equals the compiler's original input image, and checks each replacement
residue is below its Q modulus.

The application writes the 96 changed limb files and `dynamic_inputs.tsv`.
The new `stage1_patch.py materialize` action uses the source package to fill
the remaining six fixed plaintext limbs, generates a complete versioned
manifest with package and per-file SHA-256, then calls the existing `apply`
validator. That validator checks every allocation, physical length, Q bound,
input hash and source package hash, and writes a new image. The C++ application
loads **that exact output file** into an equal-address `HpuMemImage` and passes
it to `execute_instructions` with the original `program.instructions` and
`ordered_dma_spans` from the retained, once-compiled stage1 object.

For each replacement input, a separate native `seal::Evaluator` calculation
expands the fixed second-dimension encrypted query, multiplies by the new
plaintext matrix, adds terms and switches modulus. It never builds or compiles
an HPU graph. Every stage1 output object, component, Q modulus and coefficient
from the replayed DDR is checked against that new native reference. The CPU
then decrypts, recursively composes and extracts the target record; all 1,024
bytes are compared with the independent deterministic database record oracle.

The experiment first replays the baseline decomposed input through the patch
tool, then changes the database seed and executes stage0 again with the **same
first- and second-dimension encrypted queries and keys**. This changes the
stage0 intermediate ciphertext and all 393,216 physical stage1 input
coefficients. Both runs use the identical retained `CompiledApplication`
stage1 object and execute its 26,700 encoded instructions. The baseline patch
is byte-equal to the compiler image; the changed patch differs only inside
the 96 dynamic allocations. Both final records and the full stage1 ciphertext
comparison pass. A complete query change, including a new second-dimension
encrypted query, is outside this fixed-query experiment; the encrypted query
would need a separately validated dynamic input patch.

## Reproduction

From the producer repository on the test server, after building
`hpu_bfv_sealpir_2d_concentrated_example`:

```sh
python3 test/pir_dynamic_stage_handoff_test.py \
  --example ../build/hpu_bfv_sealpir_2d_concentrated_example \
  --stage1-patch-script ../../nexus-am-pir-psi/tests/hputest/scripts/stage1_patch.py \
  --output ../pir-dynamic-validation
```

Use a fresh output path for another run. The command saves both packages,
stage0 final DDR, complete physical patch inputs and manifests, patched and
executed stage1 images, final records, logs, five negative readback logs, and
`handoff_summary.json` with package, image and `.inst32` SHA-256 values.
`replay_summary.json` records instruction count and changed input coefficients.

## Evidence and limits

The observed `handoff_summary.json` reports stage0 `used_lines=30701`,
16 output limbs, `stage1_inst32_sha256=
d541de297369100e44c725468753d6148454b6e74cfa4979e582882056fe60e1`,
96 dynamic limbs and `two_input_numeric_replay=PASS`.
This proves SEAL reference agreement, functional software verification during
compile, encoded-instruction model replay on a patched image, and package
structure. It does **not** prove RTL timing, DMA implementation in silicon,
interrupt delivery, real DDR readback, or physical HPU execution. Nexus-AM's
three fixed-input targets remain separate from this CPU handoff experiment.
