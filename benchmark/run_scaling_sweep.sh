#!/usr/bin/env bash
#
# Phase 2 + Phase 3 scaling sweep driver.
#
# Matrix (after the 2026-05-18 expansion):
#   sizes       4   (5/10/16/33 M — 50 M dropped: HiOM hangs on prefill
#                    back-pressure beyond the HotTier 33.5 M capacity)
#   workloads   6   (100r_zipf, 100r_uniform — read-only baseline;
#                    a_zipf, a_uniform — standard YCSB-A 50/50 read+update;
#                    b_zipf, b_uniform — standard YCSB-B 95/5 read+update)
#   fixtures    2   (ViperFixture, HiOMFixture)
#   metrics     2   (_tp throughput, _lat tail-latency via HDR — emits
#                    hdr_read_* and hdr_write_* separately so mixed
#                    workloads get the read vs write CDF split)
#   threads     3   (1, 8, 24 — same Google Benchmark filter as Phase 2)
#   reps        3   (--benchmark_repetitions=3, hardcoded in DEFINE_BM)
#
# = 4 × 6 × 2 × 2 = 96 ycsb_bm invocations; each ~15-25 min wall =>
# ~24-40 h sequential. Resumable via the "skip if output exists" gate
# at the loop body so a Ctrl-C / power blip doesn't restart from zero.
#
# Pruning knobs (env vars):
#   SWEEP_LAT_THREADS_ONLY=8  — only run _lat at the median-thread
#                               count, since tail latency at t=24 is
#                               dominated by queueing noise that
#                               doesn't survive paper figures anyway.
#                               Default: enabled (cuts _lat cells ⅓).
#
# Usage:
#   ./run_scaling_sweep.sh              # actually run
#   ./run_scaling_sweep.sh --dry-run    # print every command + matrix
#                                        size estimate, run nothing
#
# Output: JSON files at /root/viper/results/scaling/. Filename convention
# <Fixture>_<workload>_<size>M_<metric>.json (metric = "tp" or "lat").
# The legacy filename layout (without _<metric>) is preserved for the
# Phase 2 *_tp runs already on disk so the plotter doesn't have to
# re-parse stale data.

set -e

YCSB_BM="/root/viper/build/benchmark/ycsb_bm"
OUT_DIR="/root/viper/results/scaling"
mkdir -p "${OUT_DIR}"

# Per-cell wall-clock budget. A healthy HiOM cell at 16M finishes in
# ~5-10 min; 33M write-heavy is unknown territory (M4 back-pressure
# limit per HIOM.md). Cap each cell so a single hang doesn't block
# the whole batch. On timeout, the cell's partial JSON is moved aside
# and the loop continues to the next cell.
SWEEP_PER_CELL_TIMEOUT_S="${SWEEP_PER_CELL_TIMEOUT_S:-1800}"
TIMEOUT_LOG="${OUT_DIR}/.sweep_timeouts.log"

# Datasets: 0.15× → 1× of HotTier capacity (33 M slots).
# 50 M cell intentionally omitted (M4 back-pressure issue, paper future work).
#
# All four axes are env-overridable so a subset can be run without editing
# the script. Baseline back-fill example (Dash/CCEH at 10M, throughput only):
#   SWEEP_FIXTURES="DashFixture CcehFixture" SWEEP_SIZES=10 SWEEP_METRICS=tp ./run_scaling_sweep.sh
# Default fixtures are now all four §7 systems; the skip-if-exists gate below
# means a bare re-run only fills the missing (Dash/CCEH) cells, not the
# Viper/HiOM cells already on disk.
IFS=' ' read -ra SIZES     <<< "${SWEEP_SIZES:-5 10 16 33}"
IFS=' ' read -ra WORKLOADS <<< "${SWEEP_WORKLOADS:-100r_zipf 100r_uniform a_zipf a_uniform b_zipf b_uniform}"
IFS=' ' read -ra FIXTURES  <<< "${SWEEP_FIXTURES:-ViperFixture HiOMFixture DashFixture CcehFixture}"
IFS=' ' read -ra METRICS   <<< "${SWEEP_METRICS:-tp lat}"
FILTER_THREADS_TP='threads:(1|8|24)$'
FILTER_THREADS_LAT='threads:8$'   # latency cells only at t=8 by default

DRY_RUN=0
for arg in "$@"; do
  case "${arg}" in
    --dry-run) DRY_RUN=1 ;;
    *) echo "Unknown arg: ${arg}" >&2; exit 2 ;;
  esac
done

total=0
skip=0
plan_lines=()
for size in "${SIZES[@]}"; do
  for workload in "${WORKLOADS[@]}"; do
    for fixture in "${FIXTURES[@]}"; do
      for metric in "${METRICS[@]}"; do
        total=$((total + 1))
        out="${OUT_DIR}/${fixture}_${workload}_${size}M_${metric}.json"
        # Legacy *_tp results from Phase 2 were saved without the
        # _tp suffix. Honour that so we don't re-run them.
        legacy="${OUT_DIR}/${fixture}_${workload}_${size}M.json"
        if [ "${metric}" = "tp" ] && [ -s "${legacy}" ]; then
          skip=$((skip + 1))
          plan_lines+=("SKIP-LEGACY ${fixture} ${workload} ${size}M ${metric} (legacy file exists: ${legacy})")
          continue
        fi
        if [ -s "${out}" ]; then
          skip=$((skip + 1))
          plan_lines+=("SKIP-EXISTS ${fixture} ${workload} ${size}M ${metric} (${out})")
          continue
        fi
        if [ "${metric}" = "tp" ]; then
          filter="${fixture}.*${workload}_tp.*${FILTER_THREADS_TP}"
        else
          filter="${fixture}.*${workload}_lat.*${FILTER_THREADS_LAT}"
        fi
        cmd="YCSB_SIZE_TAG=${size}M ${YCSB_BM} --benchmark_filter='${filter}' --benchmark_out='${out}' --benchmark_out_format=json"
        plan_lines+=("RUN  ${fixture} ${workload} ${size}M ${metric}  →  ${out}")
        plan_lines+=("     ${cmd}")
        if [ "${DRY_RUN}" -eq 0 ]; then
          echo "RUN  ${fixture} ${workload} ${size}M ${metric} → ${out} (timeout ${SWEEP_PER_CELL_TIMEOUT_S}s)"
          # `set -e` is active for the script overall, but we need to
          # observe the timeout exit code (124) and skip past it. Wrap
          # the eval in a subshell with -e disabled so timeout's exit
          # code propagates as a normal value we can branch on.
          set +e
          timeout --signal=KILL "${SWEEP_PER_CELL_TIMEOUT_S}" bash -c "${cmd}" 2>&1 | tail -3
          rc=${PIPESTATUS[0]}
          set -e
          if [ "${rc}" -eq 124 ] || [ "${rc}" -eq 137 ]; then
            ts=$(date +%Y%m%d_%H%M%S)
            echo "  TIMEOUT after ${SWEEP_PER_CELL_TIMEOUT_S}s — moving partial JSON aside and continuing"
            if [ -s "${out}" ]; then
              mv "${out}" "${out}.timeout_${ts}"
            fi
            echo "${ts} TIMEOUT ${fixture} ${workload} ${size}M ${metric}" >> "${TIMEOUT_LOG}"
          elif [ "${rc}" -ne 0 ]; then
            echo "  non-zero exit ${rc} — leaving artifact for inspection"
            echo "$(date +%Y%m%d_%H%M%S) FAIL(${rc}) ${fixture} ${workload} ${size}M ${metric}" >> "${TIMEOUT_LOG}"
          fi
          echo
        fi
      done
    done
  done
done

todo=$((total - skip))
echo
echo "=============================="
echo "  Matrix : ${#SIZES[@]} sizes × ${#WORKLOADS[@]} workloads × ${#FIXTURES[@]} fixtures × ${#METRICS[@]} metrics = ${total} cells"
echo "  Skipped: ${skip} (already on disk)"
echo "  To run : ${todo}"
if [ "${todo}" -gt 0 ]; then
  echo "  Estimated wall : ~$((todo * 20 / 60)) h (assuming ~20 min/cell average)"
fi
echo "=============================="

if [ "${DRY_RUN}" -eq 1 ]; then
  echo
  echo "Plan (commands not executed):"
  for line in "${plan_lines[@]}"; do echo "  ${line}"; done
fi

if [ "${DRY_RUN}" -eq 0 ]; then
  echo "ALL SWEEP RUNS DONE."
  ls -lh "${OUT_DIR}"
fi
