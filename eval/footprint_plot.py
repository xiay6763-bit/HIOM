#!/usr/bin/env python3
"""C1 figure: memory footprint — single 1x2 panel (composition + scaling).

  (a) stacked memory composition @10M (K8/V200): PMem data + PMem index +
      DRAM index for Viper / CCEH / Halo / HiOM / Dash. Total memory is
      comparable; WHERE the index lives is the design choice. HiOM keeps the
      authoritative index in PMem (ColdTier, 96 MB) plus a small fixed DRAM
      hot tier -> DRAM index cut ~87% vs Viper/CCEH.
  (b) DRAM index vs dataset size N — the three placement strategies:
      Viper/CCEH pre-allocated pinned (~2 GB flat, measured 1-50M, 100M
      extrapolated hollow), Halo on-demand DRAM CLHT (linear then
      resize-doubling to 16 GB @100M, hollow), HiOM fixed-capacity hot tier
      (272 MB constant, occupancy-independent).

Data provenance:
  - DRAM (index): WHITE-BOX via fixture_dram_bytes(). Viper 2052 / HiOM 272.02
    RE-MEASURED 2026-07-14 under the re-frozen core (CORE=220fb60), identical
    to the frozen numbers; Viper measured flat 2052->2065 MB across 1-50M
    (depth-17 CCEH directory pinned; splits +0.6%), 100M ~2100 extrapolated.
    HiOM control structs beyond the HotTier audited immaterial (pending ring
    512 KiB + commit lanes + checkpoint ~KB, ~0.2%). Halo measured 2026-06-15
    via the Halo hash_api harness (bucket bytes = total_slot/3 * 64 B).
  - PMem data: ANALYTICAL. raw = N*216 B; VPage layout (Viper/HiOM/Halo) packs
    ~= raw (Viper paper: 21.2 GB @100M vs 21.6 raw); per-entry PMDK alloc
    (CCEH/Dash) ~ +10%.
  - PMem index: ANALYTICAL. HiOM ColdTier 96 MB (32 regions x (8192 main +
    16384 overflow + 1) x 128 B, verified vs cold.bin); Dash ~210 MB.

Output: eval/charts/footprint.{pdf,png}; BW=1 -> paper/figures/footprint.pdf
(grayscale + hatch for B&W print).
"""
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

BW = os.environ.get("BW") == "1"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "paper", "figures")
                           if BW else os.path.join(SCRIPT_DIR, "charts"))

# ---------------------------------------------------------------- style ----
plt.rcParams.update({
    # 中文标注:font.family 直接给字体列表才有逐字形回退(sans-serif 别名只取首个)
    "font.family": ["DejaVu Sans", "Noto Sans CJK JP"],
    "axes.unicode_minus": False,
    "font.size": 10,
    "axes.labelsize": 10.5,
    "axes.titlesize": 11,
    "legend.fontsize": 8.2,
    "xtick.labelsize": 9.5,
    "ytick.labelsize": 9.5,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.linewidth": 0.9,
    "grid.linestyle": "--",
    "grid.alpha": 0.5,
    "legend.frameon": False,
})

if BW:
    C_DATA, C_PMIDX, C_DRAM = "0.86", "0.60", "0.20"
    H_DATA, H_PMIDX, H_DRAM = "", "//", "xx"
    C_VIPER = C_HIOM = "0.0"
    C_HALO = "0.45"
else:
    C_DATA, C_PMIDX, C_DRAM = "#e8e8e8", "#b0b0b0", "#4c72b0"
    H_DATA, H_PMIDX, H_DRAM = "", "//", ""
    C_VIPER, C_HALO, C_HIOM = "#1f77b4", "#2ca02c", "#d62728"

# ----------------------------------------------------------------- data ----
DATA_VPAGE = 2120.0     # MB @10M: compact VPage layout ~= raw N*216B
DATA_PERENTRY = 2380.0  # MB @10M: per-entry PMDK allocation, ~+10%

SYS = [  # (label, DRAM-index MB, PMem-data MB, PMem-index MB)
    ("Viper", 2052, DATA_VPAGE,    0),
    ("CCEH",  2052, DATA_PERENTRY, 0),
    ("Halo",  247,  DATA_VPAGE,    0),
    ("HiOM",  272,  DATA_VPAGE,    96),
    ("Dash",  2,    DATA_PERENTRY, 210),
]

N_GRID = [1, 10, 33, 100]                    # millions
VIPER_MEAS = [2052.0, 2052.0, 2053.0]        # measured 1/10/33M (flat, pinned)
VIPER_EXTRAP_100 = 2100.0                    # mild-split extrapolation
HALO_MEAS = [26.0, 247.2, 852.4]             # measured 1/10/33M (~26 B/key)
HALO_BLOWUP_100 = 16384.0                    # resize-doubled CLHT, 12% load
HIOM_MB = 272.02                             # capacity-bound constant
CROSS_N = 10.5                               # HiOM == Halo crossover

# ----------------------------------------------------------------- plot ----
# 跨栏浮动图:左右两个子图(1×2)宽扁版式,按 ~15.2cm 跨栏宽设计,
# 页顶浮动呈现 -> 避免竖长整块在栏尾塞不下而跳栏留白。
fig, (axa, axb) = plt.subplots(1, 2, figsize=(7.4, 2.7),
                               gridspec_kw={"width_ratios": [1.0, 1.0]})

# --- (a) composition @10M -------------------------------------------------
labels = [s[0] for s in SYS]
dram = np.array([s[1] for s in SYS]) / 1024.0     # GB
pmd = np.array([s[2] for s in SYS]) / 1024.0
pmi = np.array([s[3] for s in SYS]) / 1024.0
x = np.arange(len(SYS))

axa.bar(x, pmd, 0.62, color=C_DATA, hatch=H_DATA, edgecolor="0.35",
        linewidth=0.6, label="PM 数据")
axa.bar(x, pmi, 0.62, bottom=pmd, color=C_PMIDX, hatch=H_PMIDX,
        edgecolor="0.35", linewidth=0.6, label="PM 索引")
axa.bar(x, dram, 0.62, bottom=pmd + pmi, color=C_DRAM, hatch=H_DRAM,
        edgecolor="0.35", linewidth=0.6, label="DRAM 索引")
for i, s in enumerate(SYS):
    pass  # 柱顶 MB 数值标注已删(单栏窄柱下会重叠;精确值见正文 3.2 节)
axa.set_xticks(x)
axa.set_xticklabels(labels)
axa.set_ylabel("内存占用(GB)")
axa.set_ylim(0, 5.0)
axa.grid(axis="y")
axa.legend(loc="upper right", handlelength=1.4, handleheight=1.1)
axa.set_xlabel("(a) 内存构成(N=10M)")

# --- (b) DRAM index vs N ----------------------------------------------------
axb.plot(N_GRID[:3] + [100], VIPER_MEAS + [VIPER_EXTRAP_100],
         ls="--", lw=1.8, color=C_VIPER, marker="s", ms=6,
         markevery=[0, 1, 2], label="Viper/CCEH")
axb.plot([100], [VIPER_EXTRAP_100], marker="s", ms=7, mfc="none",
         mec=C_VIPER, mew=1.4, ls="none")
axb.plot(N_GRID[:3], HALO_MEAS, ls="-", lw=1.8, color=C_HALO, marker="o",
         ms=6, label="Halo")
axb.plot([N_GRID[2], 100], [HALO_MEAS[2], HALO_BLOWUP_100], ls=":",
         lw=1.4, color=C_HALO)
axb.plot([100], [HALO_BLOWUP_100], marker="o", ms=7, mfc="none", mec=C_HALO,
         mew=1.4, ls="none")
axb.plot(N_GRID, [HIOM_MB] * 4, ls="-.", lw=2.0, color=C_HIOM, marker="D",
         ms=6, label="HiOM")

# 空心点 = 非实测(外推 / 扩容后);"扩容至 16 GB""(外推)""恒定 272 MB"
# 三处文字标注均已删(与图例/数据点重叠),这些数值含义见正文 3.2 节

axb.set_xscale("log")
axb.set_yscale("log")
axb.set_xticks(N_GRID)
axb.set_xticklabels([str(n) for n in N_GRID])
axb.minorticks_off()
axb.set_xlabel("数据规模 N(百万)\n(b) DRAM 索引占用随 N 变化",
               linespacing=1.8)
axb.set_ylabel("DRAM 索引占用(MB)")
axb.grid(True, which="major")
axb.legend(loc="upper left", fontsize=7.5, handlelength=1.5,
           labelspacing=0.3, borderaxespad=0.5)

fig.tight_layout(w_pad=2.0)
os.makedirs(OUT_DIR, exist_ok=True)
pdf = os.path.join(OUT_DIR, "footprint.pdf")
fig.savefig(pdf, bbox_inches="tight")
if not BW:
    fig.savefig(os.path.join(OUT_DIR, "footprint.png"), dpi=300,
                bbox_inches="tight")
print("wrote", pdf)

print(f"\n{'sys':>6}  DRAM(MB)  PM.data  PM.idx  |  vs-N crossover ~{CROSS_N} M")
for s in SYS:
    print(f"{s[0]:>6}  {s[1]:8.0f}  {s[2]:7.0f}  {s[3]:6.0f}")
