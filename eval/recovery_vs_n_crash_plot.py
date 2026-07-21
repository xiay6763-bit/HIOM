#!/usr/bin/env python3
"""S5a: recovery time vs dataset size N — fresh-process cold-vs-cold (C3 paper fig).

The honest K8/V200 recovery figure. HiOM and Viper share the
hiom_crash_recovery_bm harness under the re-frozen core (CORE=220fb60); Halo is
measured on its own external clone (benchmark/run_halo_recovery.sh) under the
SAME fresh-process cold-reopen methodology as Viper's clean-restart line:

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
  Halo  : clean-restart snapshot restore (HNUSystemsLab/Halo). Child prefills +
          clean-closes (~Halo() memcpy-snapshots every CLHT bucket to PM,
          ROOT->clean=1); the parent cold-reopens and Halo's startup() restores
          the O(N) bucket snapshot (redo-log SKIPPED on clean). A shallower O(N)
          than Viper (bulk PM->DRAM memcpy vs per-record rehash) — the closest
          competitor, so the honest number to beat. This is Halo's BEST case
          (clean snapshot, no torn tail), conservative w.r.t. HiOM's advantage.


Each point is the MEDIAN of RVN_REPS independent fresh processes (ITERS=1); a
long-lived parent warms heap/PM/threads and is a benchmark artifact (see
design/HIOM.md option-(a)). Every sample MUST have lost==0 and mismatch==0 (gate) — recovery that loses
acknowledged writes is not recovery. (The gate is not vacuous: viper 100M
clean-restart lost 2 keys in 1 of 4 samples — the pre-existing upstream
recover_database concurrent-CCEH-rebuild drop, see memory
hiom-viper-baseline-crash-recovery-race — quarantined and replaced.)

Input : results/recovery/vsn/<system>_<N>_rep<r>.csv
        (hiom [SIGKILL] + hiomclean [HIOM_CR_HIOM_CLEAN=1]: run via
        hiom_crash_recovery_bm; halo: run_halo_recovery.sh; viper: clean)
Output: eval/charts/recovery_vs_n.{pdf,png}  (BW -> paper/figures/)

Panel (a): matched clean restart (HiOM/Halo/Viper, apples-to-apples) plus HiOM's
  real-SIGKILL line. Halo and Viper are shown at their clean BEST case — neither
  demonstrates crash survival; only HiOM carries a real-crash line (lost=0), so
  the two HiOM curves bracket "matched-condition speed" and "bounded under a real
  crash". Ratios annotate Viper/Halo over HiOM-clean (the matched denominator).
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

# 中文标注:font.family 直接给列表才有逐字形回退;子图题置于子图下方
plt.rcParams["font.family"] = ["DejaVu Sans", "Noto Sans CJK JP"]
plt.rcParams["axes.unicode_minus"] = False

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
        m = re.search(r"(hiomclean|hiom|viper|halo)_(\d+)_rep(\d+)\.csv$",
                      os.path.basename(path))
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

    c_hiom = "0.0" if BW else "#d62728"
    c_viper = "0.45" if BW else "#1f77b4"
    c_halo = "0.7" if BW else "#2ca02c"
    Nm = [n / 1e6 for n in Ns]

    # HiOM clean-restart (matched-condition reference vs the clean baselines) and
    # Halo clean-restart — optional lines at whatever N they were measured for.
    hc_Ns = sorted(set(data.get("hiomclean", {})) & set(Ns))
    hc_Nm = [n / 1e6 for n in hc_Ns]
    # HiOM clean-restart open floor is a fixed mmap of the (N-independent) Viper
    # pool (skip_recovery: no populate/recover; RSS ~16 MB regardless of N), so
    # it is ~O(1). N=10M reproducibly sits ~40 ms above the other N (verified
    # isolated, first-run, 10 reps × 2 sessions; not THP — dax MAP_SYNC has zero
    # huge pages — and not run-order); the residual is left unattributed. It is
    # negligible against the O(N) baseline lines, so all lines use median for
    # 口径一致 and no special-casing.
    hc_total = [stage(data["hiomclean"][n], "total_recovery_ms") for n in hc_Ns]
    hc_by_n = dict(zip(hc_Ns, hc_total))
    halo_Ns = sorted(set(data.get("halo", {})) & set(Ns))
    halo_Nm = [n / 1e6 for n in halo_Ns]
    halo_total = [stage(data["halo"][n], "total_recovery_ms") for n in halo_Ns]
    # 倍数(console 表)以 HiOM clean 为分母(同为正常重启,同口径);
    # clean 缺样时退回 HiOM SIGKILL。图内不再标注倍数,正文引用 console 表。

    fig, (axa, axb) = plt.subplots(1, 2, figsize=(13, 4.8))

    # Panel (a): matched clean restart (HiOM/Halo/Viper) + HiOM's real-crash line.
    # Halo/Viper are shown at their clean best case — neither demonstrates crash
    # survival; only HiOM has a real-SIGKILL line (lost=0), the dash-dot overlay.
    # 图例只留 系统(重启条件);机制与倍数正文已述,不在图内标注。
    if hc_Ns:
        axa.plot(hc_Nm, hc_total, ls="-", marker="D", ms=7, color=c_hiom,
                 label="HiOM(正常重启)")
    axa.plot(Nm, hiom_total, ls="-.", marker="D", ms=7, mfc="none", color=c_hiom,
             label="HiOM(真实崩溃)")
    if halo_Ns:
        axa.plot(halo_Nm, halo_total, ls=":", marker="^", ms=7, color=c_halo,
                 label="Halo(正常重启)")
    axa.plot(Nm, viper_total, ls="--", marker="s", ms=7, color=c_viper,
             label="Viper(正常重启)")
    axa.set_xscale("log")
    axa.set_yscale("log")
    axa.set_xlabel("数据规模 N(百万)\n(a) 恢复时间随 N 变化",
                   linespacing=1.8)
    axa.set_ylabel("恢复时间(ms)")
    axa.set_xticks(Nm)
    axa.set_xticklabels([f"{n:g}" for n in Nm])
    axa.minorticks_off()
    axa.grid(True, which="major", alpha=0.5, linestyle="--")
    axa.spines["top"].set_visible(False)
    axa.spines["right"].set_visible(False)
    axa.legend(fontsize=8.5, loc="upper left", frameon=False)

    # Panel (b): HiOM stage decomposition, stacked bars per N. 柱顶只标总时长
    # 单行数字(suffix 块数见图6/正文),并留出表头空间避免文字顶到图框。
    import numpy as np
    x = np.arange(len(Ns))
    c_open = "0.7" if BW else "#4c72b0"
    c_replay = "0.3" if BW else "#dd8452"
    axb.bar(x, hiom_open, 0.6, color=c_open, label="PM 映射打开")
    axb.bar(x, hiom_ctor, 0.6, bottom=hiom_open, color=c_replay,
            label="尾部重放")
    tops = [o + c for o, c in zip(hiom_open, hiom_ctor)]
    for i in range(len(Ns)):
        axb.annotate(f"{hiom_total[i]:.0f}", xy=(x[i], tops[i]), xytext=(0, 3),
                     textcoords="offset points", ha="center", fontsize=8.5)
    axb.set_ylim(0, 1.18 * max(max(tops), max(hiom_total)))
    axb.set_xticks(x)
    axb.set_xticklabels([f"{n / 1e6:g}M" for n in Ns])
    axb.set_xlabel("数据规模 N\n(b) HiOM 恢复时间分解", linespacing=1.8)
    axb.set_ylabel("恢复时间(ms)")
    axb.grid(True, axis="y", alpha=0.5, linestyle="--")
    axb.spines["top"].set_visible(False)
    axb.spines["right"].set_visible(False)
    axb.legend(fontsize=8.5, loc="upper left", frameon=False)

    plt.tight_layout()
    pdf = os.path.join(OUT_DIR, "recovery_vs_n.pdf")
    png = os.path.join(OUT_DIR, "recovery_vs_n.png")
    plt.savefig(pdf, bbox_inches="tight")
    if not BW:
        plt.savefig(png, dpi=300, bbox_inches="tight")
    print("wrote", pdf)

    print(f"\n{'N(M)':>6}{'HiOMclean':>10}{'HiOMkill':>9}{'Halo.ms':>9}"
          f"{'Viper.ms':>10}{'Halo/Hc':>9}{'Vip/Hc':>8}{'suffix':>8}")
    halo_by_n = dict(zip(halo_Ns, halo_total))
    for i, n in enumerate(Ns):
        hc = hc_by_n.get(n)
        ref = hc if hc is not None else hiom_total[i]
        hm = halo_by_n.get(n)
        hc_ms = f"{hc:>10.0f}" if hc is not None else f"{'-':>10}"
        halo_ms = f"{hm:>9.0f}" if hm is not None else f"{'-':>9}"
        halo_r = f"{hm / ref:>8.1f}×" if hm is not None else f"{'-':>9}"
        print(f"{n / 1e6:>6g}{hc_ms}{hiom_total[i]:>9.0f}{halo_ms}"
              f"{viper_total[i]:>10.0f}{halo_r}{viper_total[i] / ref:>7.0f}×"
              f"{hiom_suffix[i]:>8.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
