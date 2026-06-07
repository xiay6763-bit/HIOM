#!/usr/bin/env python3
"""
Scaling sweep plotter — Phase 2 of the HiOM win-condition experiment.

Walks /root/viper/results/scaling/*.json (one JSON per
(fixture, workload, size) invocation produced by run_scaling_sweep.sh),
groups by (workload, fixture, size, threads), takes the median of the
3 reps Google Benchmark emits, and produces one PDF per workload with
a 3-row × 3-col grid:
  Row 1: aggregate throughput (Mops/s)      vs dataset size  (3 panels: t=1, t=8, t=24)
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
# 6-workload matrix: read-only (100r), YCSB-A (50/50 read+update),
# YCSB-B (95/5 read+update), each in zipf + uniform. The 5050/1090
# legacy mixes are out — they're read+INSERT, not the standard
# YCSB-A/B semantics, kept for compat in the binary but not plotted.
WORKLOADS = (
    "100r_zipf", "100r_uniform",
    "a_zipf", "a_uniform",
    "b_zipf", "b_uniform",
)
THREAD_AXES = (1, 8, 24)
FIXTURES = ("ViperFixture", "HiOMFixture", "DashFixture", "CcehFixture")
MAX_SIZE_M = 33
STYLES = {
    "ViperFixture": {"color": "#1f77b4", "marker": "o", "ms": 8, "label": "Viper"},
    "HiOMFixture":  {"color": "#d62728", "marker": "s", "ms": 8, "label": "HiOM"},
    "DashFixture":  {"color": "#2ca02c", "marker": "^", "ms": 8, "label": "Dash (PM-resident)"},
    "CcehFixture":  {"color": "#9467bd", "marker": "D", "ms": 7, "label": "CCEH (DRAM-idx)"},
}

# Parses a Google Benchmark "name" field like
#   HiOMFixture<KeyType8,ValueType200>/100r_zipf_tp/iterations:1/repeats:3/real_time/threads:8_median
#   HiOMFixture<KeyType8,ValueType200>/a_zipf_lat/iterations:1/repeats:3/real_time/threads:8_median
# returning (fixture, workload, metric, threads, agg) or None.
NAME_RE = re.compile(
    r"^(?P<fixture>\w+Fixture)<[^>]+>/(?P<workload>\w+)_(?P<metric>tp|lat)/.+threads:(?P<threads>\d+)(?:_(?P<agg>\w+))?$"
)


def parse_results():
    """Returns dict[workload][fixture][size][threads] = {metric: value}."""
    data = defaultdict(lambda: defaultdict(lambda: defaultdict(lambda: defaultdict(dict))))
    json_paths = sorted(glob(os.path.join(RESULTS_DIR, "*.json")))
    if not json_paths:
        print(f"No JSON files in {RESULTS_DIR} — nothing to plot.")
        return data
    for path in json_paths:
        # Filename convention:
        #   <Fixture>_<workload>_<size>M.json          — Phase 2 legacy (tp)
        #   <Fixture>_<workload>_<size>M_tp.json       — Phase 3 throughput
        #   <Fixture>_<workload>_<size>M_lat.json      — Phase 3 latency
        fname = os.path.basename(path)
        m = re.match(
            r"(?P<fixture>\w+Fixture)_(?P<workload>\w+)_(?P<size>\d+)M(?:_(?P<metric>tp|lat))?\.json$",
            fname)
        if not m:
            continue
        fixture = m.group("fixture")
        workload = m.group("workload")
        size_m = int(m.group("size"))
        if size_m > MAX_SIZE_M:
            continue
        with open(path) as f:
            j = json.load(f)
        for bm in j["benchmarks"]:
            n = NAME_RE.match(bm["name"])
            if not n or n.group("agg") != "median":
                continue
            threads = int(n.group("threads"))
            metric = n.group("metric")
            cell = data[workload][fixture][size_m][threads]
            if metric == "tp":
                # AGGREGATE throughput (all threads combined): Google
                # Benchmark's items_per_second is already system-wide, NOT
                # per-thread (real_time is wall÷threads, so ips × real_time
                # = per-thread ops). Do not multiply by the thread count.
                # The key name `ips_per_thr` is legacy/misnamed — it holds
                # the aggregate rate.
                cell["ips_per_thr"] = bm.get("items_per_second", 0)
                cell["dram_mb"] = bm.get("dram_loaded_mb", 0)
                cell["rss_mb"] = bm.get("rss_loaded_mb", 0)
                cell["rss_baseline_mb"] = bm.get("rss_baseline_mb", 0)
                # Fixture-only DRAM (see comment block above for the
                # subtraction logic). Workload ops vector size depends
                # on the workload type — 100r_uniform is 100 M ops
                # (stress), all others are 5 M ops.
                PREFILL_RECORD_BYTES = 216
                workload_ops_m = 100 if workload == "100r_uniform" else 5
                prefill_mb = size_m * 1e6 * PREFILL_RECORD_BYTES / (1024 ** 2)
                workload_mb = workload_ops_m * 1e6 * PREFILL_RECORD_BYTES / (1024 ** 2)
                cell["fixture_dram_mb"] = max(0.0,
                    cell["dram_mb"] - prefill_mb - workload_mb)
                cell["hot_size"] = bm.get("hot_tier_size", 0)
                cell["hot_evictions"] = bm.get("hot_evictions", 0)
                cell["hot_hit_rate"] = bm.get("hot_hit_rate", None)
            elif metric == "lat":
                # HDR percentiles in nanoseconds. Separate read /
                # write series; aggregated hdr_* kept as legacy.
                for pct in ("median", "90", "95", "99", "999", "9999"):
                    for prefix in ("hdr_", "hdr_read_", "hdr_write_"):
                        key = f"{prefix}{pct}"
                        if key in bm:
                            cell[key] = bm[key]

    # Phase 3: tag cells that timed out in the sweep (see
    # run_scaling_sweep.sh's SWEEP_PER_CELL_TIMEOUT_S handling — the
    # partial JSON is renamed `<file>.timeout_<ts>` so the original
    # path is missing and we'd otherwise silently drop the cell from
    # the chart). We can't tell which specific thread count hung from
    # the filename alone; tag all configured thread counts at that
    # cell so the chart's HANG marker spans the panel where the
    # corresponding line would have been.
    for path in sorted(glob(os.path.join(RESULTS_DIR, "*.json.timeout_*"))):
        fname = os.path.basename(path)
        base = re.sub(r"\.timeout_[0-9_]+$", "", fname)
        m = re.match(
            r"(?P<fixture>\w+Fixture)_(?P<workload>\w+)_(?P<size>\d+)M(?:_(?P<metric>tp|lat))?\.json$",
            base)
        if not m:
            continue
        fixture = m.group("fixture")
        workload = m.group("workload")
        size_m = int(m.group("size"))
        if size_m > MAX_SIZE_M:
            continue
        metric = m.group("metric") or "tp"
        for t in THREAD_AXES:
            cell = data[workload][fixture][size_m][t]
            cell.setdefault("hang_metrics", set()).add(metric)
    return data


def _mark_hang(ax, x, y_frac=0.5, label="HANG\n(M4)"):
    """Draw a red HANG marker at x on the given axis. y is placed at
    y_frac of the current ylim so it stays inside the plot area
    regardless of log/linear y-scale."""
    ymin, ymax = ax.get_ylim()
    # On log scale, "fraction of way up" needs the geometric mean.
    if ax.get_yscale() == "log" and ymin > 0:
        y = (ymin * (ymax / ymin) ** y_frac)
    else:
        y = ymin + y_frac * (ymax - ymin)
    ax.axvline(x, color="red", alpha=0.15, linestyle=":", linewidth=1.5)
    ax.text(x, y, label, ha="center", va="center",
            color="red", fontsize=9, fontweight="bold",
            bbox=dict(boxstyle="round,pad=0.3", facecolor="white",
                      edgecolor="red", alpha=0.9))


def plot_workload(workload, w_data):
    """Builds one PDF for the given workload."""
    sizes = sorted({s for fx in FIXTURES for s in w_data.get(fx, {}).keys()})
    if not sizes:
        print(f"  skip {workload} — no data")
        return

    fig, axes = plt.subplots(3, 3, figsize=(15, 12), sharex=True)
    fig.suptitle(f"HiOM vs Viper scaling — {workload}", fontsize=14, fontweight="bold")

    # Row 1: throughput
    for j, t in enumerate(THREAD_AXES):
        ax = axes[0, j]
        hang_xs = set()
        for fixture in FIXTURES:
            xs, ys = [], []
            for size_m in sizes:
                cell = w_data[fixture].get(size_m, {}).get(t)
                if cell and cell.get("ips_per_thr"):
                    xs.append(size_m)
                    ys.append(cell["ips_per_thr"] / 1e6)
                elif cell and "tp" in cell.get("hang_metrics", set()):
                    hang_xs.add(size_m)
            if xs:
                ax.plot(xs, ys, **{k: v for k, v in STYLES[fixture].items() if k != "label"},
                        label=STYLES[fixture]["label"])
        ax.set_title(f"Throughput — t={t}")
        ax.set_ylabel("Aggregate throughput (Mops/s)")
        ax.set_xscale("log")
        ax.grid(True, alpha=0.3)
        ax.legend()
        # Draw HANG markers AFTER lines so axis limits are settled.
        for x in sorted(hang_xs):
            _mark_hang(ax, x, label="HANG\n(M4)")

    # Row 2: fixture-only DRAM (subtracts YCSB harness baseline so the
    # comparison reflects what each system actually allocates, not the
    # 10 GB+ prefill_data std::vector both fixtures share).
    for j, t in enumerate(THREAD_AXES):
        ax = axes[1, j]
        for fixture in FIXTURES:
            xs, ys = [], []
            for size_m in sizes:
                cell = w_data[fixture].get(size_m, {}).get(t)
                if cell and cell.get("fixture_dram_mb") is not None:
                    xs.append(size_m)
                    ys.append(cell["fixture_dram_mb"])
            if xs:
                ax.plot(xs, ys, **{k: v for k, v in STYLES[fixture].items() if k != "label"},
                        label=STYLES[fixture]["label"])
        ax.set_title(f"Fixture DRAM (excl. YCSB std::vectors) — t={t}")
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


def plot_latency(workload, w_data):
    """Tail-latency plot: read p99/p999 and write p99/p999 vs size.

    For read-only workloads (100r_*) we drop the write panels — they
    have no data by construction (YCSB-C analog) and the "no data"
    placeholders looked like a chart bug. Layout becomes 1×2 (Read p99
    | Read p99.9). For mixed workloads (YCSB-A/B) the 2×2 layout
    stays so read and write tails sit side-by-side at the same size.
    Workloads where the relevant op type has zero samples (e.g., the
    write panel for 100r_*) simply have no line and a no-data note.
    """
    sizes = sorted({s for fx in FIXTURES
                    for s in w_data.get(fx, {}).keys()})
    if not sizes:
        return
    read_only = workload.startswith("100r")
    if read_only:
        fig, axes_arr = plt.subplots(1, 2, figsize=(12, 4.5), sharex=True)
        panels = [
            (0, 0, "hdr_read_99",  "Read p99"),
            (0, 1, "hdr_read_999", "Read p99.9"),
        ]
    else:
        fig, axes_arr = plt.subplots(2, 2, figsize=(12, 8), sharex=True)
        panels = [
            (0, 0, "hdr_read_99",   "Read p99"),
            (0, 1, "hdr_write_99",  "Write p99"),
            (1, 0, "hdr_read_999",  "Read p99.9"),
            (1, 1, "hdr_write_999", "Write p99.9"),
        ]
    fig.suptitle(f"Tail latency — {workload} (t=8)",
                 fontsize=14, fontweight="bold")

    def get_ax(r, c):
        # 1×N axes are returned as a flat array, 2×N as a 2-D one.
        return axes_arr[c] if read_only else axes_arr[r, c]

    any_data = False
    bottom_row = 0 if read_only else 1
    for (r, c, key, title) in panels:
        ax = get_ax(r, c)
        hang_xs = set()
        for fixture in FIXTURES:
            xs, ys = [], []
            for size_m in sizes:
                cell = w_data[fixture].get(size_m, {}).get(8, {})
                if key in cell and cell[key] > 0:
                    xs.append(size_m)
                    ys.append(cell[key] / 1000.0)  # ns → μs
                elif cell and "lat" in cell.get("hang_metrics", set()):
                    hang_xs.add(size_m)
            if xs:
                any_data = True
                ax.plot(xs, ys,
                        **{k: v for k, v in STYLES[fixture].items() if k != "label"},
                        label=STYLES[fixture]["label"])
        ax.set_title(title)
        ax.set_ylabel("μs")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.grid(True, alpha=0.3, which="both")
        if ax.lines:
            ax.legend()
        else:
            ax.text(0.5, 0.5, "no data\n(workload has\nno ops of this type)",
                    ha="center", va="center", transform=ax.transAxes,
                    color="gray", fontsize=9)
        for x in sorted(hang_xs):
            _mark_hang(ax, x, label="HANG\n(M4)")
        if r == bottom_row:
            ax.set_xlabel("Dataset size (M records)")
    if not any_data:
        plt.close()
        return
    plt.tight_layout(rect=[0, 0, 1, 0.95])
    out_path = os.path.join(OUTPUT_DIR, f"latency_{workload}.pdf")
    plt.savefig(out_path)
    plt.close()
    print(f"  wrote {out_path}")


def print_summary(data):
    """Compact tabular summary to stdout — aggregate throughput (Mops/s)
    at t=8 for all four systems, with the ratio vs Viper in parentheses."""
    print("\n=== Summary (median of 3 reps; threads=8 row) ===")
    fix_order = [("ViperFixture", "Viper"), ("HiOMFixture", "HiOM"),
                 ("DashFixture", "Dash"), ("CcehFixture", "CCEH")]
    for workload in WORKLOADS:
        w = data.get(workload, {})
        if not w:
            continue
        present = [(fx, lbl) for fx, lbl in fix_order if w.get(fx)]
        sizes = sorted({s for fx, _ in present for s in w.get(fx, {}).keys()})
        if not sizes:
            continue
        print(f"\n[{workload}]  aggregate Mops/s @ t=8  (parens = ratio vs Viper)")
        header = "  ".join(f"{lbl:>14}" for _, lbl in present)
        print(f"  {'size':>5}   {header}")
        print("-" * (10 + len(present) * 16))
        for s in sizes:
            v_ips = w.get("ViperFixture", {}).get(s, {}).get(8, {}).get("ips_per_thr", 0) / 1e6
            cols = []
            for fx, _ in present:
                cell = w.get(fx, {}).get(s, {}).get(8, {})
                if "tp" in cell.get("hang_metrics", set()):
                    cols.append(f"{'HANG':>14}")
                    continue
                ips = cell.get("ips_per_thr", 0) / 1e6
                if fx == "ViperFixture" or not v_ips:
                    cols.append(f"{ips:13.2f}M")
                else:
                    cols.append(f"{ips:7.2f}({ips / v_ips:5.2f})")
            print(f"  {s:4d}M   " + "  ".join(cols))


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    data = parse_results()
    if not data:
        return
    for workload in WORKLOADS:
        if workload in data:
            print(f"Plotting {workload}…")
            plot_workload(workload, data[workload])
            plot_latency(workload, data[workload])
    print_summary(data)


if __name__ == "__main__":
    main()
