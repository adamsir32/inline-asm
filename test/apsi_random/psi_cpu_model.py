#!/usr/bin/env python3
"""Independent CPU oracle and faithful APSI-layout model for c98b8ef.

The reference answer uses Python set intersection, not the polynomial model.
All random choices are seeded and all counterexamples are serialized as JSON.
"""

import argparse
import json
import random
from collections import Counter, defaultdict
from pathlib import Path

MASK = (1 << 64) - 1
KEY = 0x4150534948505531
CONST = 0xD6E8FEB86659FD93


def mix(x):
    x = (x + 0x9E3779B97F4A7C15) & MASK
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & MASK
    return x ^ (x >> 31)


def unxorshift_right(x, shift):
    value = x
    distance = shift
    while distance < 64:
        value ^= value >> distance
        distance *= 2
    return value & MASK


def unmix(value):
    value = unxorshift_right(value, 31)
    value = (value * pow(0x94D049BB133111EB, -1, 1 << 64)) & MASK
    value = unxorshift_right(value, 27)
    value = (value * pow(0xBF58476D1CE4E5B9, -1, 1 << 64)) & MASK
    value = unxorshift_right(value, 30)
    return (value - 0x9E3779B97F4A7C15) & MASK


def token(item):
    raw_low = mix(item ^ KEY)
    high = mix(raw_low ^ ((item << 1) & MASK) ^ CONST)
    return (raw_low or 1, high)


def as_int(pair):
    return pair[0] | (pair[1] << 64)


def as_pair(value):
    return (value & MASK, (value >> 64) & MASK)


def felts(pair, count=5, bits=18):
    number = as_int(pair)
    mask = (1 << bits) - 1
    return tuple((number >> (i * bits)) & mask for i in range(count))


def locations(pair, table=1638, hashes=3):
    low, high = pair
    result = []
    for function in range(hashes):
        mixed = mix(low ^ mix((high + 0x9E3779B97F4A7C15 * (function + 1)) & MASK))
        location = mixed % table
        if location not in result:
            result.append(location)
    return result


def layout(items, item_tokens, table=1638, hashes=3):
    slots = [None] * table
    positions = {}
    maximum_chain = 0
    for item in items:
        pending = item
        choice = 0
        for attempt in range(500):
            choices = locations(item_tokens[pending], table, hashes)
            location = choices[choice % len(choices)]
            if slots[location] is None:
                slots[location] = pending
                maximum_chain = max(maximum_chain, attempt + 1)
                break
            slots[location], pending = pending, slots[location]
            displaced = locations(item_tokens[pending], table, hashes)
            choice = (displaced.index(location) + 1) % len(displaced)
        else:
            raise ValueError("cuckoo_eviction_exhausted")
    for location, item in enumerate(slots):
        if item is not None:
            positions[item] = location
    if len(positions) != len(items):
        raise ValueError("cuckoo_lost_input")
    return slots, positions, maximum_chain


def evaluate(sender, receiver, overrides=None, table=1638, hashes=3,
             count=5, bits=18, bin_limit=125):
    if len(set(sender)) != len(sender) or len(set(receiver)) != len(receiver):
        raise ValueError("duplicate_items")
    overrides = overrides or {}
    all_items = set(sender) | set(receiver)
    item_tokens = {item: overrides.get(item, token(item)) for item in all_items}
    slots, positions, chain = layout(receiver, item_tokens, table, hashes)
    roots = defaultdict(lambda: [set() for _ in range(count)])
    bin_counts = Counter()
    for item in sender:
        parts = felts(item_tokens[item], count, bits)
        for location in locations(item_tokens[item], table, hashes):
            bin_counts[location] += 1
            if bin_counts[location] > bin_limit:
                raise ValueError("sender_bin_capacity_exceeded")
            for index, value in enumerate(parts):
                roots[location][index].add(value)
    recovered = []
    detail = []
    for item in receiver:
        location = positions[item]
        parts = felts(item_tokens[item], count, bits)
        verdicts = [parts[index] in roots[location][index] for index in range(count)]
        if all(verdicts):
            recovered.append(item)
        detail.append({"item": item, "location": location,
                       "felts": list(parts), "zero_by_felt": verdicts})
    expected = sorted(set(sender) & set(receiver))
    return {"expected": expected, "recovered": sorted(recovered),
            "correct": expected == sorted(recovered),
            "max_bin_load": max(bin_counts.values(), default=0),
            "max_cuckoo_chain": chain, "occupied_bins": len(bin_counts),
            "receiver_detail": detail}


def find_truncation(rng):
    sender = as_pair(rng.getrandbits(128))
    target = set(locations(sender))
    low90 = as_int(sender) & ((1 << 90) - 1)
    for attempt in range(1, 100000):
        candidate = as_pair(low90 | (rng.getrandbits(38) << 90))
        if candidate != sender and locations(candidate)[0] in target:
            return sender, candidate, attempt
    raise RuntimeError("truncation search exhausted")


def find_cross_felt(rng):
    first = as_pair(rng.getrandbits(128))
    first_places = set(locations(first))
    for sender_attempt in range(1, 100000):
        second = as_pair(rng.getrandbits(128))
        common = first_places & set(locations(second))
        if common:
            chosen = min(common)
            break
    else:
        raise RuntimeError("sender co-bin search exhausted")
    a = felts(first)
    b = felts(second)
    mixed = (a[0], b[1], a[2], b[3], a[4])
    low90 = sum(value << (18 * index) for index, value in enumerate(mixed))
    for receiver_attempt in range(1, 100000):
        candidate = as_pair(low90 | (rng.getrandbits(38) << 90))
        if candidate != first and candidate != second and locations(candidate)[0] == chosen:
            return first, second, candidate, chosen, sender_attempt, receiver_attempt
    raise RuntimeError("receiver co-bin search exhausted")


def make_examples(seed):
    rng = random.Random(seed)
    first, receiver, steps = find_truncation(rng)
    truncation = evaluate([1], [3], {1: first, 3: receiver})
    a, b, c, location, sender_steps, receiver_steps = find_cross_felt(rng)
    cross = evaluate([1, 2], [3], {1: a, 2: b, 3: c})
    special0 = unmix(0) ^ KEY
    special1 = unmix(1) ^ KEY
    special_tokens = [token(special0), token(special1)]
    return {
        "seed": seed, "profile": {"table": 1638, "hashes": 3,
                               "felts": 5, "bits_per_felt": 18, "t": 270337},
        "truncation": {"sender_items": [1], "receiver_items": [3],
                       "overrides": {"1": list(first), "3": list(receiver)},
                       "search_attempts": steps,
                       "sender_locations": locations(first),
                       "receiver_locations": locations(receiver),
                       "sender_felts": felts(first), "receiver_felts": felts(receiver),
                       "model": truncation},
        "cross_felt": {"sender_items": [1, 2], "receiver_items": [3],
                       "overrides": {"1": list(a), "2": list(b), "3": list(c)},
                       "shared_location": location,
                       "search_attempts": {"sender": sender_steps, "receiver": receiver_steps},
                       "sender_locations": [locations(a), locations(b)],
                       "receiver_locations": locations(c),
                       "sender_felts": [felts(a), felts(b)], "receiver_felts": felts(c),
                       "model": cross},
        "fixture_low_collision_exception": {
            "items": [special0, special1], "tokens": special_tokens,
            "felt_equal": felts(special_tokens[0]) == felts(special_tokens[1]),
            "low_equal": special_tokens[0][0] == special_tokens[1][0],
            "locations": [locations(t) for t in special_tokens]},
    }


def random_suite(seed, trials):
    rng = random.Random(seed)
    classes = Counter()
    mismatches = []
    maxima = {"bin_load": 0, "cuckoo_chain": 0}
    counts = Counter()
    samples = []
    for index in range(trials):
        sender_size = rng.choice([0, 1, 10, 32, 100, 300, 700, 1200])
        receiver_size = rng.choice([0, 1, 5, 20, 60, 100, 300])
        overlap_limit = min(sender_size, receiver_size)
        overlap = rng.choice([0, overlap_limit // 2, overlap_limit])
        universe = 1 << 32
        sender = rng.sample(range(universe), sender_size)
        receiver = rng.sample(sender, overlap)
        sender_set = set(sender)
        while len(receiver) < receiver_size:
            candidate = rng.randrange(universe)
            if candidate not in sender_set and candidate not in receiver:
                receiver.append(candidate)
        rng.shuffle(receiver)
        try:
            result = evaluate(sender, receiver)
            category = "correct" if result["correct"] else "valid_input_wrong_result"
            maxima["bin_load"] = max(maxima["bin_load"], result["max_bin_load"])
            maxima["cuckoo_chain"] = max(maxima["cuckoo_chain"], result["max_cuckoo_chain"])
            if not result["correct"]:
                mismatches.append({"trial": index, "sender": sender,
                                   "receiver": receiver, "result": result})
        except ValueError as error:
            category = "valid_input_capacity" if str(error) in (
                "cuckoo_eviction_exhausted", "sender_bin_capacity_exceeded") else "invalid_input"
            if len(samples) < 10:
                samples.append({"trial": index, "class": category, "reason": str(error),
                                "sender_size": sender_size, "receiver_size": receiver_size})
        classes[category] += 1
        counts[f"sender_{sender_size}"] += 1
        counts[f"receiver_{receiver_size}"] += 1
    return {"seed": seed, "trials": trials, "classes": classes,
            "sizes": counts, "maxima": maxima, "mismatches": mismatches,
            "exception_samples": samples}


def capacity_experiment(seed):
    rng = random.Random(seed)
    selected = []
    while len(selected) < 126:
        item = rng.randrange(1, 1 << 48)
        if item not in selected and locations(token(item))[0] == 0:
            selected.append(item)
    valid = evaluate(selected[:125], [selected[0]])
    try:
        evaluate(selected, [selected[0]])
        over_capacity = "unexpectedly_accepted"
    except ValueError as error:
        over_capacity = str(error)
    return {"seed": seed, "target_bin": 0, "sender_125": {
                "correct": valid["correct"], "max_bin_load": valid["max_bin_load"],
                "max_cuckoo_chain": valid["max_cuckoo_chain"]},
            "sender_126": over_capacity,
            "items": selected}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, default=20261026)
    parser.add_argument("--trials", type=int, default=1500)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    examples = make_examples(args.seed)
    randomized = random_suite(args.seed ^ 0x5A5A, args.trials)
    capacity = capacity_experiment(args.seed ^ 0xABCD)
    result = {"token_examples": examples, "randomized": randomized,
              "capacity": capacity}
    args.out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"truncation": examples["truncation"]["model"]["correct"],
                      "cross_felt": examples["cross_felt"]["model"]["correct"],
                      "fixture_exception": examples["fixture_low_collision_exception"],
                      "random_classes": randomized["classes"],
                      "maxima": randomized["maxima"],
                      "capacity_125": capacity["sender_125"],
                      "capacity_126": capacity["sender_126"]}, indent=2))


if __name__ == "__main__":
    main()
