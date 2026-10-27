# Exact PSI regression inputs

Each input line has `S` or `R`, the original unsigned 64-bit item, and the low
and high unsigned 64-bit words of its 128-bit test token. The application
computes the expected answer from the original item sets, then independently
decrypts the HPU graph result and requires equality. These inputs never replace
the decoded HPU result with the CPU answer.

| Input | Profile | Expected intersection | Purpose |
| --- | --- | --- | --- |
| `truncation.input` | small | empty | Sender and receiver tokens share low bits but differ above the old encoding cutoff. |
| `cross_felt.input` | small | empty | Separate sender tokens supplied the old per-felt zeroes. |
| `real_input.input` | small | empty | Five genuine `test_oprf` sender tokens formerly matched one nonmember. |
| `real_input_medium.input` | medium | empty | Same genuine counterexample embedded in 8,192/512 items, seed 57192344. |
| `capacity_125.input` | medium | one item | An occupied bin at the configured 125-item degree limit. |
| `capacity_126.input` | medium | one item | A 126th item requires a second sender partition. |

The two `real_input*` tests require that every injected token equal the
application's deterministic `test_oprf` output. The arbitrary-token tests do
not impose that check. These are application workload fixtures, not FourQ OPRF
or Kuku hashing reference vectors. The original adversarial search and
generator evidence is in the independent phase-2 validation report outside
this repository.
