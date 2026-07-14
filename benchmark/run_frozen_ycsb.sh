#!/usr/bin/env bash
#
# Frozen YCSB workload-spectrum driver (paper figure ycsb_workload_spectrum.pdf).
#
# Produces the 2×3 spectrum: 4 systems × 3 workloads (YCSB-C=100r, YCSB-B=b,
# YCSB-A=a) × 2 distributions (uniform, zipf) × 6 thread points (1,2,4,8,16,24)
# × 3 reps. One JSON per (fixture, workload, dist) cell — all 6 thread points
# in a single process so the shared prefill is loaded once per cell rather than
# per point.
#
# Frozen configuration (recorded verbatim into the run manifest):
#   CORE_COMMIT   b11c861  (read-path retry across in-place-update lock window;
#                 also incl. pending-ring backpressure 48a394e, num_used_blocks
#                 durable HWM 70e88e3, and the stale-lock fix)
#   EVAL_COMMIT   296e86c  (read-update race regression + fp64 scanner; also
#                 incl. pending-ring stall telemetry + this frozen harness)
#   K/V           KeyType8 / ValueType200
#   prefill       full 10M (YCSB_SIZE_TAG=10M => ycsb_prefill_10M.dat)
#   threads       1,2,4,8,16,24   (READ_ARGS for 100r; A/B use GENERAL_ARGS
#                 1..36 so we filter to threads:(1|2|4|8|16|24) to drop 32/36)
#   repetitions   3   (registered in ycsb_bm.cpp; do NOT pass
#                 --benchmark_repetitions, it would override the registration)
#   HIOM_FLUSHERS 2
#   prefault      OFF (VIPER_PREFAULT unset)
#   GET_BATCH     16 (HIOM_GET_BATCH=16 — the group-prefetch read path is part
#                 of HiOM's method, so HiOM's frozen config runs with it ON.
#                 The env is read only by HiOMFixture; baselines ignore it.
#                 HiOM cells measured with GET_BATCH OFF are archived under
#                 archive_get_batch_off/ as the ablation.)
#
# The core/eval implementation is frozen; this driver only *selects and runs*
# already-registered benchmarks. It writes nothing into include/viper/**.
#
# PM artefacts: each fixture manages its own /pmem0 pool under its InitMap /
# DeInitMap (prefix-guarded cleanup — CLAUDE.md /pmem0 rule). This driver never
# rm's /pmem0 from the shell.
#
# Usage:
#   ./run_frozen_ycsb.sh --smoke     # 3 HiOM cells (100r/a/b uniform), t=1 only
#   ./run_frozen_ycsb.sh --dry-run   # print the full plan, run nothing
#   ./run_frozen_ycsb.sh             # full 24-cell spectrum
# Env overrides:
#   FR_FIXTURES   default "ViperFixture HiOMFixture DashFixture CcehFixture"
#   FR_WORKLOADS  default "100r a b"
#   FR_DISTS      default "uniform zipf"
#   FR_OUT_DIR    default results/frozen/ycsb_spectrum
#   FR_PER_CELL_TIMEOUT_S  default 5400
set -e

YCSB_BM="/root/viper/build/benchmark/ycsb_bm"
OUT_DIR="${FR_OUT_DIR:-/root/viper/results/frozen/ycsb_spectrum}"
INVALID_DIR="${OUT_DIR}/invalid"
SIZE=10
THREAD_FILTER='threads:(1|2|4|8|16|24)$'
PER_CELL_TIMEOUT_S="${FR_PER_CELL_TIMEOUT_S:-5400}"

CORE_COMMIT="220fb60"
EVAL_COMMIT="1c660d6"

IFS=' ' read -ra FIXTURES  <<< "${FR_FIXTURES:-ViperFixture HiOMFixture DashFixture CcehFixture}"
IFS=' ' read -ra WORKLOADS <<< "${FR_WORKLOADS:-100r a b}"
IFS=' ' read -ra DISTS     <<< "${FR_DISTS:-uniform zipf}"

DRY_RUN=0
SMOKE=0
for arg in "$@"; do
  case "${arg}" in
    --dry-run) DRY_RUN=1 ;;
    --smoke)   SMOKE=1 ;;
    *) echo "Unknown arg: ${arg}" >&2; exit 2 ;;
  esac
done

if [ "${SMOKE}" -eq 1 ]; then
  FIXTURES=(HiOMFixture)
  WORKLOADS=(100r a b)
  DISTS=(uniform)
  THREAD_FILTER='threads:1$'
  OUT_DIR="${OUT_DIR}/smoke"
  INVALID_DIR="${OUT_DIR}/invalid"
  echo "SMOKE mode: HiOM only, 100r/a/b uniform, t=1 → ${OUT_DIR}"
fi

mkdir -p "${OUT_DIR}" "${INVALID_DIR}"

# ---- run manifest (frozen config + environment provenance) -----------------
MANIFEST="${OUT_DIR}/run_manifest.txt"
{
  echo "# Frozen YCSB spectrum run manifest"
  echo "core_commit=${CORE_COMMIT}"
  echo "eval_commit=${EVAL_COMMIT}"
  echo "git_head=$(git -C /root/viper rev-parse HEAD 2>/dev/null || echo '?')"
  echo "git_dirty=$(git -C /root/viper status --porcelain --untracked-files=no | wc -l) tracked-file(s) modified"
  echo "date=$(date -Iseconds)"
  echo "hostname=$(hostname)"
  echo "kernel=$(uname -r)"
  echo "compiler=$(gcc --version | head -1)"
  echo "kv=KeyType8/ValueType200"
  echo "prefill=${SIZE}M (full)"
  echo "threads=1,2,4,8,16,24"
  echo "repetitions=3 (registered)"
  echo "hiom_flushers=${HIOM_FLUSHERS:-2}"
  echo "prefault=${VIPER_PREFAULT:-OFF}"
  echo "get_batch=${HIOM_GET_BATCH:-16}"
  echo "fixtures=${FIXTURES[*]}"
  echo "workloads=${WORKLOADS[*]}"
  echo "dists=${DISTS[*]}"
  echo "numa=$(numactl --show 2>/dev/null | grep -E '^(nodebind|physcpubind)' | tr '\n' ' ' || echo 'numactl-not-run')"
} > "${MANIFEST}"
echo "=== manifest → ${MANIFEST} ==="
cat "${MANIFEST}"
echo

# Freeze the knobs the manifest claims (so a stray shell env can't silently
# change the run out from under the recorded config).
export HIOM_FLUSHERS="${HIOM_FLUSHERS:-2}"
unset VIPER_PREFAULT
export HIOM_GET_BATCH="${HIOM_GET_BATCH:-16}"

total=0; skip=0; run=0
for workload in "${WORKLOADS[@]}"; do
  for dist in "${DISTS[@]}"; do
    for fixture in "${FIXTURES[@]}"; do
      total=$((total + 1))
      wl="${workload}_${dist}"
      out="${OUT_DIR}/${fixture}_${wl}_${SIZE}M_tp.json"
      if [ -s "${out}" ]; then
        skip=$((skip + 1))
        echo "SKIP-EXISTS ${fixture} ${wl} (${out})"
        continue
      fi
      filter="${fixture}.*${workload}_${dist}_tp.*${THREAD_FILTER}"
      cmd="YCSB_SIZE_TAG=${SIZE}M HIOM_FLUSHERS=${HIOM_FLUSHERS} ${YCSB_BM} --benchmark_filter='${filter}' --benchmark_out='${out}' --benchmark_out_format=json"
      echo "RUN  ${fixture} ${wl} → ${out} (timeout ${PER_CELL_TIMEOUT_S}s)"
      if [ "${DRY_RUN}" -eq 1 ]; then
        echo "     ${cmd}"
        continue
      fi
      set +e
      timeout --signal=KILL "${PER_CELL_TIMEOUT_S}" bash -c "${cmd}" 2>&1 | tail -4
      rc=${PIPESTATUS[0]}
      set -e
      if [ "${rc}" -eq 124 ] || [ "${rc}" -eq 137 ]; then
        ts=$(date +%Y%m%d_%H%M%S)
        echo "  TIMEOUT after ${PER_CELL_TIMEOUT_S}s — moving partial to invalid/"
        [ -s "${out}" ] && mv "${out}" "${INVALID_DIR}/$(basename "${out}").timeout_${ts}"
      elif [ "${rc}" -ne 0 ]; then
        ts=$(date +%Y%m%d_%H%M%S)
        echo "  non-zero exit ${rc} — moving artifact to invalid/"
        [ -s "${out}" ] && mv "${out}" "${INVALID_DIR}/$(basename "${out}").fail_${ts}"
      else
        run=$((run + 1))
      fi
      echo
    done
  done
done

echo "=============================="
echo "  ${total} cells: ${skip} skipped, ${run} freshly run"
echo "  out=${OUT_DIR}"
echo "=============================="
[ "${DRY_RUN}" -eq 0 ] && ls -lh "${OUT_DIR}"/*.json 2>/dev/null || true
echo
echo "Next: python3 eval/validate_frozen_run.py ${OUT_DIR}"
