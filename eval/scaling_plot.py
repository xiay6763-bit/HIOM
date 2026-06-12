#!/usr/bin/env python3
"""
HiOM HotTier-vs-dataset-size diagnostic — the surviving slice of the old
Phase-2 scaling sweep.

Reads /root/viper/results/scaling/*.json and plots how HiOM's fixed-capacity
HotTier behaves as the dataset grows toward that capacity (2^21 buckets =
33.5 M slots): occupancy (fill), cumulative SIEVE evictions (their onset at
capacity), and hit-rate (graceful degradation). This is the working-set
evidence behind C2; the read workloads (100r zipf/uniform) show it cleanest.

  → eval/charts/hot_scaling.pdf   (1x3: HotTier size | evictions | hit-rate vs N)

RETIRED 2026-06-08 — the rest of the Phase-2 vs-N figures were superseded by
the four-system @10 M figures, so their plotting code was dropped from here
(figures deleted; code removed to prevent regeneration):
  - vs-N throughput   (scaling_*.pdf row 1) → near-flat in N, no Pareto
                        consequence; use thread_scaling_*.pdf (tput vs threads).
  - vs-N fixture DRAM (scaling_*.pdf row 2) → covered by footprint.pdf (@10 M
                        stacked bar) + the §Phase-2 DRAM table (DRAM is also
                        thread-independent, so the old 3-panel layout was 3x
                        redundant).
  - tail latency vs N (latency_*.pdf)       → covered by thread_scaling_*_lat.pdf
                        (4-system latency vs threads).
Only the HotTier-vs-N diagnostic below was unique, so it is kept standalone.
"""
import json
import os
import re
from collections import defaultdict
from glob import glob

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

RESULTS_DIR = "/root/viper/results/scaling"
# BW=1 → grayscale, written to paper/figures/ (B&W-printed journals).
BW = os.environ.get("BW") == "1"
OUTPUT_DIR = "/root/viper/paper/figures" if BW else "/root/viper/eval/charts"
MAX_SIZE_M = 33
HOT_THREADS = 8           # representative thread axis for HotTier metrics
HOT_CAPACITY = 33554432   # 2^21 buckets x 16 slots = 33.5 M

# HotTier behaviour is workload-dependent; the read workloads show the
# fill-toward-capacity / graceful-degradation story cleanest (no write
# interference). Extend this tuple to overlay YCSB-A/B if ever wanted.
if BW:
    WORKLOADS = (
        ("100r_zipf",    {"color": "0.0",  "marker": "s", "ms": 7, "ls": "-",  "mfc": "0.0",   "label": "100r zipf"}),
        ("100r_uniform", {"color": "0.45", "marker": "^", "ms": 7, "ls": "--", "mfc": "white", "label": "100r uniform"}),
    )
else:
    WORKLOADS = (
        ("100r_zipf",    {"color": "#d62728", "marker": "s", "ms": 7, "ls": "-", "mfc": "#d62728", "label": "100r zipf"}),
        ("100r_uniform", {"color": "#1f77b4", "marker": "o", "ms": 7, "ls": "-", "mfc": "#1f77b4", "label": "100r uniform"}),
    )

# Google Benchmark "name" like
#   HiOMFixture<KeyType8,ValueType200>/100r_zipf_tp/iterations:1/repeats:3/real_time/threads:8_median
NAME_RE = re.compile(
    r"^(?P<fixture>\w+Fixture)<[^>]+>/(?P<workload>\w+)_(?P<metric>tp|lat)/.+"
    r"threads:(?P<threads>\d+)(?:_(?P<agg>\w+))?$"
)
FNAME_RE = re.compile(
    r"(?P<fixture>\w+Fixture)_(?P<workload>\w+)_(?P<size>\d+)M(?:_(?P<metric>tp|lat))?\.json$"
)


def parse_results():
    """dict[workload][fixture][size][threads] = {metric: value}. The HiOM
    HotTier counters (hot_size / hot_evictions / hot_hit_rate) ride the tp
    cells; we only read those."""
    data = defaultdict(lambda: defaultdict(lambda: defaultdict(lambda: defaultdict(dict))))
    json_paths = sorted(glob(os.path.join(RESULTS_DIR, "*.json")))
    if not json_paths:
        print(f"No JSON files in {RESULTS_DIR} — nothing to plot.")
        return data
    for path in json_paths:
        m = FNAME_RE.match(os.path.basename(path))
        if not m:
            continue
        workload = m.group("workload")
        size_m = int(m.group("size"))
        if size_m > MAX_SIZE_M:
            continue
        fixture = m.group("fixture")
        try:
            with open(path) as f:
                j = json.load(f)
        except (json.JSONDecodeError, OSError):
            continue
        for bm in j.get("benchmarks", []):
            n = NAME_RE.match(bm["name"])
            if not n or n.group("agg") != "median" or n.group("metric") != "tp":
                continue
            threads = int(n.group("threads"))
            cell = data[workload][fixture][size_m][threads]
            cell["hot_size"] = bm.get("hot_tier_size", 0)
            cell["hot_evictions"] = bm.get("hot_evictions", 0)
            cell["hot_hit_rate"] = bm.get("hot_hit_rate", None)
    return data


def plot_hot_scaling(data):
    """1x3: HiOM HotTier occupancy | cumulative evictions | hit-rate vs N."""
    panels = [("hot_size", "HotTier occupancy (# slots)", False),
              ("hot_evictions", "Cumulative SIEVE evictions", False),
              ("hot_hit_rate", "HotTier hit rate", True)]
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5))
    drew = False
    for ax, (metric, ylabel, is_rate) in zip(axes, panels):
        for wl, st in WORKLOADS:
            hi = data.get(wl, {}).get("HiOMFixture", {})
            xs, ys = [], []
            for size_m in sorted(hi.keys()):
                v = hi[size_m].get(HOT_THREADS, {}).get(metric)
                if v is not None:
                    xs.append(size_m)
                    ys.append(v)
            if xs:
                drew = True
                ax.plot(xs, ys, color=st["color"], marker=st["marker"],
                        ms=st["ms"], ls=st.get("ls", "-"), mfc=st.get("mfc", st["color"]),
                        lw=2.0, label=st["label"])
        if metric == "hot_size":
            ax.axhline(HOT_CAPACITY, ls=":", color="0.4", alpha=0.8,
                       label="HotTier capacity (33.5 M)")
        ax.set_title(ylabel, fontsize=12)
        ax.set_xlabel("Dataset size (M records)")
        ax.set_xscale("log")
        if is_rate:
            ax.set_ylim(0, 1.02)
        ax.grid(True, alpha=0.3, which="both")
        if ax.get_legend_handles_labels()[0]:
            ax.legend(fontsize=8)
    if not drew:
        print("  no HiOM HotTier data in results/scaling/ — skipped.")
        plt.close()
        return
    fig.suptitle("HiOM HotTier vs dataset size (t=8): fill → eviction at capacity → graceful hit-rate",
                 fontsize=13, fontweight="bold")
    plt.tight_layout(rect=[0, 0, 1, 0.95])
    out = os.path.join(OUTPUT_DIR, "hot_scaling.pdf")
    plt.savefig(out)
    plt.close()
    print("wrote", out)

    # stdout summary
    for wl, _ in WORKLOADS:
        hi = data.get(wl, {}).get("HiOMFixture", {})
        if not hi:
            continue
        print(f"\n[{wl}] HiOM HotTier (t=8)")
        print(f"  {'sizeM':>6}{'hot_size':>12}{'evictions':>12}{'hit_rate':>10}")
        for size_m in sorted(hi.keys()):
            c = hi[size_m].get(HOT_THREADS, {})
            hr = c.get("hot_hit_rate")
            print(f"  {size_m:>6}{c.get('hot_size', 0):>12}{c.get('hot_evictions', 0):>12}"
                  f"{(f'{hr:.4f}' if hr is not None else '-'):>10}")


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    data = parse_results()
    if data:
        plot_hot_scaling(data)


if __name__ == "__main__":
    main()
