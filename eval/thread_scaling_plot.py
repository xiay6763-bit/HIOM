#!/usr/bin/env python3
"""
Thread-scalability plot — aggregate throughput vs thread count for the
read-only (100r) workload, four systems (Meto/Viper-paper style).

x = threads (1/2/4/8/16/24), y = AGGREGATE throughput (Mops/s, all
threads combined). Two panels: zipf (5M ops) | uniform (100M ops).

IMPORTANT — throughput semantics: Google Benchmark's `items_per_second`
in a multi-threaded run is already the AGGREGATE rate (all threads), not
per-thread. (real_time is wall÷threads, so items_per_second × real_time
= per-thread ops.) We plot items_per_second/1e6 directly as Mops/s and
must NOT multiply by the thread count — matching the original author's
eval/full_ycsb_plot.py:69 (y-axis 0–20, peak ~14 Mops/s).

Story: HiOM (DRAM hot tier) leads Dash/CCEH (PM-resident) at low/mid
concurrency on offset-map hits, but is overtaken by Dash at t=24 — the
HotTier high-fan-in lookup contention that scopes HiOM's read win to
low/mid concurrency (a §7 limitation).

Data: results/thread_scaling/<Fixture>_100r_<wl>_10M_tp.json
      (benchmark/run_thread_scaling.sh).
Output: eval/charts/thread_scaling_100r.pdf
"""
import json
import os
import re
from collections import defaultdict
from glob import glob

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

RESULTS_DIR = "/root/viper/results/thread_scaling"
OUTPUT_DIR = "/root/viper/eval/charts"
THREADS = (1, 2, 4, 8, 16, 24)
WORKLOADS = (("100r_zipf", "100r zipf (5M ops, skewed)"),
             ("100r_uniform", "100r uniform (100M ops)"))
FIXTURES = ("ViperFixture", "HiOMFixture", "DashFixture", "CcehFixture")
STYLES = {
    "ViperFixture": {"color": "#1f77b4", "marker": "o", "ms": 7, "label": "Viper"},
    "HiOMFixture":  {"color": "#d62728", "marker": "s", "ms": 7, "label": "HiOM"},
    "DashFixture":  {"color": "#2ca02c", "marker": "^", "ms": 7, "label": "Dash (PM-resident)"},
    "CcehFixture":  {"color": "#9467bd", "marker": "D", "ms": 6, "label": "CCEH (DRAM-idx)"},
}

# Google Benchmark name like
#   HiOMFixture<KeyType8,ValueType200>/100r_zipf_tp/iterations:1/repeats:3/real_time/threads:8_median
NAME_RE = re.compile(
    r"^(?P<fixture>\w+Fixture)<[^>]+>/(?P<workload>\w+)_(?P<metric>tp|lat)/.+"
    r"threads:(?P<threads>\d+)(?:_(?P<agg>\w+))?$"
)
FNAME_RE = re.compile(
    r"(?P<fixture>\w+Fixture)_(?P<workload>100r_\w+)_(?P<size>\d+)M_tp\.json$"
)


def parse():
    """dict[workload][fixture][threads] = aggregate Mops/s (median of reps)."""
    data = defaultdict(lambda: defaultdict(dict))
    paths = sorted(glob(os.path.join(RESULTS_DIR, "*.json")))
    if not paths:
        print(f"No JSON in {RESULTS_DIR} — run benchmark/run_thread_scaling.sh first.")
        return data
    for path in paths:
        if not FNAME_RE.search(os.path.basename(path)):
            continue
        with open(path) as f:
            j = json.load(f)
        for bm in j.get("benchmarks", []):
            n = NAME_RE.match(bm["name"])
            if not n or n.group("agg") != "median" or n.group("metric") != "tp":
                continue
            wl = n.group("workload")
            fx = n.group("fixture")
            t = int(n.group("threads"))
            data[wl][fx][t] = bm.get("items_per_second", 0) / 1e6
    return data


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    data = parse()
    if not data:
        return

    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    for col, (wl, title) in enumerate(WORKLOADS):
        ax = axes[col]
        w = data.get(wl, {})
        for fx in FIXTURES:
            pts = w.get(fx, {})
            xs = [t for t in THREADS if t in pts]
            ys = [pts[t] for t in xs]
            if xs:
                st = STYLES[fx]
                ax.plot(xs, ys, color=st["color"], marker=st["marker"],
                        ms=st["ms"], lw=2.0, label=st["label"])
        ax.set_title(title, fontsize=12)
        ax.set_xlabel("Number of threads")
        ax.set_xticks(list(THREADS))
        ax.set_xlim(0, 25)
        ax.set_ylim(bottom=0)
        ax.grid(axis="y", alpha=0.3)
        if col == 0:
            ax.set_ylabel("Aggregate throughput (Mops/s)")
        ax.legend(fontsize=8, loc="upper left")

    fig.suptitle("Read-only (100r) throughput vs threads @10M (K8/V200)",
                 fontsize=13, fontweight="bold")
    plt.tight_layout(rect=[0, 0, 1, 0.96])
    out = os.path.join(OUTPUT_DIR, "thread_scaling_100r.pdf")
    plt.savefig(out)
    plt.close()
    print("wrote", out)

    # stdout summary table
    for wl, _ in WORKLOADS:
        w = data.get(wl, {})
        if not w:
            continue
        print(f"\n[{wl}] aggregate Mops/s")
        hdr = "".join(f"{'t='+str(t):>8}" for t in THREADS)
        print(f"  {'system':18}{hdr}")
        for fx in FIXTURES:
            if not w.get(fx):
                continue
            row = "".join(
                f"{w[fx][t]:8.1f}" if t in w[fx] else f"{'-':>8}"
                for t in THREADS)
            print(f"  {STYLES[fx]['label']:18}{row}")


if __name__ == "__main__":
    main()
