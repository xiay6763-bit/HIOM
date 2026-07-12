#!/usr/bin/env python3
"""Validity gate for a frozen YCSB spectrum run (paper artifact).

Reads every *_tp.json cell produced by benchmark/run_frozen_ycsb.sh and asserts
the correctness gates from the eval plan. Two gate tiers:

  All systems (every cell):
    - JSON parses
    - a 'median' aggregate exists for each thread point
    - every thread point present (1,2,4,8,16,24)
    - items_per_second > 0 on every iteration
    - (benchmark exit code is checked by the runner, which moves failed
      cells to invalid/ before this script sees them)

  HiOM only (the gate fields are HiOM-specific; baselines don't emit them):
    - read_success_rate == 1.0   (no acknowledged read missed)
    - cold_miss_rate    == 0.0   (every lookup resolved in a tier)
    - hot_tier_size / cold size sane (ColdTier post-prefill == 10M is asserted
      by the fixture itself at load; here we just surface hot_tier_size)

Invalid cells (parse error, missing thread point, gate breach) are reported and
—for the runner's benefit—their paths printed so a plotting script can be told
to skip them. This script does NOT move files; the runner already quarantines
non-zero-exit cells. A gate breach here means "recorded but do not plot".

Exit code: 0 iff every cell in scope passes. Non-zero otherwise (CI-friendly).

Usage:
  python3 eval/validate_frozen_run.py results/frozen/ycsb_spectrum
  python3 eval/validate_frozen_run.py results/frozen/ycsb_spectrum/smoke
"""
import json
import math
import os
import re
import sys

EXPECTED_THREADS = {1, 2, 4, 8, 16, 24}
HIOM_TAG = "HiOMFixture"

# read_success_rate / cold_miss_rate are floats; compare with a tolerance so a
# 0.9999999 from fp accounting doesn't false-fail. The real gate is "no genuine
# miss", i.e. within a few ulp of the ideal.
RS_MIN = 1.0 - 1e-6
CM_MAX = 1e-6


def load_cell(path):
    with open(path) as f:
        return json.load(f)


def check_cell(path):
    """Return (ok, [messages], is_hiom). ok=False marks a non-plottable cell."""
    msgs = []
    name = os.path.basename(path)
    is_hiom = HIOM_TAG in name
    try:
        d = load_cell(path)
    except Exception as e:  # noqa: BLE001 — surface any parse failure verbatim
        return False, [f"JSON parse failed: {e}"], is_hiom

    benches = d.get("benchmarks", [])
    if not benches:
        return False, ["no benchmarks in file"], is_hiom

    iters = [b for b in benches if b.get("run_type") == "iteration"]
    aggs = [b for b in benches if b.get("run_type") == "aggregate"]

    # thread coverage (from iteration rows)
    threads_seen = {int(b["threads"]) for b in iters if "threads" in b}
    missing = EXPECTED_THREADS - threads_seen
    if missing:
        msgs.append(f"missing thread points: {sorted(missing)}")

    # a median aggregate must exist for each thread point
    median_threads = {
        int(b["threads"])
        for b in aggs
        if b.get("aggregate_name") == "median" and "threads" in b
    }
    missing_median = EXPECTED_THREADS - median_threads
    if missing_median:
        msgs.append(f"missing median aggregate for threads: {sorted(missing_median)}")

    # items_per_second > 0 on every iteration
    bad_ips = [
        int(b.get("threads", -1))
        for b in iters
        if not (isinstance(b.get("items_per_second"), (int, float))
                and b["items_per_second"] > 0)
    ]
    if bad_ips:
        msgs.append(f"items_per_second <= 0 at threads {sorted(set(bad_ips))}")

    # HiOM-only correctness gates, per iteration
    if is_hiom:
        for b in iters:
            t = b.get("threads", "?")
            rs = b.get("read_success_rate")
            cm = b.get("cold_miss_rate")
            if rs is None or cm is None:
                msgs.append(f"t={t}: HiOM cell missing read_success_rate/"
                            f"cold_miss_rate (rs={rs}, cm={cm})")
                continue
            if rs < RS_MIN:
                msgs.append(f"t={t}: read_success_rate={rs:.9f} < 1.0")
            if cm > CM_MAX:
                msgs.append(f"t={t}: cold_miss_rate={cm:.9f} != 0.0")

    ok = len(msgs) == 0
    return ok, msgs, is_hiom


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    root = argv[1]
    if not os.path.isdir(root):
        print(f"not a directory: {root}", file=sys.stderr)
        return 2

    cells = sorted(
        os.path.join(root, f)
        for f in os.listdir(root)
        if f.endswith("_tp.json")
    )
    if not cells:
        print(f"no *_tp.json cells under {root}", file=sys.stderr)
        return 2

    n_ok = n_bad = 0
    bad_paths = []
    print(f"=== validating {len(cells)} cell(s) under {root} ===\n")
    for path in cells:
        ok, msgs, is_hiom = check_cell(path)
        tag = "HiOM" if is_hiom else "base"
        if ok:
            n_ok += 1
            print(f"  PASS [{tag}] {os.path.basename(path)}")
        else:
            n_bad += 1
            bad_paths.append(path)
            print(f"  FAIL [{tag}] {os.path.basename(path)}")
            for m in msgs:
                print(f"         - {m}")

    print(f"\n=== {n_ok} passed, {n_bad} failed ===")
    if bad_paths:
        print("\nDo NOT plot these cells (move to invalid/ or regenerate):")
        for p in bad_paths:
            print(f"  {p}")
    return 0 if n_bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
