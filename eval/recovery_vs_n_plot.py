#!/usr/bin/env python3
"""
C3 recovery time vs dataset size N — three recovery complexities (E3).

ALL THREE SYSTEMS AT K8/V8 in the SAME Halo hash_api harness (apples-to-apples).
Why this matters: recovery is value-size-INDEPENDENT for HiOM and Halo (HiOM
replays a cadence-bounded tail; Halo memcpy's an index of fingerprint+offset),
but value-size-SENSITIVE for Viper (recover_database rescans every VPage block,
and V200 packs ~9x fewer entries/block than V8). Mixing Viper@V200 with Halo@V8
would be unfair, so we measure Viper at V8 here and keep the paper's V200 number
in the E3 prose.

  HiOM  : O(tail). Index stays in PM (mmap, no reload); ~87 ms CONSTANT in N,
          value-independent (paper hiom_recovery_bm; tail=4096 + 28 ms hot alloc).
  Halo  : O(DRAM-index) memcpy from PM snapshot. ~320 ms floor + memcpy.
          MEASURED V8, 1/10/33M at normal 78-82% load (value-independent).
  Viper : O(N). recover_database rehash+reinsert. MEASURED V8 recover_database:
          9/65/241/1287 ms at 1/10/33/100M. At the paper's K8/V200 load Viper
          hits 2180 ms @100M (=25x vs HiOM) — shown as a single annotated point.

Same-V8 takeaways: HiOM is constant 87 ms; Viper is cheapest at small N (O(N)
small) but crosses above HiOM near ~12M and reaches 14.8x at 100M; Halo sits at
a ~320 ms floor throughout (4-5x slower than HiOM). The 100M Halo point is a
resize blow-up (16 GB CLHT, 12% load) — hollow, excluded from the trend.

Measured 2026-06-15 via the Halo harness (halo_recovery_smoke / viper_recovery_smoke).

NOTE (2026-07-14): the paper's main C3 figure (recovery_vs_n.pdf) is now the
fresh-process cold-vs-cold real-SIGKILL figure at K8/V200
(eval/recovery_vs_n_crash_plot.py). THIS script is the alternative same-V8
apples-to-apples cut that ALSO includes Halo; it writes recovery_vs_n_v8_halo.pdf
and feeds the E3 prose (Halo ~320 ms floor; Viper V200 25× point).
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BW = os.environ.get("BW") == "1"
OUT = ("/root/viper/paper/figures/recovery_vs_n_v8_halo.pdf" if BW
       else "/root/viper/eval/charts/recovery_vs_n_v8_halo.pdf")

HIOM_MS = 87.0                            # O(tail), constant, value-independent
N_HALO  = [1, 10, 33]                     # V8, normal 78-82% load
HALO_MS = [327.4, 332.1, 449.1]
HALO_BLOWUP = (100, 3024.2)               # resize artifact: 16 GB CLHT @ 12% load
N_VIP   = [1, 10, 33, 100]                # V8, measured recover_database
VIPER_V8 = [9.0, 65.0, 241.0, 1287.0]
VIPER_V200_100 = 2180.0                   # paper K8/V200 load @100M -> 25x

if BW:
    c_hiom, c_halo, c_viper = "0.0", "0.45", "0.0"
else:
    c_hiom, c_halo, c_viper = "#990000", "#006600", "#000099"

allN = [1, 10, 33, 100]
fig, ax = plt.subplots(figsize=(7, 5))

# HiOM — O(tail), constant, value-independent
ax.plot(allN, [HIOM_MS] * len(allN), ls="-.", marker="D", ms=7, color=c_hiom,
        label="HiOM — O(tail), 87 ms constant (value-indep.)")

# Viper — O(N), V8 measured; V200 paper-load point annotated
ax.plot(N_VIP, VIPER_V8, ls="--", marker="s", ms=6, color=c_viper,
        label="Viper — O(N) rebuild, V8 measured")
ax.plot([100], [VIPER_V200_100], marker="s", ms=9, mfc="none", mec=c_viper,
        mew=1.5, ls="none", label="Viper @V200 (paper load) = 2180 ms / 25×")
ax.annotate("V200 load\n2180 ms (25×)", xy=(100, VIPER_V200_100),
            xytext=(34, 1700), fontsize=8, color=c_viper,
            arrowprops=dict(arrowstyle="->", color=c_viper, lw=0.8))

# Halo — O(index) memcpy, V8 measured + isolated blow-up
ax.plot(N_HALO, HALO_MS, ls="-", marker="o", ms=7, color=c_halo,
        label="Halo — O(index) memcpy, ~320 ms floor (V8)")
ax.plot([HALO_BLOWUP[0]], [HALO_BLOWUP[1]], marker="o", ms=9, mfc="none",
        mec=c_halo, mew=1.5, ls="none",
        label="Halo @100M — resize blow-up (16 GB, 12% load)")
ax.annotate("resize artifact", xy=HALO_BLOWUP, xytext=(11, 4200), fontsize=8,
            color=c_halo, arrowprops=dict(arrowstyle="->", color=c_halo, lw=0.8))

ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("Dataset size $N$ (millions)")
ax.set_ylabel("Cold-start recovery time (ms)")
ax.set_title("C3: recovery vs dataset size (all K8/V8, same harness)\n"
             "O(tail) vs O(DRAM-index) vs O(N) — HiOM constant; wins at large N", pad=10)
ax.set_xticks(allN)
ax.set_xticklabels([str(n) for n in allN])
ax.minorticks_off()
ax.grid(True, which="major", alpha=0.3)
ax.legend(fontsize=7, loc="center left")
plt.tight_layout()
plt.savefig(OUT)
print("wrote", OUT)

print(f"\n{'N(M)':>6}{'HiOM':>7}{'Viper.V8':>10}{'Halo.V8':>9}  HiOM-vs-Viper")
for i, n in enumerate(N_VIP):
    halo = HALO_MS[N_HALO.index(n)] if n in N_HALO else (HALO_BLOWUP[1] if n == 100 else None)
    halo_s = f"{halo:.0f}" + ("*" if n == 100 else "") if halo else "-"
    ratio = VIPER_V8[i] / HIOM_MS
    tag = "HiOM faster" if ratio > 1 else "Viper faster"
    print(f"{n:>6}{HIOM_MS:>7.0f}{VIPER_V8[i]:>10.0f}{halo_s:>9}  {ratio:>4.1f}x ({tag})")
print("  (*Halo 100M = resize artifact; Viper @V200 100M = 2180 ms = 25x, in E3 prose)")
