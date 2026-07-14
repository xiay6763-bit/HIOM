#!/usr/bin/env python3
# YCSB workload-spectrum figure (paper main throughput figure).
#
# 2×3 grid, reusing the paper's existing YCSB plot conventions
# (eval/full_ycsb_plot.py: STYLES colors/markers, y-grid, hidden top/right
# spines, shared bottom legend, throughput in Mops/s):
#   rows    = distribution  (uniform, zipf)
#   columns = workload      (YCSB-C = 100r, YCSB-B = b, YCSB-A = a)
#   each panel = throughput (Mops/s) vs threads {1,2,4,8,16,24}, one line per
#                system {HiOM, Viper, Dash, CCEH}.
#
# Input: the per-cell JSONs written by benchmark/run_frozen_ycsb.sh,
#   <in_dir>/<Fixture>_<workload>_<dist>_10M_tp.json
# (default in_dir = results/frozen/ycsb_spectrum). Uses the benchmark's
# 'median' aggregate row per thread point. Cells listed in invalid/ (or absent)
# are skipped with a warning — run eval/validate_frozen_run.py first and do NOT
# plot cells it flags.
#
# Output: eval/charts/ycsb_workload_spectrum.{pdf,png}

import json
import os
import re
import sys
from collections import defaultdict

import matplotlib.pyplot as plt

# Anchored to this script's location so output lands in eval/charts/ (and the
# default input resolves) regardless of the caller's cwd — running from the
# repo root used to silently create a stray /root/viper/charts/.
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = os.path.join(SCRIPT_DIR, "charts")
DEFAULT_IN_DIR = os.path.normpath(
    os.path.join(SCRIPT_DIR, "..", "results", "frozen", "ycsb_spectrum"))
MILLION = 1000000.0

# System legend + styles — SAME palette/markers as full_ycsb_plot.py, with HiOM
# added as the proposed system (red star, drawn last so it sits on top).
FIXTURES = [
    ("HiOMFixture", "HiOM"),
    ("ViperFixture", "Viper"),
    ("DashFixture", "Dash"),
    ("CcehFixture", "CCEH"),
]
STYLES = {
    "HiOM":  {"color": "#d62728", "marker": "*", "ms": 11},  # 红色 星形
    "Viper": {"color": "#1f77b4", "marker": "o", "ms": 8},   # 蓝色 圆点
    "Dash":  {"color": "#ff7f0e", "marker": "s", "ms": 8},   # 橙色 方块
    "CCEH":  {"color": "#2ca02c", "marker": "^", "ms": 8},   # 绿色 三角
}

# Grid axes. Rows = distributions, columns = workloads.
DISTS = [("uniform", "Uniform"), ("zipf", "Zipfian")]
WORKLOADS = [("100r", "YCSB-C (100% read)"),
             ("b", "YCSB-B (95/5 read/update)"),
             ("a", "YCSB-A (50/50 read/update)")]
THREADS = [1, 2, 4, 8, 16, 24]


def hide_border(ax):
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


def cell_path(in_dir, fixture, workload, dist):
    return os.path.join(in_dir, f"{fixture}_{workload}_{dist}_10M_tp.json")


def load_cell_median(path):
    """Return {threads: throughput_mops} from a cell's median aggregate rows."""
    if not os.path.isfile(path):
        return None
    try:
        with open(path) as f:
            d = json.load(f)
    except Exception as e:  # noqa: BLE001
        print(f"  WARN: cannot parse {path}: {e}", file=sys.stderr)
        return None
    out = {}
    for b in d.get("benchmarks", []):
        if b.get("run_type") != "aggregate":
            continue
        if b.get("aggregate_name") != "median":
            continue
        t = b.get("threads")
        ips = b.get("items_per_second")
        if t is None or ips is None:
            continue
        out[int(t)] = ips / MILLION
    return out or None


def main():
    in_dir = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_IN_DIR
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    if not os.path.isdir(in_dir):
        print(f"error: input dir not found: {in_dir}", file=sys.stderr)
        return 2

    nrows, ncols = len(DISTS), len(WORKLOADS)
    fig, axes = plt.subplots(nrows, ncols, figsize=(18, 9))

    for r, (dist_key, dist_label) in enumerate(DISTS):
        for c, (wl_key, wl_label) in enumerate(WORKLOADS):
            ax = axes[r][c]
            for fixture, label in FIXTURES:
                series = load_cell_median(
                    cell_path(in_dir, fixture, wl_key, dist_key))
                if not series:
                    print(f"  skip {fixture} {wl_key}_{dist_key} (no data)")
                    continue
                xs = [t for t in THREADS if t in series]
                ys = [series[t] for t in xs]
                if not xs:
                    continue
                ax.plot(xs, ys, label=label, **STYLES[label],
                        markeredgewidth=1, lw=2.5)

            # Column titles only on the top row; row (distribution) labels on
            # the first column.
            if r == 0:
                ax.set_title(wl_label, fontsize=15)
            if c == 0:
                ax.set_ylabel(f"{dist_label}\nThroughput (Mops/s)", fontsize=14)
            ax.set_xticks(THREADS)
            ax.set_xlim(0, 25)
            ax.set_ylim(bottom=0)
            ax.grid(axis="y", linestyle="--", alpha=0.5)
            hide_border(ax)
            ax.tick_params(axis="both", labelsize=11)

    fig.text(0.5, 0.045, "Number of Threads", ha="center", fontsize=17)

    # Shared legend (dedup), same placement idiom as full_ycsb_plot.py.
    handles, labels = axes[0][0].get_legend_handles_labels()
    by_label = dict(zip(labels, handles))
    fig.legend(by_label.values(), by_label.keys(),
               loc="upper center", bbox_to_anchor=(0.5, 0.99),
               ncol=len(FIXTURES), frameon=False, fontsize=15)

    plt.tight_layout(rect=[0, 0.06, 1, 0.94])
    pdf = os.path.join(OUTPUT_DIR, "ycsb_workload_spectrum.pdf")
    png = os.path.join(OUTPUT_DIR, "ycsb_workload_spectrum.png")
    plt.savefig(pdf, format="pdf", bbox_inches="tight")
    plt.savefig(png, format="png", bbox_inches="tight", dpi=300)
    print(f"wrote {pdf}\nwrote {png}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
