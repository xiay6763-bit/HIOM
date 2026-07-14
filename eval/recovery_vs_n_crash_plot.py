#!/usr/bin/env python3
"""S5a: recovery time vs dataset size N — fresh-process cold-vs-cold (C3 paper fig).

The honest K8/V200 recovery figure, both systems on the SAME
hiom_crash_recovery_bm harness under the re-frozen core (CORE=220fb60):

  HiOM  : real fork()+SIGKILL mid-insert (16 writer threads, kill delay
          5-200 ms — the S1/S2 gate methodology). Recovery = PM mmap open
          (viper_open, a ~150 ms floor) + tail replay of the cadence-bounded
          unsafe suffix (hiom_ctor, includes tail_scan). Lazy-repair lock
          path -> lock_scan=0. O(unsafe suffix), value-independent, ~constant
          in N (the suffix is set by write intensity at crash time, not N).
  Viper : clean-restart rebuild (its own paper's methodology). Recovery =
          recover_database, which cold-faults the pre-allocated ~2 GB CCEH
          directory (32 recovery threads) and O(N)-rehashes every record. A
          large N-independent cold-fault floor + O(N) growth.

Each point is the MEDIAN of RVN_REPS independent fresh processes (ITERS=1); a
long-lived parent warms heap/PM/threads and is a benchmark artifact (see
design/HIOM.md option-(a)). Every sample MUST have lost==0 and mismatch==0 (gate) — recovery that loses
acknowledged writes is not recovery. (The gate is not vacuous: viper 100M
clean-restart lost 2 keys in 1 of 4 samples — the pre-existing upstream
recover_database concurrent-CCEH-rebuild drop, see memory
hiom-viper-baseline-crash-recovery-race — quarantined and replaced.)

Input : results/recovery/vsn/<system>_<N>_rep<r>.csv  (run_recovery_vs_n.sh)
Output: eval/charts/recovery_vs_n.{pdf,png}  (BW -> paper/figures/)

Panel (a): log-log total recovery vs N, HiOM vs Viper, scissor annotated.
Panel (b): HiOM stage decomposition (PM-open floor vs tail replay) per N.
"""
import csv
import glob
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BW = os.environ.get("BW") == "1"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = (os.path.join(SCRIPT_DIR, "..", "paper", "figures") if BW
           else os.path.join(SCRIPT_DIR, "charts"))
OUT_DIR = os.path.normpath(OUT_DIR)


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    if n == 0:
        return None
    return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])


def load_dir(in_dir):
    """-> {system: {N: [rows...]}}, with gate enforcement."""
    data = {}
    gate_fail = []
    for path in sorted(glob.glob(os.path.join(in_dir, "*.csv"))):
        m = re.search(r"(hiom|viper)_(\d+)_rep(\d+)\.csv$", os.path.basename(path))
        if not m:
            continue
        sysname, N, rep = m.group(1), int(m.group(2)), int(m.group(3))
        with open(path) as f:
            rows = list(csv.DictReader(f))
        if not rows:
            continue
        r = rows[-1]  # ITERS=1 -> one data row
        lost = int(r.get("lost", "0"))
        mismatch = int(r.get("mismatch", "0"))
        if lost != 0 or mismatch != 0:
            gate_fail.append(f"{os.path.basename(path)}: lost={lost} mismatch={mismatch}")
            continue
        data.setdefault(sysname, {}).setdefault(N, []).append(r)
    return data, gate_fail


def stage(rows, col):
    return median([float(r[col]) for r in rows])


def main():
    in_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        SCRIPT_DIR, "..", "results", "recovery", "vsn")
    in_dir = os.path.normpath(in_dir)
    if not os.path.isdir(in_dir):
        print(f"error: input dir not found: {in_dir}", file=sys.stderr)
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)

    data, gate_fail = load_dir(in_dir)
    if gate_fail:
        print("GATE FAILURE — samples lost acknowledged writes:", file=sys.stderr)
        for g in gate_fail:
            print("  " + g, file=sys.stderr)
        return 1
    if "hiom" not in data or "viper" not in data:
        print(f"error: need both systems; got {list(data)}", file=sys.stderr)
        return 2

    Ns = sorted(set(data["hiom"]) & set(data["viper"]))
    if not Ns:
        print("error: no common N between systems", file=sys.stderr)
        return 2

    hiom_total = [stage(data["hiom"][n], "total_recovery_ms") for n in Ns]
    viper_total = [stage(data["viper"][n], "total_recovery_ms") for n in Ns]
    hiom_open = [stage(data["hiom"][n], "viper_open_ms") for n in Ns]
    hiom_ctor = [stage(data["hiom"][n], "hiom_ctor_ms") for n in Ns]
    hiom_suffix = [median([int(r["unsafe_suffix_blocks"]) for r in data["hiom"][n]])
                   for n in Ns]

    c_hiom = "0.0" if BW else "#990000"
    c_viper = "0.45" if BW else "#000099"
    Nm = [n / 1e6 for n in Ns]

    fig, (axa, axb) = plt.subplots(1, 2, figsize=(13, 4.8))

    # Panel (a): scissor.
    axa.plot(Nm, hiom_total, ls="-.", marker="D", ms=7, color=c_hiom,
             label="HiOM — O(unsafe suffix), real SIGKILL")
    axa.plot(Nm, viper_total, ls="--", marker="s", ms=7, color=c_viper,
             label="Viper — O(N) rebuild, clean restart")
    for x, h, v in zip(Nm, hiom_total, viper_total):
        axa.annotate(f"{v / h:.1f}×", xy=(x, v), xytext=(0, 6),
                     textcoords="offset points", ha="center", fontsize=8,
                     color=c_viper)
    axa.set_xscale("log")
    axa.set_yscale("log")
    axa.set_xlabel("Dataset size $N$ (millions)")
    axa.set_ylabel("Cold-start recovery time (ms)")
    axa.set_title("(a) recovery vs $N$ (K8/V200, fresh cold, median)\n"
                  "O(unsafe suffix) vs O(N) — scissor widens with $N$", fontsize=11)
    axa.set_xticks(Nm)
    axa.set_xticklabels([f"{n:g}" for n in Nm])
    axa.minorticks_off()
    axa.grid(True, which="major", alpha=0.3)
    axa.legend(fontsize=8, loc="upper left")

    # Panel (b): HiOM stage decomposition, stacked bars per N.
    import numpy as np
    x = np.arange(len(Ns))
    c_open = "0.7" if BW else "#4c72b0"
    c_replay = "0.3" if BW else "#dd8452"
    axb.bar(x, hiom_open, 0.6, color=c_open, label="PM mmap open (floor)")
    axb.bar(x, hiom_ctor, 0.6, bottom=hiom_open, color=c_replay,
            label="tail replay (unsafe suffix)")
    for i in range(len(Ns)):
        axb.annotate(f"{hiom_total[i]:.0f} ms\nsuffix {hiom_suffix[i]:.0f} blk",
                     xy=(x[i], hiom_open[i] + hiom_ctor[i]), xytext=(0, 4),
                     textcoords="offset points", ha="center", fontsize=7.5)
    axb.set_xticks(x)
    axb.set_xticklabels([f"{n / 1e6:g}M" for n in Ns])
    axb.set_xlabel("Dataset size $N$")
    axb.set_ylabel("HiOM recovery time (ms)")
    axb.set_title("(b) HiOM stage decomposition (lazy-repair, lock_scan=0)\n"
                  "open floor ~constant; replay tracks the cadence-bounded suffix",
                  fontsize=11)
    axb.grid(True, axis="y", alpha=0.3)
    axb.legend(fontsize=8, loc="upper left")

    plt.tight_layout()
    pdf = os.path.join(OUT_DIR, "recovery_vs_n.pdf")
    png = os.path.join(OUT_DIR, "recovery_vs_n.png")
    plt.savefig(pdf, bbox_inches="tight")
    if not BW:
        plt.savefig(png, dpi=300, bbox_inches="tight")
    print("wrote", pdf)

    print(f"\n{'N(M)':>6}{'HiOM.ms':>9}{'Viper.ms':>10}{'ratio':>8}"
          f"{'reps(H/V)':>11}{'suffix':>8}")
    for i, n in enumerate(Ns):
        print(f"{n / 1e6:>6g}{hiom_total[i]:>9.0f}{viper_total[i]:>10.0f}"
              f"{viper_total[i] / hiom_total[i]:>7.1f}×"
              f"{len(data['hiom'][n]):>6}/{len(data['viper'][n]):<4}"
              f"{hiom_suffix[i]:>8.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
