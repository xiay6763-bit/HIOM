#!/usr/bin/env bash
# S5a: recovery-time vs dataset-size N — fresh-process cold-vs-cold.
#
# The paper's C3 figure (recovery_vs_n): O(unsafe-suffix) HiOM vs O(N) Viper,
# both K8/V200, measured on the SAME hiom_crash_recovery_bm harness under the
# re-frozen core (CORE=220fb60).
#
# 口径 (the methodology fixed 2026-07-13, see design/HIOM.md option-(a)):
#   - EACH sample is its OWN fresh process, ITERS=1. A long-lived parent warms
#     the heap / PM DIMMs / thread pool across iterations (iter0 8.6s vs iter1/2
#     2.7s at 100M) — a benchmark artifact. Real recovery = a brand-new process
#     cold-opening after a crash, so only a fresh iter0 counts. We take REPS
#     independent fresh processes per (system, N) and report the median.
#   - HiOM = real fork()+SIGKILL mid-insert: recovery replays the cadence-bounded
#     unsafe suffix (durable checkpoint frontier -> current block), lazy-repair
#     lock path (lock_scan OFF), value-independent + ~constant in N.
#   - Viper = clean-restart rebuild (its own paper's methodology, option a):
#     child creates+prefills N+clean-exits, parent cold-reopens = O(N) CCEH
#     rebuild. Real SIGKILL for Viper is available via HIOM_CR_VIPER_SIGKILL=1
#     but has a rare pre-existing recovery race (NOT HiOM's, NOT lock-intent's —
#     see memory hiom-viper-baseline-crash-recovery-race); clean-restart is the
#     fair time comparison.
#
# Output: results/recovery/vsn/<system>_<N>_rep<r>.csv (one fresh cold sample
# each). Plot with eval/recovery_vs_n_crash_plot.py.
#
# Usage: benchmark/run_recovery_vs_n.sh
# Env: RVN_NS (default "1000000 10000000 33000000 100000000"),
#      RVN_REPS (default 3), RVN_OUT_DIR (default results/recovery/vsn),
#      RVN_TIMEOUT_S (default 1200, per sample).
set -uo pipefail
cd "$(dirname "$0")/.."   # repo root

BM=./build/benchmark/hiom_crash_recovery_bm
OUT_DIR="${RVN_OUT_DIR:-results/recovery/vsn}"
NS="${RVN_NS:-1000000 10000000 33000000 100000000}"
REPS="${RVN_REPS:-3}"
TIMEOUT_S="${RVN_TIMEOUT_S:-1200}"
mkdir -p "$OUT_DIR"

run_sample() {  # $1=system(hiom|viper) $2=N $3=rep
    local sys="$1" N="$2" rep="$3"
    local csv="${OUT_DIR}/${sys}_${N}_rep${rep}.csv"
    if [ -s "$csv" ]; then echo "  SKIP-EXISTS ${sys} N=${N} rep=${rep}"; return 0; fi
    local seed=$((12345 + rep * 1000))
    echo "  RUN ${sys} N=${N} rep=${rep} seed=${seed} -> ${csv}"
    set +e
    HIOM_CR_SYSTEM="$sys" HIOM_CR_N="$N" HIOM_CR_ITERS=1 \
        HIOM_CR_WORKLOAD=insert HIOM_CR_THREADS=4 \
        HIOM_CR_MIN_MS=5 HIOM_CR_MAX_MS=60 HIOM_CR_SEED="$seed" \
        HIOM_CR_CSV="$csv" \
        timeout --signal=KILL "$TIMEOUT_S" "$BM" >"${csv%.csv}.log" 2>&1
    local rc=$?
    set -e
    if [ "$rc" -ne 0 ] || [ ! -s "$csv" ]; then
        echo "    FAIL/TIMEOUT rc=${rc} ${sys} N=${N} rep=${rep}"
        [ -f "$csv" ] && mv "$csv" "${csv}.rc${rc}"
        return 1
    fi
    # Echo the one-line total for live progress.
    tail -1 "$csv" | awk -F, '{printf "    total_ms=%s recovered=%s lost=%s\n", $(NF-4), $(NF-2), $(NF-1)}'
}

fails=0
for N in $NS; do
    echo "=== N=${N} ==="
    for rep in $(seq 0 $((REPS-1))); do
        run_sample hiom  "$N" "$rep" || fails=$((fails+1))
        run_sample viper "$N" "$rep" || fails=$((fails+1))
    done
done
echo; echo "=== recovery-vs-N sweep done, ${fails} failed sample(s), out=${OUT_DIR} ==="
echo "Next: python3 eval/recovery_vs_n_crash_plot.py ${OUT_DIR}"
exit $((fails > 0))
