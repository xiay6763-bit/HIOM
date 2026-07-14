#!/usr/bin/env bash
# S3 / E2: HotTier-capacity ablation sweep (paper figure hot_capacity_tradeoff).
#
# Purpose: the 1×3 causal-chain figure —
#   DRAM capacity → hot_hit_rate → cold-access fraction → throughput
# measured on YCSB-C (100% read), uniform + zipf, at a FIXED t=8, with HiOM in
# its frozen full configuration (GET_BATCH=16, FLUSHERS=2 — same as the frozen
# workload-spectrum figure). Viper/Dash t8 reference lines come from the frozen
# spectrum JSONs (results/frozen/ycsb_spectrum/) — same core commit, same day,
# no rerun needed.
#
# 口径 (matches design/HIOM.md Phase-1 "clean YCSB" methodology): each capacity
# point is its own process; the YCSB fixture builds the HotTier at the TARGET
# capacity before the 10M prefill, so every point is "prefill residue at target
# capacity → read-phase equilibrium" — uniform across capacities, none of the
# all_ops from-empty↔residue switch artifact. Hit rate at reads≫ws is the
# eviction-limited steady state, a true capacity signal.
#
# Capacity axis: HIOM_HOT_BUCKETS_LOG2 ∈ {14..21}; slots = 16 × buckets:
#   log2   14    15    16    17    18    19    20    21
#   slots  262k  524k  1.0M  2.1M  4.2M  8.4M  16.8M 33.6M
#   /10M   2.6%  5.2%  10%   21%   42%   84%   168%  336%
#
# Validity gates (checked by eval/hot_capacity_tradeoff_plot.py before
# plotting): read_success_rate == 1.0 and cold_miss_rate == 0.0 on every
# iteration; items_per_second > 0.
#
# Livelock caveat (2026-06-05, pre-hardening): small capacity + t≥8 could spin
# in SIEVE evict vs PINNED slots. Run `smoke` FIRST (smallest cap, t8, both
# dists, 10-min timeout) — if it hangs, the sweep design must be revisited, do
# NOT just bump the timeout.
#
# Usage:
#   benchmark/scan_hot_capacity_v2.sh smoke   # log2=14 t8 gate, both dists
#   benchmark/scan_hot_capacity_v2.sh full    # (default) 8 caps × 2 dists
# Env: HC2_OUT_DIR (default results/hot_scan_v2), HC2_LOG2S, HC2_TIMEOUT_S.
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

BM=./build/benchmark/ycsb_bm
OUT_DIR="${HC2_OUT_DIR:-results/hot_scan_v2}"
TIMEOUT_S="${HC2_TIMEOUT_S:-1800}"
LOG2S="${HC2_LOG2S:-14 15 16 17 18 19 20 21}"

# Frozen HiOM full config — identical to run_frozen_ycsb.sh.
export YCSB_SIZE_TAG="${YCSB_SIZE_TAG:-10M}"
export HIOM_FLUSHERS="${HIOM_FLUSHERS:-2}"
export HIOM_GET_BATCH="${HIOM_GET_BATCH:-16}"
unset VIPER_PREFAULT HIOM_WARM_HOT   # from-equilibrium口径, no prefault

mode="${1:-full}"
mkdir -p "${OUT_DIR}"

manifest="${OUT_DIR}/run_manifest.txt"
{
  echo "# HotTier capacity sweep (S3/E2) manifest"
  echo "git_head=$(git rev-parse HEAD 2>/dev/null || echo '?')"
  echo "git_dirty=$(git status --porcelain --untracked-files=no | wc -l) tracked-file(s) modified"
  echo "date=$(date -Iseconds)"
  echo "kv=KeyType8/ValueType200  prefill=10M  threads=8  reps=3(registered)"
  echo "hiom_flushers=${HIOM_FLUSHERS}  get_batch=${HIOM_GET_BATCH}  prefault=OFF  warm_hot=OFF"
  echo "log2s=${LOG2S}"
  echo "reference_lines=results/frozen/ycsb_spectrum (Viper/Dash t8 medians)"
} > "${manifest}"
cat "${manifest}"; echo

run_cell() {  # $1=log2  $2=dist
  local l="$1" dist="$2"
  local out="${OUT_DIR}/hiom_log2_${l}_${dist}.json"
  if [ -s "${out}" ]; then echo "SKIP-EXISTS log2=${l} ${dist}"; return 0; fi
  echo "RUN log2=${l} ${dist} -> ${out}"
  set +e
  timeout --signal=KILL "${TIMEOUT_S}" env HIOM_HOT_BUCKETS_LOG2="${l}" \
    "$BM" --benchmark_filter="HiOMFixture.*100r_${dist}_tp.*threads:8\$" \
          --benchmark_out="${out}" --benchmark_out_format=json 2>&1 | tail -2
  local rc=${PIPESTATUS[0]}
  set -e
  if [ "${rc}" -ne 0 ]; then
    echo "  FAIL/TIMEOUT rc=${rc} log2=${l} ${dist} — quarantining"
    mkdir -p "${OUT_DIR}/invalid"
    [ -s "${out}" ] && mv "${out}" "${OUT_DIR}/invalid/$(basename "${out}").rc${rc}"
    return 1
  fi
}

fails=0
if [ "${mode}" = smoke ]; then
  for dist in uniform zipf; do run_cell 14 "${dist}" || fails=$((fails+1)); done
else
  for l in ${LOG2S}; do
    for dist in uniform zipf; do run_cell "${l}" "${dist}" || fails=$((fails+1)); done
  done
fi
echo; echo "=== sweep done, ${fails} failed cell(s), out=${OUT_DIR} ==="
echo "Next: python3 eval/hot_capacity_tradeoff_plot.py ${OUT_DIR}"
exit $((fails > 0))
