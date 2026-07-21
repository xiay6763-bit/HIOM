#!/usr/bin/env python3
"""S5b: unsafe-suffix figure — T = alpha + beta*U fit (single panel).

Companion to recovery_vs_n (C3). With the O(P) lock scan gone (bounded-lock-set
protocol) tail replay dominates total recovery, so the O(unsafe suffix) claim
is directly measurable:

  Single T_total-vs-U scatter, pooled linear fit T = alpha + beta*U:
      alpha = the cold-open floor (PM mmap + checkpoint read + hot alloc),
      beta  = per-block replay cost. Update-heavy crashes anchor alpha at
      U ~= 0 (in-place updates never move the block frontier -> U stays at
      the floor, =8); insert crashes span U in the hundreds-thousands and pin
      beta. U's distribution is visible in the x-coordinates themselves — a
      separate distribution panel was a redundant projection and was dropped.

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

# 中文标注:font.family 直接给列表才有逐字形回退;子图题置于子图下方
plt.rcParams["font.family"] = ["DejaVu Sans", "Noto Sans CJK JP"]
plt.rcParams["axes.unicode_minus"] = False

BW = os.environ.get("BW") == "1"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.normpath(os.path.join(
    SCRIPT_DIR, "..", "paper", "figures") if BW
    else os.path.join(SCRIPT_DIR, "charts"))

WL_STYLE = {
    "insert": {"label": "纯插入(16线程)", "color": "0.0" if BW else "#d62728",
               "marker": "o"},
    "ycsb_b": {"label": "YCSB-B(8线程)", "color": "0.45" if BW else "#1f77b4",
               "marker": "s"},
    "ycsb_a": {"label": "YCSB-A(8线程)", "color": "0.7" if BW else "#2ca02c",
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

    # 占满左栏的图(随正文内联,width=7.2cm)。图幅按栏宽设计,绘图区用
    # tight 撑满、不留多余白边(ylabel 在左属正常,不再强行对称留白——那会
    # 把右侧挤出一块白边、绘图区反而变窄)。
    fig, ax = plt.subplots(figsize=(3.5, 2.55))

    # 单面板:T-vs-U 散点 + 合并线性拟合。U 的分布信息就在横坐标里(插入
    # 样本铺开、更新样本贴零),单独的分布面板是冗余投影,已删(user 拍板);
    # U≈0 处 50 个样本挤成一簇,用一条标注点明,数值范围正文给。
    all_u = np.array([u for pts in data.values() for u, _ in pts])
    all_t = np.array([t for pts in data.values() for _, t in pts])
    beta, alpha = np.polyfit(all_u, all_t, 1)
    r = np.corrcoef(all_u, all_t)[0, 1]
    for wl in ("insert", "ycsb_b", "ycsb_a"):
        if wl not in data:
            continue
        st = WL_STYLE[wl]
        pts = data[wl]
        ax.scatter([u for u, _ in pts], [t for _, t in pts], s=18,
                   color=st["color"], marker=st["marker"], alpha=0.8,
                   label=st["label"])
    xs = np.linspace(0, all_u.max() * 1.05, 100)
    ax.plot(xs, alpha + beta * xs, color="0.2", ls="--", lw=1.5,
            label=(f"T={alpha:.0f}+{beta:.2f}U"
                   f" (R\N{SUPERSCRIPT TWO}={r * r:.2f})"))
    ax.annotate("更新负载:U 恒为 8", xy=(40, 150), xytext=(340, 170),
                fontsize=8, color="0.25", va="center",
                arrowprops=dict(arrowstyle="->", color="0.45", lw=0.7))
    ax.set_xlabel("未持久尾部 U(块)", fontsize=9, labelpad=1)
    ax.set_ylabel("恢复时间 T(ms)", fontsize=9, labelpad=1)
    ax.tick_params(axis="both", labelsize=8, pad=1.5)
    ax.grid(alpha=0.5, linestyle="--")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(fontsize=7.2, loc="upper left", frameon=False, borderpad=0.4,
              labelspacing=0.3, handletextpad=0.4, borderaxespad=0.5)

    # 手动对称留白(left 与 1-right 相等),让绘图区在图片内水平居中——
    # bbox_inches="tight" 会把 ylabel 侧和绘图区右缘都裁到贴内容,使绘图区
    # 偏右(半栏图上尤其明显)。故不用 tight,固定对称边距。
    # 数据框在图片内左右对称留白 -> 绘图框居中于栏(user 选定)。左留白容纳
    # ylabel+刻度,右留等量白;图片仍占满栏宽,故绘图框居中于左栏。不用
    # bbox_inches="tight"(它会把两侧裁到贴内容,破坏对称)。
    fig.subplots_adjust(left=0.15, right=0.85, top=0.955, bottom=0.16)
    pdf = os.path.join(OUT_DIR, "unsafe_suffix.pdf")
    plt.savefig(pdf)
    if not BW:
        plt.savefig(os.path.join(OUT_DIR, "unsafe_suffix.png"), dpi=300)
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
