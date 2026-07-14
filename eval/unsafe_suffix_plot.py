#!/usr/bin/env python3
"""S5b: unsafe-suffix double panel — U distribution + T = alpha + beta*U fit.

Companion to recovery_vs_n (C3). With the O(P) lock scan gone (bounded-lock-set
protocol) tail replay dominates total recovery, so the O(unsafe suffix) claim
is directly measurable:

  (a) CDF of U = unsafe_suffix_blocks per crash workload. U is set by WRITE
      intensity at the moment of the kill: pure insert (t16) appends blocks at
      full speed; YCSB-A/B (t8) update in place — updates persist their value
      in the slot they hit and never move the block frontier, so U ~= 0-ish.
  (b) T_total vs U scatter, pooled linear fit T = alpha + beta*U:
      alpha = the cold-open floor (PM mmap + checkpoint read + hot alloc),
      beta  = per-block replay cost. Update-heavy crashes anchor alpha at
      U ~= 0; insert crashes span U and pin beta.

Input : results/recovery/unsafe_suffix/hiom_<wl>_rep<r>.csv
        (run_unsafe_suffix.sh; fresh-process cold samples, N=10M K8/V200)
Gate  : every sample must have lost==0 and mismatch==0 — violations abort the
        plot (exit 1), they are crash-consistency failures, not style.
Output: eval/charts/unsafe_suffix.{pdf,png}   (BW=1 -> paper/figures/)
"""
import csv
import glob
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

BW = os.environ.get("BW") == "1"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.normpath(os.path.join(
    SCRIPT_DIR, "..", "paper", "figures") if BW
    else os.path.join(SCRIPT_DIR, "charts"))

WL_STYLE = {
    "insert": {"label": "pure insert (t16)", "color": "0.0" if BW else "#d62728",
               "marker": "o"},
    "ycsb_b": {"label": "YCSB-B 5% upd (t8)", "color": "0.45" if BW else "#1f77b4",
               "marker": "s"},
    "ycsb_a": {"label": "YCSB-A 50% upd (t8)", "color": "0.7" if BW else "#2ca02c",
               "marker": "^"},
}


def load(in_dir):
    """-> {wl: [(U, T)]}, gate failures."""
    data, gate_fail = {}, []
    for path in sorted(glob.glob(os.path.join(in_dir, "hiom_*_rep*.csv"))):
        m = re.search(r"hiom_(insert|ycsb_a|ycsb_b)_rep(\d+)\.csv$",
                      os.path.basename(path))
        if not m:
            continue
        wl = m.group(1)
        with open(path) as f:
            rows = list(csv.DictReader(f))
        if not rows:
            continue
        r = rows[-1]
        if int(r.get("lost", "0")) != 0 or int(r.get("mismatch", "0")) != 0:
            gate_fail.append(f"{os.path.basename(path)}: lost={r['lost']} "
                             f"mismatch={r['mismatch']}")
            continue
        data.setdefault(wl, []).append(
            (float(r["unsafe_suffix_blocks"]), float(r["total_recovery_ms"])))
    return data, gate_fail


def main():
    in_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        SCRIPT_DIR, "..", "results", "recovery", "unsafe_suffix")
    in_dir = os.path.normpath(in_dir)
    if not os.path.isdir(in_dir):
        print(f"error: input dir not found: {in_dir}", file=sys.stderr)
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)

    data, gate_fail = load(in_dir)
    if gate_fail:
        print("GATE FAILURE — crash-consistency violations:", file=sys.stderr)
        for g in gate_fail:
            print("  " + g, file=sys.stderr)
        return 1
    if not data:
        print("error: no samples", file=sys.stderr)
        return 2

    fig, (axa, axb) = plt.subplots(1, 2, figsize=(13, 4.8))

    # (a) U CDF per workload (symlog x: update workloads sit at U ~= 0).
    for wl in ("insert", "ycsb_b", "ycsb_a"):
        if wl not in data:
            continue
        st = WL_STYLE[wl]
        us = np.sort([u for u, _ in data[wl]])
        cdf = np.arange(1, len(us) + 1) / len(us)
        axa.step(us, cdf, where="post", color=st["color"], lw=2,
                 marker=st["marker"], ms=5,
                 label=f"{st['label']}  (n={len(us)})")
    axa.set_xscale("symlog", linthresh=10)
    axa.set_xlabel("Unsafe suffix $U$ (blocks)")
    axa.set_ylabel("CDF over crash samples")
    axa.set_ylim(0, 1.02)
    axa.set_title("(a) unsafe suffix by crash workload\n"
                  "$U$ tracks write intensity; in-place updates leave $U\\approx 0$",
                  fontsize=11)
    axa.grid(alpha=0.3)
    axa.legend(fontsize=8, loc="lower right")

    # (b) T vs U scatter + pooled fit.
    all_u = np.array([u for pts in data.values() for u, _ in pts])
    all_t = np.array([t for pts in data.values() for _, t in pts])
    beta, alpha = np.polyfit(all_u, all_t, 1)
    r = np.corrcoef(all_u, all_t)[0, 1]
    for wl, pts in data.items():
        st = WL_STYLE[wl]
        axb.scatter([u for u, _ in pts], [t for _, t in pts], s=36,
                    color=st["color"], marker=st["marker"], alpha=0.8,
                    label=st["label"])
    xs = np.linspace(0, all_u.max() * 1.05, 100)
    axb.plot(xs, alpha + beta * xs, color="0.2", ls="--", lw=1.5,
             label=(f"fit $T = {alpha:.0f} + {beta:.3f}\\,U$ ms  "
                    f"($R^2$={r * r:.3f})"))
    axb.set_xlabel("Unsafe suffix $U$ (blocks)")
    axb.set_ylabel("Total recovery time $T$ (ms)")
    axb.set_title("(b) recovery cost model (fresh-cold, N=10M)\n"
                  "$\\alpha$ = open floor, $\\beta$ = per-block replay cost",
                  fontsize=11)
    axb.grid(alpha=0.3)
    axb.legend(fontsize=8, loc="upper left")

    plt.tight_layout()
    pdf = os.path.join(OUT_DIR, "unsafe_suffix.pdf")
    plt.savefig(pdf, bbox_inches="tight")
    if not BW:
        plt.savefig(os.path.join(OUT_DIR, "unsafe_suffix.png"), dpi=300,
                    bbox_inches="tight")
    print("wrote", pdf)

    print(f"\nfit: T = {alpha:.1f} + {beta:.4f}*U ms   R^2 = {r * r:.4f}")
    for wl, pts in sorted(data.items()):
        us = [u for u, _ in pts]
        ts = [t for _, t in pts]
        print(f"  {wl:7s} n={len(pts):3d}  U=[{min(us):.0f},{max(us):.0f}] "
              f"median {np.median(us):.0f}   T median {np.median(ts):.0f} ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
