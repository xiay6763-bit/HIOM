#!/usr/bin/env python3
"""
Thread-scalability plots — four systems (Viper / HiOM / Dash / CCEH),
Meto/Viper-paper style.

Two metric modes, both driven from results/thread_scaling/. Running this
script renders every (family, metric) for which JSON exists on disk, so a
tp-only tree behaves exactly as before, and a tree that also has _lat.json
additionally gets the latency figures:

  metric=tp  → AGGREGATE throughput (Mops/s) vs threads
               → eval/charts/thread_scaling_<stem>.pdf
  metric=lat → read-latency percentiles (ns, log y) vs threads
               → eval/charts/thread_scaling_<stem>_lat.pdf
               (p99 solid+marker, p50 dashed; from the _lat HDR histograms)

Each figure is ONE 1x2 (zipf | uniform) per workload family, all in the
same format so they read as a series across the read→write spectrum:

  read-only   100r          → thread_scaling_100r{,_lat}.pdf
  read-mostly YCSB-B (95/5) → thread_scaling_ycsb_b{,_lat}.pdf
  write-heavy YCSB-A (50/50)→ thread_scaling_ycsb_a{,_lat}.pdf

throughput semantics: Google Benchmark's `items_per_second` in a
multi-threaded run is already the AGGREGATE rate (all threads), not
per-thread. Plot items_per_second/1e6 as Mops/s; do NOT multiply by the
thread count.

latency semantics: the _lat runs record each op's service time into HDR
histograms (ns; hdr_init range 1..1e9). We read the *read*-path
percentiles (hdr_read_median/99/999) off the per-run median aggregate —
the read path is HiOM's headline, and for 100r read==all. p99 is the tail,
p50 the typical hit. PM-resident Dash/CCEH pay a PM random read per
lookup, so they sit far above HiOM/Viper whose hot offset lives in DRAM —
the latency mirror of the read-throughput win. NB: lat-mode throughput is
lower than tp-mode (per-op HDR/clock cost), so latency *values* are read
from _lat runs and throughput from _tp runs; the two are not mixed.

Data: results/thread_scaling/<Fixture>_<workload>_10M_<tp|lat>.json
      (benchmark/run_thread_scaling.sh; TS_METRICS="tp lat").
"""
import json
import os
import re
from collections import defaultdict
from glob import glob

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

# BW=1 → grayscale, line-style + marker differentiated, written to paper/figures/
# (for B&W-printed Chinese journals). Default = colour into eval/charts/.
BW = os.environ.get("BW") == "1"
RESULTS_DIR = "/root/viper/results/thread_scaling"
OUTPUT_DIR = "/root/viper/paper/figures" if BW else "/root/viper/eval/charts"
THREADS = (1, 2, 4, 8, 16, 24)
FIXTURES = ("ViperFixture", "HiOMFixture", "DashFixture", "CcehFixture")
if BW:
    # HiOM = hero (solid black); others by distinct shade + linestyle + marker.
    STYLES = {
        "ViperFixture": {"color": "0.0",  "ls": "--", "marker": "o", "ms": 7, "mfc": "white", "label": "Viper"},
        "HiOMFixture":  {"color": "0.0",  "ls": "-",  "marker": "s", "ms": 7, "mfc": "0.0",   "label": "HiOM"},
        "DashFixture":  {"color": "0.45", "ls": ":",  "marker": "^", "ms": 7, "mfc": "white", "label": "Dash (PM-resident)"},
        "CcehFixture":  {"color": "0.45", "ls": "-.", "marker": "D", "ms": 6, "mfc": "0.45",  "label": "CCEH (DRAM-idx)"},
    }
else:
    STYLES = {
        "ViperFixture": {"color": "#1f77b4", "ls": "-", "marker": "o", "ms": 7, "mfc": "#1f77b4", "label": "Viper"},
        "HiOMFixture":  {"color": "#d62728", "ls": "-", "marker": "s", "ms": 7, "mfc": "#d62728", "label": "HiOM"},
        "DashFixture":  {"color": "#2ca02c", "ls": "-", "marker": "^", "ms": 7, "mfc": "#2ca02c", "label": "Dash (PM-resident)"},
        "CcehFixture":  {"color": "#9467bd", "ls": "-", "marker": "D", "ms": 6, "mfc": "#9467bd", "label": "CCEH (DRAM-idx)"},
    }

# One figure per workload family.
FAMILIES = (
    {"stem": "100r",
     "suptitle_tp":  "Read-only (100r) throughput vs threads @10M (K8/V200)",
     "suptitle_lat": "Read-only (100r) read latency vs threads @10M (K8/V200)",
     "panels": [("100r_zipf", "Zipfian (skewed)"),
                ("100r_uniform", "Uniform")]},
    {"stem": "ycsb_b",
     "suptitle_tp":  "YCSB-B (read-mostly, 95% read / 5% update) vs threads @10M (K8/V200)",
     "suptitle_lat": "YCSB-B (read-mostly) read latency vs threads @10M (K8/V200)",
     "panels": [("b_zipf", "Zipfian (skewed)"),
                ("b_uniform", "Uniform")]},
    {"stem": "ycsb_a",
     "suptitle_tp":  "YCSB-A (write-heavy, 50% read / 50% update) vs threads @10M (K8/V200)",
     "suptitle_lat": "YCSB-A (write-heavy) read latency vs threads @10M (K8/V200)",
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
    r"(?P<fixture>\w+Fixture)_(?P<workload>(?:100r|a|b)_(?:zipf|uniform))"
    r"_(?P<size>\d+)M_(?P<metric>tp|lat)\.json$"
)


def _lat_percentiles(bm):
    """Read-path p50/p99/p999 (ns) off a benchmark row; fall back to the
    aggregate histogram (read==all for a 100% read workload)."""
    def g(*keys):
        for k in keys:
            if k in bm:
                return bm[k]
        return None
    return {
        "p50":  g("hdr_read_median", "hdr_median"),
        "p99":  g("hdr_read_99", "hdr_99"),
        "p999": g("hdr_read_999", "hdr_999"),
    }


def parse(metric):
    """dict[workload][fixture][threads] = value (median of reps).
    tp  → aggregate Mops/s (float);  lat → {p50,p99,p999} ns (dict)."""
    data = defaultdict(lambda: defaultdict(dict))
    paths = sorted(glob(os.path.join(RESULTS_DIR, f"*_{metric}.json")))
    for path in paths:
        m = FNAME_RE.search(os.path.basename(path))
        if not m or m.group("metric") != metric:
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
            if not n or n.group("agg") != "median" or n.group("metric") != metric:
                continue
            wl, fx, t = n.group("workload"), n.group("fixture"), int(n.group("threads"))
            if metric == "tp":
                data[wl][fx][t] = bm.get("items_per_second", 0) / 1e6
            else:
                data[wl][fx][t] = _lat_percentiles(bm)
    return data


def plot_family_tp(data, fam):
    """1x2 throughput-vs-threads figure; return True if it had data."""
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
                        ms=st["ms"], mfc=st.get("mfc", st["color"]),
                        ls=st.get("ls", "-"), lw=2.0, label=st["label"])
        ax.set_title(title, fontsize=12)
        ax.set_xlabel("Number of threads")
        ax.set_xticks(list(THREADS))
        ax.set_xlim(0, 25)
        ax.set_ylim(bottom=0)
        ax.grid(axis="y", alpha=0.3)
        if col == 0:
            ax.set_ylabel("Aggregate throughput (Mops/s)")
        ax.legend(fontsize=8, loc="upper left")
    fig.suptitle(fam["suptitle_tp"], fontsize=13, fontweight="bold")
    plt.tight_layout(rect=[0, 0, 1, 0.96])
    out = os.path.join(OUTPUT_DIR, f"thread_scaling_{fam['stem']}.pdf")
    plt.savefig(out)
    plt.close()
    print("wrote", out)
    return True


def plot_family_lat(data, fam):
    """1x2 read-latency-vs-threads figure (log y; p99 solid, p50 dashed)."""
    if not any(data.get(wl) for wl, _ in fam["panels"]):
        return False
    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    for col, (wl, title) in enumerate(fam["panels"]):
        ax = axes[col]
        w = data.get(wl, {})
        for fx in FIXTURES:
            pts = w.get(fx, {})
            xs = [t for t in THREADS if t in pts and pts[t].get("p99") is not None]
            if not xs:
                continue
            st = STYLES[fx]
            ax.plot(xs, [pts[t]["p99"] for t in xs], color=st["color"],
                    marker=st["marker"], ms=st["ms"], mfc=st.get("mfc", st["color"]),
                    ls=st.get("ls", "-"), lw=2.0, label=st["label"])
            p50 = [pts[t].get("p50") for t in xs]
            if all(v is not None for v in p50):
                ax.plot(xs, p50, color=st["color"], lw=1.3,
                        ls=st.get("ls", "-"), alpha=0.45)
        ax.set_title(title, fontsize=12)
        ax.set_xlabel("Number of threads")
        ax.set_xticks(list(THREADS))
        ax.set_xlim(0, 25)
        ax.set_yscale("log")
        ax.grid(axis="y", which="both", alpha=0.3)
        if col == 0:
            ax.set_ylabel("Read latency (ns, log scale)")
        if ax.get_legend_handles_labels()[0]:
            leg1 = ax.legend(fontsize=8, loc="upper left")
            ax.add_artist(leg1)
            ax.legend(handles=[Line2D([0], [0], color="0.3", lw=2.0, label="p99"),
                               Line2D([0], [0], color="0.3", lw=1.3, ls="--", label="p50")],
                      fontsize=7, loc="lower right")
    fig.suptitle(fam["suptitle_lat"], fontsize=13, fontweight="bold")
    plt.tight_layout(rect=[0, 0, 1, 0.96])
    out = os.path.join(OUTPUT_DIR, f"thread_scaling_{fam['stem']}_lat.pdf")
    plt.savefig(out)
    plt.close()
    print("wrote", out)
    return True


def table_tp(data, fam):
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
            row = "".join(f"{w[fx][t]:8.1f}" if t in w[fx] else f"{'-':>8}"
                          for t in THREADS)
            print(f"  {STYLES[fx]['label']:18}{row}")


def table_lat(data, fam):
    for wl, _ in fam["panels"]:
        w = data.get(wl, {})
        if not w:
            continue
        print(f"\n[{wl}] read latency ns  (p50 / p99)")
        hdr = "".join(f"{'t='+str(t):>14}" for t in THREADS)
        print(f"  {'system':18}{hdr}")
        for fx in FIXTURES:
            if not w.get(fx):
                continue
            cells = []
            for t in THREADS:
                d = w[fx].get(t)
                if d and d.get("p50") is not None and d.get("p99") is not None:
                    cells.append(f"{int(d['p50']):>6}/{int(d['p99']):<7}")
                else:
                    cells.append(f"{'-':>14}")
            print(f"  {STYLES[fx]['label']:18}{''.join(cells)}")


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    any_data = False
    for metric, plot_fn, table_fn in (("tp", plot_family_tp, table_tp),
                                       ("lat", plot_family_lat, table_lat)):
        data = parse(metric)
        if not data:
            continue
        any_data = True
        print(f"\n===== metric = {metric} =====")
        for fam in FAMILIES:
            if plot_fn(data, fam):
                table_fn(data, fam)
    if not any_data:
        print(f"No JSON in {RESULTS_DIR} — run benchmark/run_thread_scaling.sh first.")


if __name__ == "__main__":
    main()
