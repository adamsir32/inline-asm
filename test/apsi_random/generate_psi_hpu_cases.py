#!/usr/bin/env python3
"""Generate medium-profile custom-input cases with genuine test_oprf tokens."""

import argparse
import json
import random
from pathlib import Path

from psi_cpu_model import evaluate, locations, token


def pick_with_hash_bias(rng, amount, allowed_locations, excluded):
    chosen = []
    used = set(excluded)
    while len(chosen) < amount:
        candidate = rng.randrange(1, 1 << 48)
        if candidate in used:
            continue
        if locations(token(candidate))[0] not in allowed_locations:
            continue
        used.add(candidate)
        chosen.append(candidate)
    return chosen


def make_case(index, base_seed):
    seed = base_seed + index
    rng = random.Random(seed)
    overlap_options = [0, 1, 32, 256, 500]
    overlap = overlap_options[index % len(overlap_options)]
    if index % 5 == 4:
        sender = pick_with_hash_bias(rng, 8192, set(range(128)), set())
        mode = "sender_hash0_first128"
    else:
        sender = rng.sample(range(1, 1 << 48), 8192)
        mode = "uniform"
    receiver = rng.sample(sender, overlap)
    if index % 5 == 3:
        receiver.extend(pick_with_hash_bias(rng, 512 - overlap,
                                            set(range(300)), set(sender) | set(receiver)))
        mode += "+receiver_hash0_first300"
    else:
        used = set(sender) | set(receiver)
        while len(receiver) < 512:
            candidate = rng.randrange(1, 1 << 48)
            if candidate not in used:
                used.add(candidate)
                receiver.append(candidate)
    rng.shuffle(receiver)
    result = evaluate(sender, receiver)
    return seed, mode, sender, receiver, result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--count", type=int, default=20)
    parser.add_argument("--base-seed", type=int, default=91827000)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    manifest = []
    for index in range(args.count):
        seed, mode, sender, receiver, result = make_case(index, args.base_seed)
        path = args.out_dir / f"case_{index:02d}.input"
        with path.open("w", encoding="ascii") as stream:
            stream.write(f"# medium profile; test_oprf(item) genuine; seed={seed} mode={mode}\n")
            for item in sender:
                low, high = token(item)
                stream.write(f"S {item} {low} {high}\n")
            for item in receiver:
                low, high = token(item)
                stream.write(f"R {item} {low} {high}\n")
        manifest.append({"case": index, "seed": seed, "mode": mode,
                         "sender_size": len(sender), "receiver_size": len(receiver),
                         "expected_intersection": result["expected"],
                         "cpu_recovered": result["recovered"],
                         "cpu_correct": result["correct"],
                         "max_bin_load": result["max_bin_load"],
                         "max_cuckoo_chain": result["max_cuckoo_chain"],
                         "file": path.name})
    (args.out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2),
                                                   encoding="utf-8")
    print(json.dumps({"cases": len(manifest), "cpu_correct": sum(x["cpu_correct"] for x in manifest),
                      "max_bin_load": max(x["max_bin_load"] for x in manifest),
                      "max_cuckoo_chain": max(x["max_cuckoo_chain"] for x in manifest),
                      "modes": sorted(set(x["mode"] for x in manifest))}, indent=2))


if __name__ == "__main__":
    main()
