#!/usr/bin/env bash
# S5a (Halo line): recovery-time vs dataset-size N for the Halo baseline, on the
# SAME fresh-process cold-reopen methodology as run_recovery_vs_n.sh's Viper
# clean-restart line — so Halo drops into eval/recovery_vs_n_crash_plot.py as a
# third O(N)-rebuild line next to HiOM (O(unsafe suffix)) and Viper.
#
# Halo lives in the EXTERNAL baseline clone (benchmark/hash_api/README.md); this
# script builds benchmark/hash_api/halo_recovery.cpp against it, then sweeps.
#
# 口径: each sample = its own fresh process. halo_recovery forks a child that
# prefills N K8/V200 records + clean-closes (dtor snapshots CLHT, ROOT->clean=1);
# the parent cold-reopens (startup() O(N) snapshot restore, redo skipped) and
# times that ctor as total_recovery_ms. Gate: lost==0 && mismatch==0.
#
# Output: results/recovery/vsn/halo_<N>_rep<r>.csv  (SAME dir as hiom/viper).
# Plot:   python3 eval/recovery_vs_n_crash_plot.py results/recovery/vsn
#
# Env: RVN_NS (default "1000000 10000000 33000000 100000000"),
#      RVN_REPS (default 3), RVN_OUT_DIR (default results/recovery/vsn),
#      RVN_TIMEOUT_S (default 1800, per sample), HALO_REC_THREADS (default 16),
#      HALO_DIR (external clone, default /root/hiom-baselines/Halo).
set -uo pipefail
cd "$(dirname "$0")/.."   # repo root

HALO_DIR="${HALO_DIR:-/root/hiom-baselines/Halo}"
HALO_SRC="${HALO_DIR}/Halo"                       # nested dir with Halo.hpp/.cpp
SRC=benchmark/hash_api/halo_recovery.cpp
BIN="${HALO_DIR}/halo_recovery"
OUT_DIR="${RVN_OUT_DIR:-results/recovery/vsn}"
NS="${RVN_NS:-1000000 10000000 33000000 100000000}"
REPS="${RVN_REPS:-3}"
TIMEOUT_S="${RVN_TIMEOUT_S:-1800}"
THREADS="${HALO_REC_THREADS:-16}"
mkdir -p "$OUT_DIR"

if [ ! -d "$HALO_SRC" ] || [ ! -f "$HALO_SRC/Halo.hpp" ]; then
    echo "ERROR: Halo clone not found at ${HALO_SRC} — see benchmark/hash_api/README.md 'Reproduce'." >&2
    exit 2
fi

# 1. Build libHalo.a (from Halo.cpp) if missing, then the recovery harness.
if [ ! -f "${HALO_SRC}/libHalo.a" ]; then
    echo "=== building libHalo.a ==="
    ( cd "$HALO_SRC" && make libHalo.a ) || { echo "libHalo.a build failed" >&2; exit 2; }
fi
if [ ! -x "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
    echo "=== building halo_recovery ==="
    g++-11 -O3 -std=c++17 -march=native -mavx -mclwb -fpermissive \
        -I"$HALO_SRC" \
        -o "$BIN" "$SRC" \
        -L"$HALO_SRC" -lHalo -lpthread -lpmem \
        || { echo "halo_recovery build failed" >&2; exit 2; }
fi

run_sample() {  # $1=N $2=rep
    local N="$1" rep="$2"
    local csv="${OUT_DIR}/halo_${N}_rep${rep}.csv"
    if [ -s "$csv" ]; then echo "  SKIP-EXISTS halo N=${N} rep=${rep}"; return 0; fi
    echo "  RUN halo N=${N} rep=${rep} -> ${csv}"
    set +e
    HALO_REC_N="$N" HALO_REC_REP="$rep" HALO_REC_THREADS="$THREADS" \
        HALO_REC_PMPATH="/pmem0/halo_recovery/" HALO_REC_CSV="$csv" \
        numactl -N 0 timeout --signal=KILL "$TIMEOUT_S" "$BIN" >"${csv%.csv}.log" 2>&1
    local rc=$?
    set -e
    if [ "$rc" -ne 0 ] || [ ! -s "$csv" ]; then
        echo "    FAIL/TIMEOUT rc=${rc} halo N=${N} rep=${rep}"
        [ -f "$csv" ] && mv "$csv" "${csv}.rc${rc}"
        return 1
    fi
    tail -1 "$csv" | awk -F, '{printf "    total_ms=%s recovered=%s lost=%s mismatch=%s\n", $(NF-5), $(NF-3), $(NF-2), $(NF-1)}'
}

fails=0
for N in $NS; do
    echo "=== N=${N} ==="
    for rep in $(seq 0 $((REPS-1))); do
        run_sample "$N" "$rep" || fails=$((fails+1))
    done
done
echo; echo "=== halo recovery-vs-N sweep done, ${fails} failed sample(s), out=${OUT_DIR} ==="
echo "Next: python3 eval/recovery_vs_n_crash_plot.py ${OUT_DIR}"
exit $((fails > 0))
