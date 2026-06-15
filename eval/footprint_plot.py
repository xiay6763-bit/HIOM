#!/usr/bin/env python3
"""
Memory footprint stacked bar — Viper-paper Fig.9 style, four-system (E1 / C1).

Each bar stacks, bottom→top:
  PMem — data records   (light grey)
  PMem — index          (dark grey; only HiOM ColdTier + Dash hash table)
  DRAM — index          (system colour, on top)

Story: DRAM is the scarce resource (Viper paper: ~1/8 the capacity, ~9x the
$/GB). DRAM-index designs (Viper, CCEH) spend ~2 GB DRAM on the offset map;
HiOM keeps the authoritative index in PMem (ColdTier) plus a small fixed DRAM
hot tier, cutting DRAM ~87% while total memory stays comparable. Dash is the
PM-resident end (DRAM ~0) but pays it back in read throughput (see E2).

Data provenance (10 M records, K8/V200 = 216 B):
  - DRAM (index): WHITE-BOX measured via fixture_dram_bytes()
      Viper 2052, CCEH 2052, HiOM 272, Dash ~0 MB.
  - PMem data: ANALYTICAL. raw = N*216B = 2.16 GB. VPage layout (Viper/HiOM)
      packs ~= raw (Viper paper: 21.2 GB @100M vs raw 21.6 GB). Per-entry PMDK
      allocation (CCEH/Dash) carries ~+10% (Viper paper: Dash 23.8 GB @100M).
  - PMem index: ANALYTICAL. HiOM ColdTier = 96 MB (struct: 32 regions x
      (8192 main + 16384 overflow + 1) x 128 B Bucket, verified vs cold.bin).
      Dash ~210 MB (extendible hashing; Viper paper Dash index 2.1 GB @100M /10).
      Viper/CCEH keep the index in DRAM -> 0 PMem.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# BW=1 → grayscale + hatch, written to paper/figures/ (B&W-printed journals).
BW = os.environ.get("BW") == "1"
OUT = "/root/viper/paper/figures/footprint.pdf" if BW else "/root/viper/eval/charts/footprint.pdf"
if BW:
    C_DATA, C_INDEX, C_DRAM = "0.82", "0.55", "0.15"
    H_DATA, H_INDEX, H_DRAM = "", "//", "xx"
    DRAM_TXT = "0.0"
else:
    C_DATA, C_INDEX, C_DRAM = "#cfcfcf", "#7f7f7f", "#d62728"
    H_DATA, H_INDEX, H_DRAM = "", "", ""
    DRAM_TXT = "#d62728"

DATA_VPAGE = 2120.0      # MB  (Viper/HiOM compact VPage ~ raw, Viper-paper scaled)
DATA_PERENTRY = 2380.0   # MB  (CCEH/Dash per-entry PMDK alloc, +~10%)

SYS = [
    ("Viper", dict(dram=2052, pm_data=DATA_VPAGE,    pm_index=0)),
    ("CCEH",  dict(dram=2052, pm_data=DATA_PERENTRY, pm_index=0)),
    ("HiOM",  dict(dram=272,  pm_data=DATA_VPAGE,    pm_index=96)),
    ("Dash",  dict(dram=2,    pm_data=DATA_PERENTRY, pm_index=210)),
]

labels   = [s[0] for s in SYS]
pm_data  = [s[1]["pm_data"]  / 1024 for s in SYS]   # GB
pm_index = [s[1]["pm_index"] / 1024 for s in SYS]
dram     = [s[1]["dram"]     / 1024 for s in SYS]

fig, ax = plt.subplots(figsize=(7, 5))
x = range(len(labels))
ax.bar(x, pm_data,  color=C_DATA, hatch=H_DATA, edgecolor="0.2", label="PMem — data records")
ax.bar(x, pm_index, bottom=pm_data, color=C_INDEX, hatch=H_INDEX, edgecolor="0.2",
       label="PMem — index (HiOM ColdTier / Dash hash)")
base2 = [a + b for a, b in zip(pm_data, pm_index)]
ax.bar(x, dram, bottom=base2, color=C_DRAM, hatch=H_DRAM, edgecolor="0.2",
       label="DRAM — index (offset map)")

for i in x:
    total = pm_data[i] + pm_index[i] + dram[i]
    dmb = dram[i] * 1024
    ax.text(i, total + 0.06,
            (f"DRAM\n{dmb:.0f} MB" if dmb >= 10 else "DRAM\n~0"),
            ha="center", va="bottom", fontsize=9, fontweight="bold",
            color=DRAM_TXT)

# Headroom so the two-line DRAM labels clear the title and the legend box.
_max_total = max(b + d for b, d in zip(base2, dram))
ax.set_ylim(0, _max_total * 1.22)

ax.set_xticks(list(x))
ax.set_xticklabels(labels)
ax.set_ylabel("Memory footprint (GB)")
ax.set_title("C1: Memory footprint @10M (K8/V200)\n"
             "DRAM white-box measured · PMem analytical estimate", pad=12)
ax.legend(loc="upper right", fontsize=8)
ax.grid(axis="y", alpha=0.3)
plt.tight_layout()
plt.savefig(OUT)
print("wrote", OUT)

print(f"\n{'system':7}{'DRAM(MB)':>10}{'PMem(GB)':>10}{'total(GB)':>11}{'DRAM:PMem':>12}")
print("-" * 50)
for name, d in SYS:
    pm = (d["pm_data"] + d["pm_index"]) / 1024
    tot = pm + d["dram"] / 1024
    ratio = f"1/{pm * 1024 / max(d['dram'], 1):.0f}"
    print(f"{name:7}{d['dram']:>10}{pm:>10.2f}{tot:>11.2f}{ratio:>12}")
