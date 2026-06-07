#!/usr/bin/env python3
"""
Thread-scalability plots — aggregate throughput vs thread count, four
systems (Viper / HiOM / Dash / CCEH), Meto/Viper-paper style.

Emits ONE 1x2 figure (zipf | uniform panels) per workload family, all in
the same format so they read as a series across the read→write spectrum:

  read-only  100r    → eval/charts/thread_scaling_100r.pdf
  read-mostly YCSB-B (95/5) → eval/charts/thread_scaling_ycsb_b.pdf
  write-heavy YCSB-A (50/50) → eval/charts/thread_scaling_ycsb_a.pdf

x = threads (1/2/4/8/16/24), y = AGGREGATE throughput (Mops/s, all
threads combined). Each figure auto-scales its own y-axis, so A's lower
write throughput is not dwarfed by B/100r.

IMPORTANT — throughput semantics: Google Benchmark's `items_per_second`
in a multi-threaded run is already the AGGREGATE rate (all threads), not
per-thread. (real_time is wall÷threads, so items_per_second × real_time
= per-thread ops.) We plot items_per_second/1e6 directly as Mops/s and
must NOT multiply by the thread count.

Story (post per-Client-shard fix, 2026-06-07): HiOM matches/exceeds Viper
on read-only at ALL concurrencies (the earlier "overtaken by Dash at
t=24" wall was the contended stats fetch_add, now removed) and stays
≈0.8× Viper on read-mostly B while beating both PM-resident baselines.
On write-heavy A it pays a documented ~0.3–0.5× cost vs Viper (the
ColdTier durability mirror) — still ahead of Dash/CCEH.

Data: results/thread_scaling/<Fixture>_<workload>_10M_tp.json
      (benchmark/run_thread_scaling.sh).
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
FIXTURES = ("ViperFixture", "HiOMFixture", "DashFixture", "CcehFixture")
STYLES = {
    "ViperFixture": {"color": "#1f77b4", "marker": "o", "ms": 7, "label": "Viper"},
    "HiOMFixture":  {"color": "#d62728", "marker": "s", "ms": 7, "label": "HiOM"},
    "DashFixture":  {"color": "#2ca02c", "marker": "^", "ms": 7, "label": "Dash (PM-resident)"},
    "CcehFixture":  {"color": "#9467bd", "marker": "D", "ms": 6, "label": "CCEH (DRAM-idx)"},
}

# One figure per workload family.
#   stem      → output file thread_scaling_<stem>.pdf
#   suptitle  → bold figure title
#   panels    → [(workload_key, panel_title), ...] left-to-right
FAMILIES = (
    {"stem": "100r",
     "suptitle": "Read-only (100r) throughput vs threads @10M (K8/V200)",
     "panels": [("100r_zipf", "Zipfian (skewed)"),
                ("100r_uniform", "Uniform")]},
    {"stem": "ycsb_b",
     "suptitle": "YCSB-B (read-mostly, 95% read / 5% update) vs threads @10M (K8/V200)",
     "panels": [("b_zipf", "Zipfian (skewed)"),
                ("b_uniform", "Uniform")]},
    {"stem": "ycsb_a",
     "suptitle": "YCSB-A (write-heavy, 50% read / 50% update) vs threads @10M (K8/V200)",
     "panels": [("a_zipf", "Zipfian (skewed)"),
                ("a_uniform", "Uniform")]},
)

# Google Benchmark name like
#   HiOMFixture<KeyType8,ValueType200>/a_zipf_tp/iterations:1/repeats:3/real_time/threads:8_median
NAME_RE = re.compile(
    r"^(?P<fixture>\w+Fixture)<[^>]+>/(?P<workload>\w+)_(?P<metric>tp|lat)/.+"
    r"threads:(?P<threads>\d+)(?:_(?P<agg>\w+))?$"
)
FNAME_RE = re.compile(
    r"(?P<fixture>\w+Fixture)_(?P<workload>(?:100r|a|b)_(?:zipf|uniform))_(?P<size>\d+)M_tp\.json$"
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
        try:
            with open(path) as f:
                j = json.load(f)
        except (json.JSONDecodeError, OSError) as e:
            # A run may still be writing this file (partial JSON); skip it.
            print(f"  skip (unreadable/in-progress): {os.path.basename(path)} — {e}")
            continue
        for bm in j.get("benchmarks", []):
            n = NAME_RE.match(bm["name"])
            if not n or n.group("agg") != "median" or n.group("metric") != "tp":
                continue
            wl = n.group("workload")
            fx = n.group("fixture")
            t = int(n.group("threads"))
            data[wl][fx][t] = bm.get("items_per_second", 0) / 1e6
    return data


def plot_family(data, fam):
    """Render one 1x2 figure for a workload family; return True if it had data."""
    if not any(data.get(wl) for wl, _ in fam["panels"]):
        return False
    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    for col, (wl, title) in enumerate(fam["panels"]):
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

    fig.suptitle(fam["suptitle"], fontsize=13, fontweight="bold")
    plt.tight_layout(rect=[0, 0, 1, 0.96])
    out = os.path.join(OUTPUT_DIR, f"thread_scaling_{fam['stem']}.pdf")
    plt.savefig(out)
    plt.close()
    print("wrote", out)
    return True


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    data = parse()
    if not data:
        return

    for fam in FAMILIES:
        if not plot_family(data, fam):
            continue
        # stdout summary table
        for wl, _ in fam["panels"]:
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
