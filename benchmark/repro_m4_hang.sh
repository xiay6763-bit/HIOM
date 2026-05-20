#!/usr/bin/env bash
#
# repro_m4_hang.sh — minimal repro for the HiOM M4 back-pressure hang
# at HiOMFixture<KeyType8, ValueType200>/a_uniform_tp threads:24, dataset
# 16M (YCSB-A uniform = 50% read + 50% update).
#
# Background: the 2026-05-18 Phase 2 scaling sweep finished threads:1
# and threads:8 of this cell, then locked up at threads:24 (partial
# JSON file at /root/viper/results/scaling/HiOMFixture_a_uniform_16M_tp.json
# truncates mid-write at 14607 B). ViperFixture finishes the same cell
# in a few minutes (18.75 M items/s at t=24), so a wall-clock budget
# of 20 min is generous enough to distinguish "slow" from "hung".
#
# Suspected cause: bucket-all-PINNED back-pressure path in
# include/viper/hiom/hiom.hpp:578-637 (try_inline_flush retry loop
# followed by the unbounded "while size_hint() > 0" drain wait). With
# 24 update-heavy producers contending on 4 background flushers'
# flusher_mus_, the cooperative inline-flush try_lock often loses,
# all 24 producers fall into sleep(50us) + wake_all_flushers storms,
# and the buffer never drains.
#
# Output: a timestamped directory under ${OUT_DIR_BASE:-/tmp}/. Contains
#   bm.log                   ycsb_bm stdout/stderr
#   bm.json                  Google Benchmark output (may be truncated
#                            on timeout — same shape as the original
#                            partial JSON the user is debugging)
#   gdb_threads_t<sec>.txt   `thread apply all bt` snapshot
#   ps_threads_t<sec>.txt    per-thread state + wchan
#   perf_t<sec>.data         CPU sample over 15 s (perf record -F 99
#                            -g --call-graph dwarf), use
#                              perf report -i perf_t<sec>.data
#                            to inspect; futex_wait will dominate, the
#                            interesting bit is the userspace stack
#                            above it.
#
# Env overrides:
#   TIMEOUT_S        wall-clock budget before SIGKILL (default 1200)
#   SAMPLE_AT_S      first stack/perf snapshot (default 600)
#   OUT_DIR_BASE     where to put the run directory (default /tmp)
#   YCSB_BM          path to ycsb_bm binary (default build/benchmark/ycsb_bm)

set -uo pipefail

YCSB_BM="${YCSB_BM:-/root/viper/build/benchmark/ycsb_bm}"
PREFILL_FILE="/pmem0/ycsb_data/ycsb_prefill_16M.dat"
WORKLOAD_FILE="/pmem0/ycsb_data/ycsb_wl_a_uniform_16M.dat"
TIMEOUT_S="${TIMEOUT_S:-1200}"
SAMPLE_AT_S="${SAMPLE_AT_S:-600}"
OUT_DIR_BASE="${OUT_DIR_BASE:-/tmp}"

OUT_DIR="${OUT_DIR_BASE}/hiom_m4_repro_$(date +%Y%m%d_%H%M%S)"
mkdir -p "${OUT_DIR}"
LOG="${OUT_DIR}/bm.log"
JSON="${OUT_DIR}/bm.json"

# Sanity checks.
[ -x "${YCSB_BM}" ] || { echo "missing binary: ${YCSB_BM}" >&2; exit 1; }
for f in "${PREFILL_FILE}" "${WORKLOAD_FILE}"; do
  [ -s "${f}" ] || { echo "missing ycsb data: ${f}" >&2; exit 1; }
done

# Diagnostic tools are nice-to-have; print warnings but proceed.
have_gdb=1;  command -v gdb  >/dev/null 2>&1 || { echo "warn: gdb not in PATH — skipping thread stack snapshots";  have_gdb=0; }
have_perf=1; command -v perf >/dev/null 2>&1 || { echo "warn: perf not in PATH — skipping CPU sampling";          have_perf=0; }

echo "=== Repro: HiOMFixture<KeyType8, ValueType200> a_uniform_tp 16M threads:24 ==="
echo "OUT_DIR     : ${OUT_DIR}"
echo "TIMEOUT_S   : ${TIMEOUT_S}"
echo "SAMPLE_AT_S : ${SAMPLE_AT_S}"
echo

# Launch ycsb_bm with a strict filter — only the threads:24 cell
# of the hung workload. The benchmark itself will internally do the
# 16M-key prefill via the util thread pool (NUM_UTIL_THREADS=36
# from benchmark.hpp), then run 3 reps of the timed phase at t=24.
YCSB_SIZE_TAG=16M "${YCSB_BM}" \
    --benchmark_filter='HiOMFixture.*a_uniform_tp.*threads:24$' \
    --benchmark_out="${JSON}" \
    --benchmark_out_format=json \
    >"${LOG}" 2>&1 &
BM_PID=$!
echo "started ycsb_bm pid=${BM_PID}, polling every 30 s"

snapshot() {
  local tag="$1"
  echo "[$(date +%H:%M:%S)] snapshot ${tag}"
  if [ "${have_gdb}" -eq 1 ]; then
    gdb -batch -nx \
        -ex 'set pagination off' \
        -ex 'set print thread-events off' \
        -ex 'thread apply all bt' \
        -p "${BM_PID}" \
        >"${OUT_DIR}/gdb_threads_${tag}.txt" 2>&1 \
      || echo "  gdb snapshot failed (process may have exited)"
  fi
  # Per-thread state + wait channel (wchan) — quick view of which
  # syscall each thread is blocked in. Cheap, always do.
  ps -L -o pid,tid,pcpu,state,wchan:32,comm -p "${BM_PID}" \
      >"${OUT_DIR}/ps_threads_${tag}.txt" 2>&1 || true
}

perf_sample() {
  local tag="$1"
  if [ "${have_perf}" -eq 0 ]; then return; fi
  echo "[$(date +%H:%M:%S)] perf record 15 s (${tag})"
  perf record -F 99 -g --call-graph dwarf -p "${BM_PID}" \
      -o "${OUT_DIR}/perf_${tag}.data" -- sleep 15 \
      >"${OUT_DIR}/perf_${tag}.log" 2>&1 \
    || echo "  perf record failed (kernel.perf_event_paranoid?)"
}

START_S=$(date +%s)
sampled=0
while kill -0 "${BM_PID}" 2>/dev/null; do
  NOW_S=$(date +%s)
  ELAPSED=$((NOW_S - START_S))

  if [ "${sampled}" -eq 0 ] && [ "${ELAPSED}" -ge "${SAMPLE_AT_S}" ]; then
    snapshot   "t${ELAPSED}s"
    perf_sample "t${ELAPSED}s"
    sampled=1
  fi

  if [ "${ELAPSED}" -ge "${TIMEOUT_S}" ]; then
    echo "[$(date +%H:%M:%S)] timeout at t=${ELAPSED}s — final snapshot then SIGKILL"
    snapshot "final"
    kill -KILL "${BM_PID}" 2>/dev/null || true
    sleep 2
    break
  fi

  sleep 30
done

wait "${BM_PID}" 2>/dev/null
EXIT_CODE=$?
TOTAL_S=$(( $(date +%s) - START_S ))
echo
echo "=== done ==="
echo "exit_code   : ${EXIT_CODE}"
echo "wall_secs   : ${TOTAL_S}"
echo "bm log      : ${LOG}"
echo "captures dir: ${OUT_DIR}"
echo
echo "if hung (exit_code 137 / wall_secs >= ${TIMEOUT_S}):"
echo "  less ${OUT_DIR}/gdb_threads_*.txt   # look for back-pressure call stacks"
echo "  perf report -i ${OUT_DIR}/perf_*.data --no-children   # most-on-CPU userspace stacks"
