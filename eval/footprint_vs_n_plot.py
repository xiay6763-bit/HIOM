#!/usr/bin/env python3
"""
Index-DRAM vs dataset size N — the three DRAM-placement strategies (E1 / C1).

  Viper/CCEH : ~2 GB DRAM directory, pre-allocated and PINNED (flat across
               5-33M; the 131072-segment reservation only grows once exceeded).
  Halo       : DRAM-resident CLHT, ~26 B/key at moderate N, but the table
               resize-doubles -> DRAM jumps in steps and load-factor sags
               (MEASURED 1-100M: 26MB@1M .. 852MB@33M, then 16 GB@100M at only
               12% load after a doubling — far above a naive linear extrapolation).
  HiOM       : fixed-capacity SIEVE hot tier, 272 MB CONSTANT in N (8 B/key,
               3.2x denser per key than Halo); independent of dataset size.

HiOM is not the smallest at *every* N: below the ~10.5M crossover its fixed
272 MB reservation exceeds Halo's on-demand footprint. HiOM wins by being
constant + immune to resize blow-up — it overtakes Halo at large N (33M: 272 vs
852 MB; 100M: 272 MB vs 16 GB, where Halo's CLHT has resize-doubled past even
Viper) and never OOMs.

Halo DRAM is WHITE-BOX measured (total bucket bytes = total_slot/3 * 64 B,
primary array + overflow chain) via the Halo hash_api harness, 2026-06-15.
HiOM 272 / Viper 2052 from fixture_dram_bytes() (matches footprint_plot.py).
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BW = os.environ.get("BW") == "1"
OUT = ("/root/viper/paper/figures/footprint_vs_n.pdf" if BW
       else "/root/viper/eval/charts/footprint_vs_n.pdf")

# --- MEASURED Halo index DRAM (MB): exact bucket bytes, 2026-06-15 ---
# 100M is NOT a linear extrapolation: the CLHT resize-doubled (268M buckets,
# 12.4% load) -> 16 GB, a step jump that overshoots even Viper.
N_meas  = [1, 5, 10, 16, 33, 100]
HALO_MB = [26.0, 123.6, 247.2, 416.8, 852.4, 16384.0]

VIPER_MB = 2052.0                 # pre-allocated, pinned (measured flat 5-33M)
HIOM_MB  = 272.0                  # fixed hot-tier capacity, constant in N
CROSS_N  = 10.5                   # 272 MB == Halo @ ~26 B/key

if BW:
    c_viper, c_halo, c_hiom = "0.0", "0.45", "0.0"
else:
    c_viper, c_halo, c_hiom = "#000099", "#006600", "#990000"
ls_viper, ls_halo, ls_hiom = "--", "-", "-."
m_viper, m_halo, m_hiom = "s", "o", "D"

allN = [1, 5, 10, 16, 33, 100]
fig, ax = plt.subplots(figsize=(7, 5))

ax.plot(allN, [VIPER_MB] * len(allN), ls=ls_viper, marker=m_viper, ms=7,
        color=c_viper, label="Viper/CCEH — pre-alloc, pinned (~2 GB)")
ax.plot(N_meas, HALO_MB, ls=ls_halo, marker=m_halo, ms=7, color=c_halo,
        label="Halo — DRAM CLHT, resize-doubling (measured)")
ax.plot(allN, [HIOM_MB] * len(allN), ls=ls_hiom, marker=m_hiom, ms=7,
        color=c_hiom, label="HiOM — fixed hot tier, 272 MB (8 B/key)")

ax.axvline(CROSS_N, color="0.5", ls=":", lw=1)
ax.annotate(f"HiOM $\\approx$ Halo\n$\\approx${CROSS_N:.0f}M",
            xy=(CROSS_N, HIOM_MB), xytext=(CROSS_N * 1.25, HIOM_MB * 2.6),
            fontsize=8, color="0.3",
            arrowprops=dict(arrowstyle="->", color="0.5", lw=0.8))
ax.annotate("Halo resize\n16 GB @ 12% load", xy=(100, 16384),
            xytext=(34, 5200), fontsize=8, color=c_halo,
            arrowprops=dict(arrowstyle="->", color=c_halo, lw=0.8))

ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("Dataset size $N$ (millions)")
ax.set_ylabel("Index DRAM (MB)")
ax.set_title("C1: index DRAM vs dataset size (white-box)\n"
             "three placement strategies — pinned / linear / constant", pad=10)
ax.set_xticks(allN)
ax.set_xticklabels([str(n) for n in allN])
ax.minorticks_off()
ax.grid(True, which="major", alpha=0.3)
ax.legend(fontsize=8, loc="lower right")
plt.tight_layout()
plt.savefig(OUT)
print("wrote", OUT)

print(f"\n{'N(M)':>6}{'Viper':>9}{'Halo':>9}{'HiOM':>9}  winner")
for i, n in enumerate(N_meas):
    h = HALO_MB[i]
    win = "HiOM" if HIOM_MB < min(VIPER_MB, h) else ("Halo" if h < VIPER_MB else "Viper")
    print(f"{n:>6}{VIPER_MB:>9.0f}{h:>9.1f}{HIOM_MB:>9.0f}  {win}")
