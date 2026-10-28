#!/usr/bin/env python3
"""Compare actual fixed-Trident SenderDB/BinBundle caches with HPU preparation.

BinBundle order at one bundle index is semantically irrelevant in APSI, so
instances are compared as a multiset of full per-slot root/coeff signatures.
"""

import argparse
import json
import re
import subprocess
from collections import defaultdict
from pathlib import Path


def parse_dump(path: Path):
    tokens, bins, bundles, powers, matches = {}, defaultdict(dict), {}, {}, []
    meta = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if not fields:
            continue
        kind = fields[0]
        if kind == "META":
            meta = dict(part.split("=", 1) for part in fields[1:] if "=" in part)
        elif kind == "TOKEN":
            first, second = fields.index("felts"), fields.index("locations")
            placed = fields.index("placed") if "placed" in fields else len(fields)
            key = (fields[1], int(fields[2]))
            tokens[key] = (
                tuple(map(int, fields[first + 1:second])),
                tuple(sorted(map(int, fields[second + 1:placed]))),
                int(fields[placed + 1]) if placed < len(fields) else None,
            )
        elif kind == "BIN":
            key = (int(fields[1]), int(fields[2]))
            slot = int(fields[3])
            split = fields.index("coefficients")
            value = (
                tuple(sorted(map(int, fields[5:split]))),
                tuple(map(int, fields[split + 1:])),
            )
            if slot in bins[key]:
                raise AssertionError(f"duplicate {key} bin slot {slot} in {path}")
            bins[key][slot] = value
        elif kind == "BUNDLE":
            key = (int(fields[1]), int(fields[2]))
            bundles[key] = (int(fields[4]), int(fields[6]), int(fields[8]))
        elif kind == "POWER":
            node = tuple(map(int, fields[1:]))
            powers[node[0]] = node[1:]
        elif kind == "MATCH":
            matches.append(int(fields[1]))
        else:
            raise AssertionError(f"unknown audit row {kind} in {path}")
    if set(bins) - set(bundles):
        raise AssertionError(f"unowned bins in {path}")
    normalized = defaultdict(list)
    for key, (degree, nonempty, slots) in bundles.items():
        contents = bins[key]
        if len(contents) != nonempty:
            raise AssertionError(f"wrong nonempty count for {key} in {path}")
        if max((len(roots) for roots, _ in contents.values()), default=0) != degree:
            raise AssertionError(f"wrong polynomial degree for {key} in {path}")
        if max(contents, default=-1) >= slots:
            raise AssertionError(f"out-of-range slot for {key} in {path}")
        normalized[key[1]].append((degree, tuple(sorted(contents.items()))))
    return meta, tokens, {
        bundle: tuple(sorted(instances)) for bundle, instances in normalized.items()
    }, powers, tuple(sorted(matches)), bundles


def run(command, log):
    done = subprocess.run(command, capture_output=True, text=True)
    log.write_text(done.stdout + done.stderr, encoding="utf-8")
    if done.returncode:
        raise RuntimeError(f"exit {done.returncode}: {command}; see {log}")
    return done


def audit_values(output, marker):
    match = re.search(rf"^{marker}([^\n]*)$", output, re.MULTILINE)
    if match is None:
        raise AssertionError(f"missing {marker}")
    return tuple(sorted(map(int, match.group(1).split())))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--params", required=True)
    parser.add_argument("--example", required=True)
    parser.add_argument("--case-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--full", nargs="*", default=[])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    requested_full = set(args.full)
    summary = []
    for raw in sorted(args.case_dir.glob("*.raw")):
        name = raw.stem
        fixture = args.output / f"{name}.input"
        native_dump = args.output / f"{name}.native"
        hpu_dump = args.output / f"{name}.hpu"
        record = {"case": name, "status": "FAIL"}
        try:
            run([args.native, args.params, str(raw), str(fixture), str(native_dump)],
                args.output / f"{name}.native.log")
            run([args.example, "--profile", "medium", "--match-mode", "trident",
                 "--test-input", str(fixture), "--dump-preprocessing", str(hpu_dump),
                 "--preprocess-only"], args.output / f"{name}.preprocess.log")
            n_meta, n_tokens, n_bundles, n_powers, n_matches, n_details = parse_dump(native_dump)
            h_meta, h_tokens, h_bundles, h_powers, h_matches, h_details = parse_dump(hpu_dump)
            for key in ("sender", "receiver", "table"):
                if n_meta[key] != h_meta[key]:
                    raise AssertionError(f"{key}: native={n_meta[key]} HPU={h_meta[key]}")
            # Trident has 4095 item slots; HPU's SEAL batch contains one unused
            # extra slot. The HPU dump checks that its polynomial is one there.
            if int(n_meta["bins_per_bundle"]) != 4095 or int(h_meta["bins_per_bundle"]) != 4096:
                raise AssertionError("unexpected native/HPU batch-slot layout")
            if n_meta["plain_modulus"] != "188417" or n_meta["bits_per_felt"] != "17":
                raise AssertionError("native PSI parameters differ from the fixed Trident JSON")
            if n_tokens != h_tokens:
                raise AssertionError("token fields, Kuku candidates, or receiver placement differ")
            if n_bundles != h_bundles:
                raise AssertionError("SenderDB BinBundle root or polynomial cache differs")
            if n_powers != h_powers:
                raise AssertionError("PowersDag topology differs")
            if n_matches != h_matches:
                raise AssertionError("native polynomial zero test and HPU preparation differ")
            record.update({
                "status": "PASS", "sender": len([x for x in n_tokens if x[0] == "S"]),
                "receiver": len([x for x in n_tokens if x[0] == "R"]),
                "bin_bundles": len(n_details),
                "max_degree": max((value[0] for value in n_details.values()), default=0),
                "native_matches": list(n_matches),
            })
            if name in requested_full:
                full = run([args.example, "--profile", "medium", "--match-mode", "trident",
                            "--test-input", str(fixture)], args.output / f"{name}.full.log")
                reference = audit_values(full.stderr, "AUDIT_APSI_REFERENCE")
                recovered = audit_values(full.stderr, "AUDIT_RECOVERED")
                if reference != n_matches or recovered != n_matches:
                    raise AssertionError("HPU decrypted result differs from native cache zero test")
                if "seal_oracle=PASS hpu_software=PASS instruction_model=PASS" not in full.stderr:
                    raise AssertionError("HPU software verification missing")
                record["full_hpu_verified"] = True
        except Exception as error:
            record["status"] = "FAIL"
            record["error"] = str(error)
        summary.append(record)
        print(f"{name}: {record['status']} {record.get('error', '')}", flush=True)
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n",
                                                encoding="utf-8")
    if any(record["status"] != "PASS" for record in summary):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
