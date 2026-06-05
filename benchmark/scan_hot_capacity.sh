#!/usr/bin/env bash
# C2 HotTier capacity sweep driver (hit-rate & throughput vs HotTier DRAM budget).
#
# Measurement caveat (2026-06-05): at small HotTier capacities the read phase
# livelocks at high thread counts — 8 threads' mirror_into_hot all contend on
# SIEVE eviction and spin on each other's PINNED slots (same root cause as the
# a_zipf-33M limitation). Hit rate is thread-INDEPENDENT, so the capacity curve
# is measured at t=1 (verified livelock-free across all 6 capacities). The
# read-heavy throughput RATIO (HiOM vs Viper) is reported SEPARATELY at t=8
# full-capacity (log2=21: 33M slots > 10M working set ⇒ eviction negligible,
# no livelock), matching the paper's t=8 convention.
#
# Usage:
#   benchmark/scan_hot_capacity.sh smoke   # gate: log2=16, t=1, uniform+zipf
#   benchmark/scan_hot_capacity.sh full    # (default) t=1 curve + t=8 full-cap ratio
#
# Each run is a fresh process; HIOM_HOT_BUCKETS_LOG2 picks the HotTier capacity
# (hiom_fixture reads + validates it). READ_ARGS already does Repetitions(3),
# so every point is a 3-rep median. Filters pin _tp (throughput) and exclude
# _lat to keep the sweep fast. Results land as GBench JSON under results/hot_scan/.
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root
export YCSB_SIZE_TAG="${YCSB_SIZE_TAG:-10M}"   # 10M-record working set (matches Status 05-17)
mkdir -p results/hot_scan eval/charts

mode="${1:-full}"
BM=./build/benchmark/ycsb_bm

if [[ "$mode" == smoke ]]; then
  # Gate: smallest capacity, t=1 (livelock-free), both workloads. Confirm the
  # t=1 sweep runs end-to-end (and uniform hit rate < zipf) before the full sweep.
  HIOM_HOT_BUCKETS_LOG2=16 "$BM" \
    --benchmark_filter='HiOMFixture.*100r_(uniform|zipf)_tp.*threads:1' \
    --benchmark_out=results/hot_scan/smoke_log2_16.json \
    --benchmark_out_format=json
  exit 0
fi

# (1) Hit-rate / throughput curve: HiOM at t=1, all 6 capacities (10%..330% of
#     the 10M working set). t=1 is livelock-free; hit rate is thread-independent.
for log2 in 16 17 18 19 20 21; do
  HIOM_HOT_BUCKETS_LOG2=$log2 "$BM" \
    --benchmark_filter='HiOMFixture.*100r_(uniform|zipf)_tp.*threads:1' \
    --benchmark_out=results/hot_scan/hiom_log2_${log2}.json \
    --benchmark_out_format=json
done

# (1b) Viper t=1 baseline for the throughput curve (same t=1 convention as the
#      HiOM curve, so the curve and its reference line are directly comparable).
"$BM" --benchmark_filter='ViperFixture.*100r_(uniform|zipf)_tp.*threads:1' \
  --benchmark_out=results/hot_scan/viper_baseline_t1.json \
  --benchmark_out_format=json

# (2) Read-heavy ratio: full-capacity (log2=21) at t=8 — HiOM vs Viper, the
#     paper's t=8 convention. log2=21 does not livelock (eviction negligible).
#     This pins down the current read-heavy ratio (clarifies Status 0.27–0.58).
HIOM_HOT_BUCKETS_LOG2=21 "$BM" \
  --benchmark_filter='HiOMFixture.*100r_(uniform|zipf)_tp.*threads:8' \
  --benchmark_out=results/hot_scan/hiom_fullcap_t8.json \
  --benchmark_out_format=json
"$BM" --benchmark_filter='ViperFixture.*100r_(uniform|zipf)_tp.*threads:8' \
  --benchmark_out=results/hot_scan/viper_fullcap_t8.json \
  --benchmark_out_format=json
