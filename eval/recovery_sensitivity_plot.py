#!/usr/bin/env python3
"""C3 recovery sensitivity: recovery time vs tail size (O(tail) verification).

Reads results/recovery/tail_sweep.csv + tail_sweep_meta.json (produced by
`hiom_recovery_bm --tail-sweep[-prefill] <N>`), takes the per-(threads, tail)
median over reps, and emits:
  - eval/charts/recovery_tail_scan.pdf    figure 1: pure O(tail), threads=1
  - eval/charts/recovery_vs_baseline.pdf  figure 2: total open vs Viper, t=32
  - results/recovery/summary.txt          slope + speedup + crossover + cadence

Conventions / caveats (see design/HIOM.md):
  - Figure 1 uses threads=1 so wall-clock is genuinely proportional to scan
    blocks (at t=32 the tail-scan parallelism itself varies with tail —
    num_threads = min(num_blocks, 32) — so small tails would flatten).
  - x-axis is the MEASURED recovery_replayed, not the target.
  - Per-entry tail-scan cost is dominated by ColdTier upsert PMem random
    access (~tens of microseconds at >=10M scale; the prefill log shows the
    same ~50 us/op), NOT a bug. The O(tail) TREND is the point.
  - tail=0 lands just below the write frontier (replays ~0) and is the origin
    anchor; larger tails are anchored at the real data top (probe_data_top
    walks back from the frontier past the ~50 empty trailing blocks) so they
    replay their full target. The cadence worst-case recovery is therefore
    read off the MEASURED tail=cadence point, not extrapolated.

Usage: python3 eval/recovery_sensitivity_plot.py [recovery_dir]
"""
import csv
import json
import os
import sys
from collections import defaultdict
from statistics import median

REC_DIR = sys.argv[1] if len(sys.argv) > 1 else "results/recovery"
# BW=1 → grayscale, written to paper/figures/ (B&W-printed journals).
BW = os.environ.get("BW") == "1"
CHARTS_DIR = "paper/figures" if BW else "eval/charts"
if BW:
    C_TAIL, C_OPEN, C_BASE, C_CROSS, C_CAD = "0.0", "0.0", "0.45", "0.3", "0.3"
else:
    C_TAIL, C_OPEN, C_BASE, C_CROSS, C_CAD = "#990000", "#000099", "#444444", "#cc6600", "#009900"


def load():
    with open(os.path.join(REC_DIR, "tail_sweep_meta.json")) as f:
        meta = json.load(f)
    with open(os.path.join(REC_DIR, "tail_sweep.csv")) as f:
        rows = list(csv.DictReader(f))
    return meta, rows


def aggregate(rows):
    """(threads, tail_target) -> median metrics over reps."""
    groups = defaultdict(list)
    for r in rows:
        groups[(int(r["threads"]), int(r["tail_target"]))].append(r)
    agg = {}
    for key, rs in groups.items():
        agg[key] = {
            "replayed": int(rs[0]["recovery_replayed"]),  # deterministic
            "tail_scan_ms": median(float(x["tail_scan_ms"]) for x in rs),
            "total_ms": median(float(x["total_ms"]) for x in rs),
        }
    return agg


def series(agg, threads, key):
    pts = sorted((k[1], v) for k, v in agg.items() if k[0] == threads)
    return ([v["replayed"] for _, v in pts], [v[key] for _, v in pts])


def fit(xs, ys):
    try:
        import numpy as np
        if len(xs) >= 2:
            s, b = np.polyfit(xs, ys, 1)
            return float(s), float(b)
    except Exception:
        pass
    return float("nan"), float("nan")


def main():
    meta, rows = load()
    agg = aggregate(rows)
    baseline = meta["baseline_ms"]
    cadence = int(meta.get("cadence_entries", 4096))
    spb = int(meta.get("slots_per_block", 1518))

    x1, y1 = series(agg, 1, "tail_scan_ms")      # O(tail) mechanism (t=1)
    x32s, y32s = series(agg, 32, "tail_scan_ms")  # tail scan, parallel
    x32, y32 = series(agg, 32, "total_ms")        # deployment total open

    s1, b1 = fit(x1, y1)        # t=1 tail-scan slope (pure O(tail))
    s32, b32 = fit(x32, y32)    # t=32 total slope (crossover)

    crossover = (baseline - b32) / s32 if s32 and s32 > 0 else float("nan")
    cadence_rec = b32 + cadence * s32   # extrapolated t=32 total at cadence

    o = []
    o.append("C3 recovery sensitivity — recovery time vs tail size")
    o.append("N=%d  current_block=%d  slots/block=%d  hot_buckets=%d  "
             "cceh_cap=%d  reps=%d"
             % (meta["N"], meta["current_block"], spb, meta["hot_buckets"],
                meta["cceh_init_cap"], meta["reps"]))
    o.append("baseline (Viper O(N) full rebuild, t=%d): %.1f ms"
             % (meta.get("baseline_recovery_threads", 32), baseline))
    o.append("")
    o.append("[figure 1] O(tail) tail-scan replay (threads=1):")
    o.append("  slope     = %.1f ms / M entries  (%.2f us/entry)"
             % (s1 * 1e6, s1 * 1e3))
    o.append("  intercept = %.1f ms (fixed cold-start floor: new open + cold "
             "mmap page-fault)" % b1)
    o.append("")
    o.append("[parallel speedup] tail_scan_ms  t=1 / t=32  (PMem latency hidden"
             " by threads):")
    t1 = {k[1]: v for k, v in agg.items() if k[0] == 1}
    t32 = {k[1]: v for k, v in agg.items() if k[0] == 32}
    for tail in sorted(set(t1) & set(t32)):
        a, b = t1[tail]["tail_scan_ms"], t32[tail]["tail_scan_ms"]
        sp = (a / b) if b > 0 else float("nan")
        o.append("  tail=%-9d replayed=%-9d  t1=%9.2f ms  t32=%8.2f ms  %.1fx"
                 % (tail, t1[tail]["replayed"], a, b, sp))
    o.append("")
    o.append("[figure 2] deployment open (threads=32) vs Viper rebuild:")
    o.append("  t=32 total slope = %.1f ms / M entries  (intercept %.1f ms "
             "fixed open floor)" % (s32 * 1e6, b32))
    o.append("  crossover (HiOM open == Viper rebuild) ≈ %.0f entries "
             "(~%.0f blocks)" % (crossover, crossover / spb))
    o.append("")
    o.append("[cadence] worst-case unflushed tail = cadence_entries = %d (~%d "
             "blocks)" % (cadence, max(1, cadence // spb)))
    cm = t32.get(cadence)
    if cm and cm["replayed"] > 0:
        o.append("  MEASURED at tail=%d (replayed=%d): open = %.1f ms (t=32)  "
                 "vs  baseline %.1f ms  ->  %.1fx faster"
                 % (cadence, cm["replayed"], cm["total_ms"], baseline,
                    baseline / cm["total_ms"] if cm["total_ms"] > 0
                    else float("nan")))
        o.append("  (open is dominated by a ~%.0f ms cold-tier first-touch "
                 "floor; the %d-entry replay itself is a small increment.)"
                 % (b32, cadence))
    else:
        o.append("  extrapolated (no direct point): open ≈ %.1f ms -> %.0fx"
                 % (cadence_rec, baseline / cadence_rec
                    if cadence_rec > 0 else float("nan")))
    o.append("  safety margin  crossover / cadence ≈ %.0fx  -> any realistic "
             "cadence keeps recovery far below O(N) rebuild."
             % (crossover / cadence if cadence else float("nan")))

    txt = "\n".join(o) + "\n"
    os.makedirs(REC_DIR, exist_ok=True)
    with open(os.path.join(REC_DIR, "summary.txt"), "w") as f:
        f.write(txt)
    print(txt.rstrip())

    # ----- charts -----
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except Exception as e:
        print("matplotlib/numpy unavailable, charts skipped:", e)
        return
    os.makedirs(CHARTS_DIR, exist_ok=True)
    nm = meta["N"] // 1_000_000

    # figure 1: pure O(tail), threads=1, through origin.
    if x1:
        plt.figure(figsize=(6, 4))
        plt.scatter(x1, y1, color=C_TAIL, zorder=3, label="tail_scan (t=1)")
        if s1 == s1:  # not nan
            xs = np.linspace(0, max(x1), 100)
            plt.plot(xs, s1 * xs + b1, color=C_TAIL, ls="--", alpha=0.7,
                     label="fit: %.0f ms/M, intercept %.1f ms" % (s1 * 1e6, b1))
        plt.xlabel("tail entries replayed (measured)")
        plt.ylabel("tail-scan time (ms)")
        plt.title("C3: O(tail) tail-scan replay (N=%dM, threads=1)" % nm)
        plt.grid(True, alpha=0.3)
        plt.legend()
        plt.tight_layout()
        p1 = os.path.join(CHARTS_DIR, "recovery_tail_scan.pdf")
        plt.savefig(p1, dpi=130)
        print("wrote", p1)

    # figure 2: total open vs Viper baseline (t=32) + crossover + cadence.
    if x32:
        plt.figure(figsize=(6, 4))
        plt.plot(x32, y32, marker="o", color=C_OPEN,
                 label="HiOM open total (t=32)")
        plt.axhline(baseline, color=C_BASE, ls="--",
                    label="Viper O(N) rebuild (%.0f ms)" % baseline)
        if crossover == crossover and 0 < crossover <= max(x32) * 1.5:
            plt.axvline(crossover, color=C_CROSS, ls="-.", alpha=0.8,
                        label="crossover ≈ %.0fK entries" % (crossover / 1e3))
        cm = t32.get(cadence)
        cadence_x = cm["replayed"] if (cm and cm["replayed"] > 0) else cadence
        plt.axvline(cadence_x, color=C_CAD, ls=":", alpha=0.9,
                    label="cadence=%d worst-case tail" % cadence)
        plt.xlabel("tail entries replayed (measured)")
        plt.ylabel("total open time (ms)")
        plt.title("C3: HiOM open vs Viper rebuild (N=%dM, threads=32)" % nm)
        plt.grid(True, alpha=0.3)
        plt.legend()
        plt.tight_layout()
        p2 = os.path.join(CHARTS_DIR, "recovery_vs_baseline.pdf")
        plt.savefig(p2, dpi=130)
        print("wrote", p2)


if __name__ == "__main__":
    main()
