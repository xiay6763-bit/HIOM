#!/usr/bin/env python3
"""
E2 iso-DRAM read figure (C2): four systems on one shared DRAM-budget axis.

  Left  (a) — HotTier hit rate vs DRAM budget   (thread-independent)
  Right (b) — read throughput vs DRAM budget    (t=1)

Story (C2 Pareto). Across the DRAM-budget interval [~8 MB, 2052 MB) the
DRAM-index camp (Viper / CCEH) is INFEASIBLE: their CCEH directory is
eager-preallocated to ~2052 MB @10M and cannot index the full keyspace in
less DRAM. The PM-resident camp (Dash) runs at any budget (index lives in PM,
DRAM ~0) but is slow. Only HiOM is BOTH feasible under a tight budget AND fast
-- at 272 MB (1/8 of Viper's DRAM) it already reaches 0.9996 hit rate /
Viper-class read throughput. The shaded region is exactly where the DRAM-index
camp cannot run; HiOM's whole curve and Dash's flat line live inside it.

Data provenance (10 M, K8/V200; all already on disk -> pure plotting):
  - HiOM curve (6 budgets 8.5..272 MB): results/hot_scan/summary.csv
      (t=1 capacity ablation: hit rate + items/s, zipf & uniform).
  - Baseline t=1 read tput (fixed points): results/thread_scaling/
      {Viper,Cceh,Dash}Fixture_100r_{zipf,uniform}_10M_tp.json, threads:1.
  - Index DRAM constants (white-box, same as footprint_plot.py SYS):
      Viper 2052, CCEH 2052, Dash ~2 MB.

Honesty caveats (kept explicit, matching HIOM.md's verified/derived nicety):
  - The "infeasible / OOM" region is ANALYTICAL (derived from the white-box
    index-DRAM floor), NOT a measured OOM crash.
  - The throughput panel is t=1: small HotTier capacities livelock in the read
    path at t>1 (HIOM.md:1825-1833), so the capacity curve is measured at t=1.
    Hit rate is thread-independent, so panel (a) is general.

Colours/markers match thread_scaling_plot.py (same paper, same systems).
Usage: python3 eval/iso_dram_plot.py
"""
import csv
import json
import os
import re

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

HOT_SCAN = "results/hot_scan"
TS = "results/thread_scaling"
# BW=1 → grayscale, written to paper/figures/ (B&W-printed journals).
BW = os.environ.get("BW") == "1"
OUT = "paper/figures/iso_dram.pdf" if BW else "eval/charts/iso_dram.pdf"

# White-box index DRAM (MB) -- identical to footprint_plot.py SYS.
DRAM_MB = {"Viper": 2052.0, "CCEH": 2052.0, "Dash": 2.0}
# DRAM-index camp floor: CCEH directory is eager-preallocated to ~2052 MB at
# 10 M and cannot index the full keyspace in less DRAM, so any budget below
# this is infeasible for Viper/CCEH (analytical bound, not a measured OOM).
DRAM_INDEX_FLOOR = 2052.0

if BW:
    STYLE = {
        "Viper": {"color": "0.0",  "marker": "o", "label": "Viper (DRAM-idx)"},
        "HiOM":  {"color": "0.0",  "marker": "s", "label": "HiOM"},
        "Dash":  {"color": "0.45", "marker": "^", "label": "Dash (PM-resident)"},
        "CCEH":  {"color": "0.45", "marker": "D", "label": "CCEH (DRAM-idx)"},
    }
else:
    STYLE = {
        "Viper": {"color": "#1f77b4", "marker": "o", "label": "Viper (DRAM-idx)"},
        "HiOM":  {"color": "#d62728", "marker": "s", "label": "HiOM"},
        "Dash":  {"color": "#2ca02c", "marker": "^", "label": "Dash (PM-resident)"},
        "CCEH":  {"color": "#9467bd", "marker": "D", "label": "CCEH (DRAM-idx)"},
    }
WL_LS = {"zipf": "-", "uniform": "--"}           # system = colour, workload = linestyle
WL_FROM_CSV = {"100r_zipf": "zipf", "100r_uniform": "uniform"}

XMIN, XMAX = 1.3, 4000.0
YMAX_TP = 3.4


def hiom_curve():
    """{workload: ([dram_mb], [hit], [ips_M])} from summary.csv, sorted by DRAM."""
    out = {"zipf": ([], [], []), "uniform": ([], [], [])}
    with open(os.path.join(HOT_SCAN, "summary.csv")) as f:
        for r in csv.DictReader(f):
            wl = WL_FROM_CSV.get(r["workload"])
            if not wl:
                continue
            out[wl][0].append(float(r["hot_tier_index_dram_mb"]))
            out[wl][1].append(float(r["hot_hit_rate"]))
            out[wl][2].append(float(r["items_per_second"]) / 1e6)
    for wl in out:
        z = sorted(zip(*out[wl]))
        out[wl] = tuple(list(c) for c in zip(*z)) if z else ([], [], [])
    return out


def baseline_t1():
    """{system: {workload: ips_M}} -- read tput at threads:1 (median row)."""
    fx_map = {"Viper": "ViperFixture", "Dash": "DashFixture", "CCEH": "CcehFixture"}
    out = {}
    for sys, fx in fx_map.items():
        out[sys] = {}
        for wl in ("zipf", "uniform"):
            p = os.path.join(TS, f"{fx}_100r_{wl}_10M_tp.json")
            if not os.path.exists(p):
                continue
            d = json.load(open(p))
            for b in d.get("benchmarks", []):
                if b.get("aggregate_name") != "median":
                    continue
                m = re.search(r"threads:(\d+)", b["name"])
                if m and int(m.group(1)) == 1:
                    out[sys][wl] = b.get("items_per_second", 0.0) / 1e6
    return out


def shade_oom(ax, ytop):
    """Shade the DRAM-index-camp-infeasible region + floor line + label."""
    ax.axvspan(XMIN, DRAM_INDEX_FLOOR, color="0.85", alpha=0.45, zorder=0)
    ax.axvline(DRAM_INDEX_FLOOR, color="0.4", ls=":", lw=1.5, zorder=1)
    gm = (XMIN * DRAM_INDEX_FLOOR) ** 0.5
    ax.text(gm, ytop,
            "Viper / CCEH infeasible here\n(DRAM index $\\approx$ 2052 MB @10M)",
            ha="center", va="top", fontsize=8.5, color="0.30", zorder=6)


def main():
    hiom = hiom_curve()
    base = baseline_t1()
    hred = STYLE["HiOM"]["color"]

    fig, (axL, axR) = plt.subplots(1, 2, figsize=(12, 4.8))

    # ---- (a) hit rate vs DRAM budget ----
    for wl in ("zipf", "uniform"):
        x, hit, _ = hiom[wl]
        axL.plot(x, hit, color=hred, marker="s", ms=6, lw=2.0, ls=WL_LS[wl],
                 zorder=4)
    shade_oom(axL, 0.60)
    axL.set_xscale("log")
    axL.set_xlim(XMIN, XMAX)
    axL.set_ylim(0, 1.03)
    axL.set_xlabel("DRAM budget (MB, log)")
    axL.set_ylabel("HotTier hit rate")
    axL.set_title("(a) hit rate vs DRAM budget (thread-independent)")
    axL.grid(True, alpha=0.3)
    axL.annotate("HiOM @272 MB:\n0.9996", xy=(272, 0.999),
                 xytext=(330, 0.62), fontsize=8.5, color=hred,
                 arrowprops=dict(arrowstyle="->", color=hred, lw=1.2))

    # ---- (b) read throughput vs DRAM budget (t=1) ----
    for wl in ("zipf", "uniform"):
        x, _, ips = hiom[wl]
        axR.plot(x, ips, color=hred, marker="s", ms=6, lw=2.0, ls=WL_LS[wl],
                 zorder=4)
    # Dash: PM-resident -> horizontal line from its ~2 MB DRAM across all budgets.
    for wl in ("zipf", "uniform"):
        v = base.get("Dash", {}).get(wl)
        if v is not None:
            axR.plot([DRAM_MB["Dash"], XMAX], [v, v], color=STYLE["Dash"]["color"],
                     ls=WL_LS[wl], lw=1.8, zorder=3)
            axR.plot([DRAM_MB["Dash"]], [v], color=STYLE["Dash"]["color"],
                     marker="^", ms=8, zorder=3)
    # Viper / CCEH: single feasible point at their 2052 MB index DRAM (filled=zipf).
    for sys in ("Viper", "CCEH"):
        for wl in ("zipf", "uniform"):
            v = base.get(sys, {}).get(wl)
            if v is None:
                continue
            mfc = STYLE[sys]["color"] if wl == "zipf" else "white"
            axR.plot([DRAM_MB[sys]], [v], color=STYLE[sys]["color"],
                     marker=STYLE[sys]["marker"], ms=9, mfc=mfc, mew=1.8, zorder=5)
    shade_oom(axR, YMAX_TP * 0.97)
    axR.set_xscale("log")
    axR.set_xlim(XMIN, XMAX)
    axR.set_ylim(0, YMAX_TP)
    axR.set_xlabel("DRAM budget (MB, log)")
    axR.set_ylabel("Read throughput (M items/s, t=1)")
    axR.set_title("(b) read throughput vs DRAM budget (t=1)")
    axR.grid(True, alpha=0.3)

    # ---- unified legend at the bottom (system colour + workload linestyle) ----
    sys_handles = [Line2D([0], [0], color=STYLE[s]["color"], marker=STYLE[s]["marker"],
                          ls="", ms=8, label=STYLE[s]["label"])
                   for s in ("HiOM", "Viper", "CCEH", "Dash")]
    wl_handles = [Line2D([0], [0], color="0.3", ls="-", lw=2, label="zipf ($\\theta$=0.99)"),
                  Line2D([0], [0], color="0.3", ls="--", lw=2, label="uniform")]
    fig.legend(handles=sys_handles + wl_handles, loc="lower center", ncol=6,
               fontsize=9, frameon=False, bbox_to_anchor=(0.5, -0.01))

    fig.suptitle("C2: iso-DRAM reads @10M (K8/V200) -- "
                 "only HiOM is feasible under a tight DRAM budget AND fast",
                 fontsize=12)
    fig.tight_layout(rect=[0, 0.07, 1, 0.95])
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fig.savefig(OUT)
    print("wrote", OUT)

    # ---- text table for cross-check / HIOM.md ----
    print("\nHiOM capacity curve (t=1, from summary.csv):")
    print(f"  {'DRAM(MB)':>9}{'hit_zipf':>10}{'hit_uni':>9}{'ips_z(M)':>10}{'ips_u(M)':>10}")
    for i, dram in enumerate(hiom["zipf"][0]):
        print(f"  {dram:>9.1f}{hiom['zipf'][1][i]:>10.4f}{hiom['uniform'][1][i]:>9.4f}"
              f"{hiom['zipf'][2][i]:>10.3f}{hiom['uniform'][2][i]:>10.3f}")
    print("\nBaseline fixed points (index DRAM, t=1 read tput M/s):")
    for sys in ("Viper", "CCEH", "Dash"):
        z = base.get(sys, {}).get("zipf", float("nan"))
        u = base.get(sys, {}).get("uniform", float("nan"))
        print(f"  {sys:6} DRAM={DRAM_MB[sys]:>7.0f} MB   zipf={z:.2f}  uniform={u:.2f}")


if __name__ == "__main__":
    main()
