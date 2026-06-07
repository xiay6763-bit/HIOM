#!/usr/bin/env bash
#
# Thread-scalability sweep for the read-only (100r) throughput-vs-threads
# figure (eval/thread_scaling_plot.py, Meto/Viper-paper style).
#
# Fixed at 10M records, metric=tp, threads 1/2/4/8/16/24 (the dense
# READ_ARGS sweep in ycsb_bm.cpp). 8 cells = 4 systems × {100r_zipf,
# 100r_uniform}. Output: results/thread_scaling/<Fixture>_100r_<wl>_10M_tp.json
#
# Kept as a SEPARATE dir + driver from run_scaling_sweep.sh so the vs-N
# scaling data (results/scaling/, t=1/8/24) and this vs-threads data
# (6 points) never clobber each other.
#
# NB on cost: every (thread config × rep) re-runs InitMap+prefill+DeInit,
# so each cell does 6×3 = 18 prefills of 10M. zipf workload = 5M ops
# (fast); uniform = 100M ops (t=1 ~50s/rep, the slow end). ~1–1.5h total.
#
# Usage:
#   ./run_thread_scaling.sh            # run
#   ./run_thread_scaling.sh --dry-run  # print plan only
# Env overrides: TS_FIXTURES, TS_WORKLOADS, TS_PER_CELL_TIMEOUT_S.
set -e

YCSB_BM="/root/viper/build/benchmark/ycsb_bm"
OUT_DIR="/root/viper/results/thread_scaling"
mkdir -p "${OUT_DIR}"

SIZE=10
THREAD_FILTER='threads:(1|2|4|8|16|24)$'
# Generous per-cell budget so the slow uniform t=1 reps (100M ops) finish.
PER_CELL_TIMEOUT_S="${TS_PER_CELL_TIMEOUT_S:-5400}"
TIMEOUT_LOG="${OUT_DIR}/.timeouts.log"

IFS=' ' read -ra FIXTURES  <<< "${TS_FIXTURES:-ViperFixture HiOMFixture DashFixture CcehFixture}"
IFS=' ' read -ra WORKLOADS <<< "${TS_WORKLOADS:-100r_zipf 100r_uniform}"

DRY_RUN=0
for arg in "$@"; do
  case "${arg}" in
    --dry-run) DRY_RUN=1 ;;
    *) echo "Unknown arg: ${arg}" >&2; exit 2 ;;
  esac
done

total=0; skip=0
for workload in "${WORKLOADS[@]}"; do
  for fixture in "${FIXTURES[@]}"; do
    total=$((total + 1))
    out="${OUT_DIR}/${fixture}_${workload}_${SIZE}M_tp.json"
    if [ -s "${out}" ]; then
      skip=$((skip + 1))
      echo "SKIP-EXISTS ${fixture} ${workload} (${out})"
      continue
    fi
    filter="${fixture}.*${workload}_tp.*${THREAD_FILTER}"
    cmd="YCSB_SIZE_TAG=${SIZE}M ${YCSB_BM} --benchmark_filter='${filter}' --benchmark_out='${out}' --benchmark_out_format=json"
    echo "RUN  ${fixture} ${workload} → ${out} (timeout ${PER_CELL_TIMEOUT_S}s)"
    if [ "${DRY_RUN}" -eq 1 ]; then
      echo "     ${cmd}"
      continue
    fi
    set +e
    timeout --signal=KILL "${PER_CELL_TIMEOUT_S}" bash -c "${cmd}" 2>&1 | tail -3
    rc=${PIPESTATUS[0]}
    set -e
    if [ "${rc}" -eq 124 ] || [ "${rc}" -eq 137 ]; then
      ts=$(date +%Y%m%d_%H%M%S)
      echo "  TIMEOUT after ${PER_CELL_TIMEOUT_S}s — moving partial aside"
      [ -s "${out}" ] && mv "${out}" "${out}.timeout_${ts}"
      echo "${ts} TIMEOUT ${fixture} ${workload}" >> "${TIMEOUT_LOG}"
    elif [ "${rc}" -ne 0 ]; then
      echo "  non-zero exit ${rc} — leaving artifact for inspection"
      echo "$(date +%Y%m%d_%H%M%S) FAIL(${rc}) ${fixture} ${workload}" >> "${TIMEOUT_LOG}"
    fi
    echo
  done
done

echo "=============================="
echo "  ${total} cells, ${skip} skipped, $((total-skip)) run"
echo "=============================="
[ "${DRY_RUN}" -eq 0 ] && ls -lh "${OUT_DIR}"
