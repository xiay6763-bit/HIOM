#!/usr/bin/env bash
#
# YCSB workload generator with optional size + dual-narrative ops control.
#
# Legacy usage (no args): generates the original 10M-prefill + 5M-ops
# workloads (5050_uniform, 5050_zipf, 1090_uniform, 1090_zipf,
# 100r_uniform, 100r_zipf) under the unsuffixed filenames.
#
# Scaling-sweep usage:
#   ./generate_ycsb.sh <size_M>
#       Generates ycsb_prefill_${size}M.dat + ycsb_wl_100r_zipf_${size}M.dat
#       (5M ops) + ycsb_wl_100r_uniform_${size}M.dat (100M ops, the "stress"
#       narrative — enough unique reads to cross the 33M HotTier capacity).
#       Skips files already on disk (idempotent).
#
# Optional second arg overrides the uniform-stress ops count
# (default 100, meaning 100M operations). Pass `5` to get a non-stress
# uniform workload identical in size to zipf.
#
# Examples:
#   ./generate_ycsb.sh            # legacy 10M default + all 6 mix workloads
#   ./generate_ycsb.sh 50         # 50M prefill + zipf(5M ops) + uniform(100M ops)
#   ./generate_ycsb.sh 50 5       # 50M prefill + zipf(5M ops) + uniform(5M ops, no stress)
set -e

SIZE_M=${1:-}
UNIFORM_OPS_M=${2:-100}

BASE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
YCSB_DIR="${BASE_DIR}/ycsb-0.17.0"
DATA_DIR="/pmem0/ycsb_data"
CFG_DIR="${BASE_DIR}/config"
PREFILL_CONF="${CFG_DIR}/ycsb_prefill.conf"

mkdir -p "${DATA_DIR}"

if [ ! -d "${YCSB_DIR}" ]; then
  echo "Error: YCSB directory not found at ${YCSB_DIR}" >&2
  exit 1
fi

# Auto-detect python
if command -v python3 &> /dev/null; then PYTHON_CMD="python3"
else PYTHON_CMD="python"
fi

# Generate one workload (raw text → binary) idempotently.
# Args: workload_name ops_M [out_suffix]
gen_one_workload() {
  local wl="$1" ops_m="$2" suffix="$3"
  local out_bin="${DATA_DIR}/ycsb_wl_${wl}${suffix}.dat"
  if [ -s "${out_bin}" ]; then
    echo "SKIP ${wl}${suffix} — binary already exists ($(ls -lh "${out_bin}" | awk '{print $5}'))"
    return
  fi
  local raw="${DATA_DIR}/raw_ycsb_wl_${wl}${suffix}.dat"
  local op_prop=""; [ -n "${ops_m}" ] && op_prop="-p operationcount=${ops_m}000000"
  local size_prop=""; [ -n "${SIZE_M}" ] && size_prop="-p recordcount=${SIZE_M}000000"
  echo "GEN ${wl}${suffix} (ops=${ops_m}M, recordcount=${SIZE_M:-default})..."
  (cd "${YCSB_DIR}" && ./bin/ycsb run basic \
        -P "${PREFILL_CONF}" \
        -P "${CFG_DIR}/ycsb_${wl}.conf" \
        ${op_prop} ${size_prop} -s > "${raw}")
  ${PYTHON_CMD} "${BASE_DIR}/convert_ycsb.py" "${raw}" "${out_bin}"
  rm "${raw}"
  echo "  → ${out_bin} ($(ls -lh "${out_bin}" | awk '{print $5}'))"
}

# Generate prefill idempotently. Arg: suffix (empty for default 10M).
gen_prefill() {
  local suffix="$1"
  local out_bin="${DATA_DIR}/ycsb_prefill${suffix}.dat"
  if [ -s "${out_bin}" ]; then
    echo "SKIP prefill${suffix} — binary already exists ($(ls -lh "${out_bin}" | awk '{print $5}'))"
    return
  fi
  local raw="${DATA_DIR}/raw_prefill${suffix}.dat"
  local size_prop=""; [ -n "${SIZE_M}" ] && size_prop="-p recordcount=${SIZE_M}000000"
  echo "GEN prefill${suffix} (recordcount=${SIZE_M:-default})..."
  (cd "${YCSB_DIR}" && ./bin/ycsb load basic -P "${PREFILL_CONF}" ${size_prop} -s > "${raw}")
  ${PYTHON_CMD} "${BASE_DIR}/convert_ycsb.py" "${raw}" "${out_bin}"
  rm "${raw}"
  echo "  → ${out_bin} ($(ls -lh "${out_bin}" | awk '{print $5}'))"
}

if [ -z "${SIZE_M}" ]; then
  # Legacy invocation — original 6-workload mix at 10M prefill / 5M ops.
  gen_prefill ""
  for wl in 5050_uniform 5050_zipf 1090_uniform 1090_zipf 100r_uniform 100r_zipf \
            a_uniform a_zipf b_uniform b_zipf; do
    gen_one_workload "${wl}" 5 ""
  done
else
  # Scaling-sweep invocation — prefill + read-only / mixed / standard YCSB.
  #   - 100r_zipf   : 5M ops (low-pressure read-only baseline)
  #   - 100r_uniform: 100M ops by default (stress, crosses HotTier capacity)
  #   - YCSB-A/B    : 5M ops each (read+update mix, the standard mix story)
  SUFFIX="_${SIZE_M}M"
  gen_prefill "${SUFFIX}"
  gen_one_workload "100r_zipf"    5                  "${SUFFIX}"
  gen_one_workload "100r_uniform" "${UNIFORM_OPS_M}" "${SUFFIX}"
  gen_one_workload "a_zipf"       5                  "${SUFFIX}"
  gen_one_workload "a_uniform"    5                  "${SUFFIX}"
  gen_one_workload "b_zipf"       5                  "${SUFFIX}"
  gen_one_workload "b_uniform"    5                  "${SUFFIX}"
fi

echo "DONE."
