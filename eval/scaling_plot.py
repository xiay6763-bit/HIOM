#!/usr/bin/env python3
"""
Scaling sweep plotter — Phase 2 of the HiOM win-condition experiment.

Walks /root/viper/results/scaling/*.json (one JSON per
(fixture, workload, size) invocation produced by run_scaling_sweep.sh),
groups by (workload, fixture, size, threads), takes the median of the
3 reps Google Benchmark emits, and produces one PDF per workload with
a 3-row × 3-col grid:
  Row 1: throughput (M items/s per thread)  vs dataset size  (3 panels: t=1, t=8, t=24)
  Row 2: dram_loaded_mb                     vs dataset size  (3 panels)
  Row 3: HiOM-only diagnostics              vs dataset size  (hot_size, hot_evictions, hot_hit_rate)

X-axis is log-scaled dataset size; each panel shows Viper and HiOM as
two lines.

Output: eval/charts/scaling_<workload>.pdf
"""

import json
import os
import re
from collections import defaultdict
from glob import glob

import matplotlib.pyplot as plt

RESULTS_DIR = "/root/viper/results/scaling"
OUTPUT_DIR = "/root/viper/eval/charts"
WORKLOADS = ("100r_zipf", "100r_uniform")
THREAD_AXES = (1, 8, 24)
FIXTURES = ("ViperFixture", "HiOMFixture")
STYLES = {
    "ViperFixture": {"color": "#1f77b4", "marker": "o", "ms": 8, "label": "Viper"},
    "HiOMFixture":  {"color": "#d62728", "marker": "s", "ms": 8, "label": "HiOM"},
}

# Parses a Google Benchmark "name" field like
#   HiOMFixture<KeyType8,ValueType200>/100r_zipf_tp/iterations:1/repeats:3/real_time/threads:8_median
# returning (workload, threads, agg) or None.
NAME_RE = re.compile(
    r"^(?P<fixture>\w+Fixture)<[^>]+>/(?P<workload>\w+)_tp/.+threads:(?P<threads>\d+)(?:_(?P<agg>\w+))?$"
)


def parse_results():
    """Returns dict[workload][fixture][size][threads] = {metric: value}."""
    data = defaultdict(lambda: defaultdict(lambda: defaultdict(lambda: defaultdict(dict))))
    json_paths = sorted(glob(os.path.join(RESULTS_DIR, "*.json")))
    if not json_paths:
        print(f"No JSON files in {RESULTS_DIR} — nothing to plot.")
        return data
    for path in json_paths:
        # Filename convention: <Fixture>_<workload>_<size>M.json
        fname = os.path.basename(path)
        m = re.match(r"(?P<fixture>\w+Fixture)_(?P<workload>\w+)_(?P<size>\d+)M\.json$", fname)
        if not m:
            continue
        fixture = m.group("fixture")
        workload = m.group("workload")
        size_m = int(m.group("size"))
        with open(path) as f:
            j = json.load(f)
        for bm in j["benchmarks"]:
            n = NAME_RE.match(bm["name"])
            if not n or n.group("agg") != "median":
                continue
            threads = int(n.group("threads"))
            cell = data[workload][fixture][size_m][threads]
            # Per-thread throughput (Google Benchmark already normalizes).
            cell["ips_per_thr"] = bm.get("items_per_second", 0)
            cell["dram_mb"] = bm.get("dram_loaded_mb", 0)
            cell["rss_mb"] = bm.get("rss_loaded_mb", 0)
            cell["hot_size"] = bm.get("hot_tier_size", 0)
            cell["hot_evictions"] = bm.get("hot_evictions", 0)
            cell["hot_hit_rate"] = bm.get("hot_hit_rate", None)
    return data


def plot_workload(workload, w_data):
    """Builds one PDF for the given workload."""
    sizes_v = sorted(w_data.get("ViperFixture", {}).keys())
    sizes_h = sorted(w_data.get("HiOMFixture", {}).keys())
    sizes = sorted(set(sizes_v) | set(sizes_h))
    if not sizes:
        print(f"  skip {workload} — no data")
        return

    fig, axes = plt.subplots(3, 3, figsize=(15, 12), sharex=True)
    fig.suptitle(f"HiOM vs Viper scaling — {workload}", fontsize=14, fontweight="bold")

    # Row 1: throughput
    for j, t in enumerate(THREAD_AXES):
        ax = axes[0, j]
        for fixture in FIXTURES:
            xs, ys = [], []
            for size_m in sizes:
                cell = w_data[fixture].get(size_m, {}).get(t)
                if cell and cell.get("ips_per_thr"):
                    xs.append(size_m)
                    ys.append(cell["ips_per_thr"] / 1e6)
            if xs:
                ax.plot(xs, ys, **{k: v for k, v in STYLES[fixture].items() if k != "label"},
                        label=STYLES[fixture]["label"])
        ax.set_title(f"Throughput — t={t}")
        ax.set_ylabel("M items/s per thread")
        ax.set_xscale("log")
        ax.grid(True, alpha=0.3)
        ax.legend()

    # Row 2: DRAM
    for j, t in enumerate(THREAD_AXES):
        ax = axes[1, j]
        for fixture in FIXTURES:
            xs, ys = [], []
            for size_m in sizes:
                cell = w_data[fixture].get(size_m, {}).get(t)
                if cell and cell.get("dram_mb"):
                    xs.append(size_m)
                    ys.append(cell["dram_mb"])
            if xs:
                ax.plot(xs, ys, **{k: v for k, v in STYLES[fixture].items() if k != "label"},
                        label=STYLES[fixture]["label"])
        ax.set_title(f"DRAM (rss_loaded) — t={t}")
        ax.set_ylabel("MB")
        ax.set_xscale("log")
        ax.grid(True, alpha=0.3)
        ax.legend()

    # Row 3: HiOM HotTier diagnostics (3 panels: size, evictions, hit_rate)
    metrics = [("hot_size", "HotTier size (# slots)"),
               ("hot_evictions", "HotTier evictions (cumulative)"),
               ("hot_hit_rate", "HotTier hit rate")]
    for j, (metric, ylabel) in enumerate(metrics):
        ax = axes[2, j]
        # Pick t=8 as the representative thread axis for HotTier metrics.
        xs, ys = [], []
        for size_m in sizes:
            cell = w_data["HiOMFixture"].get(size_m, {}).get(8)
            if cell and cell.get(metric) is not None:
                xs.append(size_m)
                ys.append(cell[metric])
        if xs:
            ax.plot(xs, ys, **{k: v for k, v in STYLES["HiOMFixture"].items() if k != "label"},
                    label="HiOM (t=8)")
            ax.legend()
        # Reference line for HotTier capacity on hot_size panel.
        if metric == "hot_size":
            ax.axhline(33554432, ls="--", color="gray", alpha=0.5, label="HotTier capacity 33.5 M")
            ax.legend()
        ax.set_title(ylabel)
        ax.set_xscale("log")
        ax.set_xlabel("Dataset size (M records)")
        ax.grid(True, alpha=0.3)

    # Bottom-row x-labels
    for j in range(3):
        axes[2, j].set_xlabel("Dataset size (M records)")

    plt.tight_layout(rect=[0, 0, 1, 0.96])
    out_path = os.path.join(OUTPUT_DIR, f"scaling_{workload}.pdf")
    plt.savefig(out_path)
    plt.close()
    print(f"  wrote {out_path}")


def print_summary(data):
    """Compact tabular summary to stdout."""
    print("\n=== Summary (median of 3 reps; threads=8 row) ===")
    for workload in WORKLOADS:
        w = data.get(workload, {})
        if not w:
            continue
        sizes = sorted({s for fx in FIXTURES for s in w.get(fx, {}).keys()})
        if not sizes:
            continue
        print(f"\n[{workload}]  size →  Viper M/thr   HiOM M/thr   H/V ratio   Viper RSS(MB)   HiOM RSS(MB)   HotTier size   evictions   hit_rate")
        print("-" * 130)
        for s in sizes:
            v = w.get("ViperFixture", {}).get(s, {}).get(8, {})
            h = w.get("HiOMFixture", {}).get(s, {}).get(8, {})
            v_ips = v.get("ips_per_thr", 0) / 1e6
            h_ips = h.get("ips_per_thr", 0) / 1e6
            ratio = h_ips / v_ips if v_ips else 0
            v_rss = v.get("rss_mb", 0)
            h_rss = h.get("rss_mb", 0)
            h_size = h.get("hot_size", 0)
            h_evict = h.get("hot_evictions", 0)
            h_hr = h.get("hot_hit_rate", None)
            hr_str = f"{h_hr:.3f}" if h_hr else "n/a"
            print(f"  {s:3d}M     {v_ips:7.2f}M     {h_ips:7.2f}M     {ratio:6.3f}    {v_rss:10.0f}      {h_rss:10.0f}    {h_size:11.0f}    {h_evict:8.0f}    {hr_str}")


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    data = parse_results()
    if not data:
        return
    for workload in WORKLOADS:
        if workload in data:
            print(f"Plotting {workload}…")
            plot_workload(workload, data[workload])
    print_summary(data)


if __name__ == "__main__":
    main()
