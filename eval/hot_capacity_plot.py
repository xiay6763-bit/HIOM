#!/usr/bin/env python3
"""C2 HotTier capacity sweep: summarize GBench JSON -> CSV + plots.

Measurement convention (口径 1, see scan_hot_capacity.sh header):
  - Capacity curve (hit rate + throughput vs HotTier DRAM) is at t=1: small
    capacities livelock at high thread counts, and hit rate is
    thread-independent, so t=1 gives a clean curve across all 6 capacities.
  - The read-heavy throughput RATIO (HiOM/Viper) is reported separately at
    t=8 full-capacity (log2=21), the paper's t=8 convention (no livelock).

Reads results/hot_scan/{hiom_log2_*.json, viper_baseline_t1.json,
hiom_fullcap_t8.json, viper_fullcap_t8.json}, takes ONLY the GBench `_median`
aggregate row of `_tp` benchmarks, and emits:
  - results/hot_scan/summary.csv            (t=1 capacity curve)
  - results/hot_scan/readheavy_ratio.txt    (t=8 full-cap HiOM/Viper ratio)
  - eval/charts/hot_capacity_hitrate.pdf
  - eval/charts/hot_capacity_throughput.pdf

x-axis is hot_tier_index_dram_mb (HotTier index-structure DRAM, authoritative;
NOT fixture_dram_mb which is the whole-fixture total). Hit rate is the
cumulative read-phase hit rate, warming from an empty target-capacity HotTier
(prefill-then-rebuild in the fixture); see HIOM.md C2 notes.

Usage: python3 eval/hot_capacity_plot.py [hot_scan_dir]
"""
import csv
import glob
import json
import os
import re
import sys

HOT_SCAN_DIR = sys.argv[1] if len(sys.argv) > 1 else "results/hot_scan"
CHARTS_DIR = "eval/charts"


def workload_of(name):
    # Only throughput (_tp) variants carry the curve metrics; _lat is the
    # latency variant (excluded by the scan filter, guarded here too).
    if "_tp" not in name:
        return None
    if "uniform" in name:
        return "100r_uniform"
    if "zipf" in name:
        return "100r_zipf"
    return None


def median_rows(path):
    """Yield (workload, benchmark_obj) for each _median aggregate row of a _tp run."""
    with open(path) as f:
        d = json.load(f)
    for b in d.get("benchmarks", []):
        if b.get("aggregate_name") != "median":
            continue
        wl = workload_of(b["name"])
        if wl:
            yield wl, b


def ips_by_workload(path):
    """workload -> items_per_second (median), or {} if file absent."""
    out = {}
    if not os.path.exists(path):
        return out
    for wl, b in median_rows(path):
        out[wl] = b.get("items_per_second", 0.0)
    return out


def main():
    # (1) t=1 capacity curve.
    rows = []
    for path in sorted(glob.glob(os.path.join(HOT_SCAN_DIR, "hiom_log2_*.json"))):
        m = re.search(r"hiom_log2_(\d+)\.json", path)
        if not m:
            continue
        log2 = int(m.group(1))
        for wl, b in median_rows(path):
            rows.append({
                "workload": wl,
                "log2": log2,
                "hot_buckets": 1 << log2,
                # hot_capacity etc. taken straight from JSON, not recomputed,
                # so a layout/associativity change can't silently desync.
                "hot_capacity": int(b.get("hot_capacity", 0)),
                "hot_tier_index_dram_mb": round(b.get("hot_tier_index_dram_mb", 0.0), 3),
                "fixture_dram_mb": round(b.get("fixture_dram_mb", 0.0), 3),
                "hot_hit_rate": round(b.get("hot_hit_rate", float("nan")), 6),
                "items_per_second": round(b.get("items_per_second", 0.0), 1),
                "hot_evictions": int(b.get("hot_evictions", 0)),
            })

    # Viper t=1 baseline (horizontal reference for the throughput curve).
    viper_t1 = ips_by_workload(os.path.join(HOT_SCAN_DIR, "viper_baseline_t1.json"))

    rows.sort(key=lambda r: (r["workload"], r["log2"]))
    os.makedirs(HOT_SCAN_DIR, exist_ok=True)
    csv_path = os.path.join(HOT_SCAN_DIR, "summary.csv")
    fields = ["workload", "log2", "hot_buckets", "hot_capacity",
              "hot_tier_index_dram_mb", "fixture_dram_mb", "hot_hit_rate",
              "items_per_second", "hot_evictions"]
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    print("wrote", csv_path, "(%d rows, t=1 curve)" % len(rows))
    print("viper t=1 baseline items/s:", {k: round(v, 1) for k, v in viper_t1.items()})
    for r in rows:
        print(f"  {r['workload']:13s} log2={r['log2']} "
              f"dram={r['hot_tier_index_dram_mb']:8.1f}MB "
              f"hit={r['hot_hit_rate']:.4f} "
              f"ips={r['items_per_second']:,.0f} evict={r['hot_evictions']:,}")

    # (2) Read-heavy ratio at t=8 full-capacity (log2=21).
    hiom_t8 = ips_by_workload(os.path.join(HOT_SCAN_DIR, "hiom_fullcap_t8.json"))
    viper_t8 = ips_by_workload(os.path.join(HOT_SCAN_DIR, "viper_fullcap_t8.json"))
    ratio_lines = ["# Read-heavy ratio (100r YCSB-C, t=8, full-capacity log2=21)",
                   "# HiOM / Viper items_per_second, median of 3 reps",
                   "workload\thiom_ips\tviper_ips\tratio"]
    for wl in sorted(set(hiom_t8) | set(viper_t8)):
        h = hiom_t8.get(wl, 0.0)
        v = viper_t8.get(wl, 0.0)
        r = (h / v) if v > 0 else float("nan")
        ratio_lines.append(f"{wl}\t{h:.0f}\t{v:.0f}\t{r:.3f}")
    ratio_txt = "\n".join(ratio_lines) + "\n"
    ratio_path = os.path.join(HOT_SCAN_DIR, "readheavy_ratio.txt")
    with open(ratio_path, "w") as f:
        f.write(ratio_txt)
    print("\nwrote", ratio_path)
    print(ratio_txt.rstrip())

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        print("matplotlib unavailable, charts skipped:", e)
        return
    os.makedirs(CHARTS_DIR, exist_ok=True)
    colors = {"100r_uniform": "#000099", "100r_zipf": "#990000"}

    def series(wl, key):
        rs = [r for r in rows if r["workload"] == wl]
        return ([r["hot_tier_index_dram_mb"] for r in rs],
                [r[key] for r in rs])

    # (1) hit rate vs HotTier DRAM (t=1)
    plt.figure(figsize=(6, 4))
    for wl in ("100r_zipf", "100r_uniform"):
        x, y = series(wl, "hot_hit_rate")
        if x:
            plt.plot(x, y, marker="o", color=colors[wl], label=wl)
    plt.xlabel("HotTier index DRAM (MB)")
    plt.ylabel("cumulative read-phase hit rate")
    plt.title("C2: HotTier hit rate vs DRAM budget (10M, t=1)")
    plt.ylim(0, 1.02)
    plt.grid(True, alpha=0.3)
    plt.legend()
    p1 = os.path.join(CHARTS_DIR, "hot_capacity_hitrate.pdf")
    plt.tight_layout()
    plt.savefig(p1, dpi=130)
    print("wrote", p1)

    # (2) throughput vs HotTier DRAM (t=1) + Viper t=1 baseline
    plt.figure(figsize=(6, 4))
    for wl in ("100r_zipf", "100r_uniform"):
        x, y = series(wl, "items_per_second")
        if x:
            plt.plot(x, [v / 1e6 for v in y], marker="o", color=colors[wl],
                     label="HiOM " + wl)
        if wl in viper_t1:
            plt.axhline(viper_t1[wl] / 1e6, color=colors[wl], ls="--", alpha=0.6,
                        label="Viper " + wl)
    plt.xlabel("HotTier index DRAM (MB)")
    plt.ylabel("throughput (M items/s, t=1)")
    plt.title("C2: read throughput vs DRAM budget (10M, t=1)")
    plt.grid(True, alpha=0.3)
    plt.legend()
    p2 = os.path.join(CHARTS_DIR, "hot_capacity_throughput.pdf")
    plt.tight_layout()
    plt.savefig(p2, dpi=130)
    print("wrote", p2)


if __name__ == "__main__":
    main()
