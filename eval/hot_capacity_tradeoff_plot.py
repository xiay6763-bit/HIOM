#!/usr/bin/env python3
"""S3 / E2: HotTier-capacity ablation — the 1x3 causal-chain figure.

  DRAM capacity -> hot_hit_rate -> cold-access fraction -> throughput

Input : results/hot_scan_v2/hiom_log2_<L>_<dist>.json  (scan_hot_capacity_v2.sh)
        results/frozen/ycsb_spectrum/{Viper,Dash}Fixture_100r_<dist>_10M_tp.json
        (t8 median reference lines — same frozen core, no rerun)
Output: eval/charts/hot_capacity_tradeoff.{pdf,png}

Panels (shared x = HotTier slots / working set, log2 scale):
  (a) hot_hit_rate            — median across the 3 reps' iteration rows
  (b) cold-access fraction    — 1 - hot_hit_rate (every hot miss / fp
                                collision falls through to the PM ColdTier)
  (c) throughput (Mops/s, t8) — the cell's `median` aggregate, with Viper /
                                Dash t8 reference hlines from the frozen
                                spectrum

Validity gate (same tiers as validate_frozen_run.py, enforced BEFORE
plotting): every iteration must have read_success_rate == 1.0 and
cold_miss_rate == 0.0; gate-breaching cells are reported and EXCLUDED.
Exit code 1 if any cell was excluded (CI-friendly), 2 on usage errors.
"""
import glob
import json
import os
import re
import sys

import matplotlib.pyplot as plt

# 中文标注:font.family 直接给列表才有逐字形回退;子图题置于子图下方
plt.rcParams["font.family"] = ["DejaVu Sans", "Noto Sans CJK JP"]
plt.rcParams["axes.unicode_minus"] = False

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(SCRIPT_DIR, ".."))
OUTPUT_DIR = os.path.join(SCRIPT_DIR, "charts")
SPECTRUM_DIR = os.path.join(REPO, "results", "frozen", "ycsb_spectrum")

WORKING_SET = 10_000_000
SLOTS_PER_BUCKET = 16
RS_MIN, CM_MAX = 1.0 - 1e-6, 1e-6

DIST_LBL = {"uniform": "Uniform", "zipf": "Zipf"}
DIST_STYLE = {
    "uniform": {"color": "#d62728", "marker": "o", "ls": "-",
                "label": "HiOM(Uniform)"},
    "zipf":    {"color": "#d62728", "marker": "*", "ls": "--", "ms": 11,
                "label": "HiOM(Zipf)"},
}
REF_STYLE = {
    ("Viper", "uniform"): {"color": "#1f77b4", "ls": "-"},
    ("Viper", "zipf"):    {"color": "#1f77b4", "ls": "--"},
    ("Dash", "uniform"):  {"color": "#ff7f0e", "ls": "-"},
    ("Dash", "zipf"):     {"color": "#ff7f0e", "ls": "--"},
}


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    if n == 0:
        return None
    return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])


def load_cell(path):
    """-> (log2, dist, hit_rate, tput_mops, gate_msgs)."""
    m = re.search(r"hiom_log2_(\d+)_(uniform|zipf)\.json$", path)
    if not m:
        return None
    log2, dist = int(m.group(1)), m.group(2)
    with open(path) as f:
        d = json.load(f)
    iters = [b for b in d.get("benchmarks", [])
             if b.get("run_type") == "iteration"]
    aggs = [b for b in d.get("benchmarks", [])
            if b.get("run_type") == "aggregate"
            and b.get("aggregate_name") == "median"]
    msgs = []
    if not iters or not aggs:
        msgs.append("missing iterations/median aggregate")
        return log2, dist, None, None, msgs
    for b in iters:
        rs, cm = b.get("read_success_rate"), b.get("cold_miss_rate")
        if rs is None or rs < RS_MIN:
            msgs.append(f"read_success_rate={rs}")
        if cm is None or cm > CM_MAX:
            msgs.append(f"cold_miss_rate={cm}")
    hit = median([b["hot_hit_rate"] for b in iters if "hot_hit_rate" in b])
    if hit is None:
        msgs.append("no hot_hit_rate in iterations")
    tput = aggs[0].get("items_per_second", 0) / 1e6
    return log2, dist, hit, tput, msgs


def spectrum_ref(system, dist, threads=8):
    p = os.path.join(SPECTRUM_DIR,
                     f"{system}Fixture_100r_{dist}_10M_tp.json")
    if not os.path.isfile(p):
        return None
    with open(p) as f:
        d = json.load(f)
    for b in d.get("benchmarks", []):
        if (b.get("run_type") == "aggregate"
                and b.get("aggregate_name") == "median"
                and int(b.get("threads", -1)) == threads):
            return b["items_per_second"] / 1e6
    return None


def main():
    in_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        REPO, "results", "hot_scan_v2")
    if not os.path.isdir(in_dir):
        print(f"error: input dir not found: {in_dir}", file=sys.stderr)
        return 2
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    series = {"uniform": [], "zipf": []}   # dist -> [(frac, hit, tput)]
    excluded = 0
    for path in sorted(glob.glob(os.path.join(in_dir, "hiom_log2_*.json"))):
        cell = load_cell(path)
        if cell is None:
            continue
        log2, dist, hit, tput, msgs = cell
        if msgs:
            excluded += 1
            print(f"  GATE-EXCLUDED log2={log2} {dist}: {'; '.join(msgs)}",
                  file=sys.stderr)
            continue
        frac = (1 << log2) * SLOTS_PER_BUCKET / WORKING_SET
        series[dist].append((frac, hit, tput))
        print(f"  ok log2={log2:2d} {dist:7s} slots/ws={frac:7.3f} "
              f"hit={hit:.4f} tput={tput:.2f} M/s")

    fig, axes = plt.subplots(1, 3, figsize=(16.5, 4.6))

    for dist in ("uniform", "zipf"):
        pts = sorted(series[dist])
        if not pts:
            continue
        xs = [p[0] for p in pts]
        st = dict(DIST_STYLE[dist])
        label = st.pop("label")
        axes[0].plot(xs, [p[1] for p in pts], label=label,
                     lw=2.2, markeredgewidth=1, **st)
        axes[1].plot(xs, [1.0 - p[1] for p in pts], label=label,
                     lw=2.2, markeredgewidth=1, **st)
        axes[2].plot(xs, [p[2] for p in pts], label=label,
                     lw=2.2, markeredgewidth=1, **st)

    # Reference hlines (t8 medians from the frozen spectrum). t=8 已写进
    # (c) 的子图题,图例只留 系统(分布)。
    for (system, dist), st in REF_STYLE.items():
        v = spectrum_ref(system, dist)
        if v is not None:
            axes[2].axhline(v, lw=1.6, alpha=0.85,
                            label=f"{system}({DIST_LBL[dist]})", **st)

    panels = [("(a) 热层命中率", "热层命中率"),
              ("(b) 冷层访问占比", "冷层访问占比"),
              ("(c) 读吞吐量(8线程)", "吞吐量(Mops/s)")]
    for ax, (subcap, ylab) in zip(axes, panels):
        ax.set_xscale("log", base=2)
        ax.set_xlabel(f"热层槽位数/工作集\n{subcap}", fontsize=12.5,
                      linespacing=1.7)
        ax.set_ylabel(ylab, fontsize=12.5)
        ax.tick_params(axis="both", labelsize=11.5)
        ax.margins(x=0.06)  # keep the 2^-5 / 2^2 end tick labels off the spines
        ax.grid(axis="y", linestyle="--", alpha=0.5)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
    axes[0].set_ylim(0, 1.02)
    axes[1].set_ylim(0, 1.02)
    axes[2].set_ylim(bottom=0)
    axes[0].legend(frameon=False, fontsize=10, loc="upper left")
    axes[2].legend(frameon=False, fontsize=9, loc="upper left", ncol=1)

    plt.tight_layout()
    pdf = os.path.join(OUTPUT_DIR, "hot_capacity_tradeoff.pdf")
    png = os.path.join(OUTPUT_DIR, "hot_capacity_tradeoff.png")
    plt.savefig(pdf, bbox_inches="tight")
    plt.savefig(png, dpi=300, bbox_inches="tight")
    print(f"wrote {pdf}\nwrote {png}")
    return 1 if excluded else 0


if __name__ == "__main__":
    sys.exit(main())
