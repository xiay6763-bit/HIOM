#!/usr/bin/env bash
#
# Phase 2 — full scaling sweep driver.
#
# For each (dataset_size, workload, fixture) combination, runs ycsb_bm
# in a fresh process so the RSS baseline isn't polluted by the
# previous fixture's CCEH/heap residue. Each process exercises
# 3 reps × 3 thread counts = 9 timed runs per invocation.
#
# Outputs JSON to /root/viper/results/scaling/.
#
# Skips invocations whose output JSON already exists, so the script
# can be safely resumed after a Ctrl-C or hardware glitch — delete
# the offending JSON file to force re-run.
#
# Total wall time estimate at all 5 sizes + 2 workloads + 2 fixtures:
# ~3-4 hours sequential.
set -e

YCSB_BM="/root/viper/build/benchmark/ycsb_bm"
OUT_DIR="/root/viper/results/scaling"
mkdir -p "${OUT_DIR}"

# Datasets: 0.15x → 3x of HotTier capacity (33M slots).
SIZES=(5 10 16 33 50)
WORKLOADS=(100r_zipf 100r_uniform)
FIXTURES=(ViperFixture HiOMFixture)

# Filter to throughput-only (skip _lat); 3 thread counts; --benchmark_repetitions
# is already 3 via READ_ARGS macro.
FILTER_THREADS='threads:(1|8|24)$'

for size in "${SIZES[@]}"; do
  for workload in "${WORKLOADS[@]}"; do
    for fixture in "${FIXTURES[@]}"; do
      out="${OUT_DIR}/${fixture}_${workload}_${size}M.json"
      if [ -s "${out}" ]; then
        echo "SKIP ${fixture} ${workload} ${size}M — output exists"
        continue
      fi
      filter="${fixture}.*${workload}_tp.*${FILTER_THREADS}"
      echo "RUN  ${fixture} ${workload} ${size}M → ${out}"
      YCSB_SIZE_TAG="${size}M" "${YCSB_BM}" \
          --benchmark_filter="${filter}" \
          --benchmark_out="${out}" \
          --benchmark_out_format=json \
          2>&1 | tail -5
      echo
    done
  done
done

echo "ALL SWEEP RUNS DONE."
ls -lh "${OUT_DIR}"
