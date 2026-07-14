#!/usr/bin/env bash
# S5b: unsafe-suffix characterization — U distribution + T = alpha + beta*U fit.
#
# The companion figure to recovery_vs_n (C3): now that the O(P) lock scan is
# gone (bounded-lock-set protocol, lazy repair) and tail replay truly dominates
# total recovery, this figure substantiates the O(unsafe suffix) claim directly:
#   left  panel: CDF of U = unsafe_suffix_blocks per crash workload — U is set
#                by WRITE intensity at crash time (insert t16 >> YCSB-A t8
#                updates-in-place ~= YCSB-B t8), not by N.
#   right panel: T_total vs U scatter pooled across workloads + linear fit
#                T = alpha + beta*U (alpha = cold-open floor, beta = per-block
#                replay cost). Update-heavy crashes cluster at U ~= 0 and anchor
#                alpha; insert crashes span U and pin beta.
#
# 口径 = S5a / S1-S2 gates: every sample its own fresh process (ITERS=1,
# cold-vs-cold), real fork()+SIGKILL, N=10M K8/V200, kill delay 5-200 ms,
# lazy-repair lock path, lost==0/mismatch==0 gate enforced by the plotter.
# Workload thread counts follow the S1 gate: insert t16, ycsb_a t8, ycsb_b t8.
#
# Usage:
#   benchmark/run_unsafe_suffix.sh pilot   # 5 reps per workload (validate)
#   benchmark/run_unsafe_suffix.sh full    # 25 reps per workload
# Env: US_OUT_DIR (default results/recovery/unsafe_suffix), US_N (default 10M),
#      US_REPS (overrides mode), US_TIMEOUT_S (default 600 per sample).
set -uo pipefail
cd "$(dirname "$0")/.."   # repo root

BM=./build/benchmark/hiom_crash_recovery_bm
OUT_DIR="${US_OUT_DIR:-results/recovery/unsafe_suffix}"
N="${US_N:-10000000}"
TIMEOUT_S="${US_TIMEOUT_S:-600}"

mode="${1:-pilot}"
case "$mode" in
  pilot) REPS="${US_REPS:-5}" ;;
  full)  REPS="${US_REPS:-25}" ;;
  *) echo "usage: $0 pilot|full" >&2; exit 2 ;;
esac
mkdir -p "$OUT_DIR"

threads_for() {  # S1-gate thread counts
    case "$1" in
        insert) echo 16 ;;
        ycsb_a|ycsb_b) echo 8 ;;
    esac
}

run_sample() {  # $1=workload $2=rep
    local wl="$1" rep="$2"
    local csv="${OUT_DIR}/hiom_${wl}_rep${rep}.csv"
    if [ -s "$csv" ]; then echo "  SKIP-EXISTS ${wl} rep=${rep}"; return 0; fi
    local seed=$((977 + rep * 131))   # decorrelated from the S5a seed series
    local t; t=$(threads_for "$wl")
    echo "  RUN ${wl} t=${t} rep=${rep} seed=${seed}"
    set +e
    HIOM_CR_SYSTEM=hiom HIOM_CR_N="$N" HIOM_CR_ITERS=1 \
        HIOM_CR_WORKLOAD="$wl" HIOM_CR_THREADS="$t" \
        HIOM_CR_MIN_MS=5 HIOM_CR_MAX_MS=200 HIOM_CR_SEED="$seed" \
        HIOM_CR_CSV="$csv" \
        timeout --signal=KILL "$TIMEOUT_S" "$BM" >"${csv%.csv}.log" 2>&1
    local rc=$?
    set -e
    if [ "$rc" -ne 0 ] || [ ! -s "$csv" ]; then
        echo "    FAIL/TIMEOUT rc=${rc} ${wl} rep=${rep}"
        [ -f "$csv" ] && mv "$csv" "${csv}.rc${rc}"
        return 1
    fi
    tail -1 "$csv" | awk -F, \
        '{printf "    delay=%s U=%s replayed=%s total=%.1f lost=%s\n", $4, $12, $14, $22, $25}'
}

fails=0
for rep in $(seq 0 $((REPS-1))); do          # round-robin so partial runs cover all workloads
    for wl in insert ycsb_b ycsb_a; do
        run_sample "$wl" "$rep" || fails=$((fails+1))
    done
done
echo; echo "=== unsafe-suffix ${mode} done, ${fails} failed sample(s), out=${OUT_DIR} ==="
echo "Next: python3 eval/unsafe_suffix_plot.py ${OUT_DIR}"
exit $((fails > 0))
