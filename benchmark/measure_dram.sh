#!/bin/bash
# measure_dram.sh — white-box index-DRAM per dataset size.
#
# Reports each fixture's own index DRAM via fixture_dram_bytes() (the
# `fixture_dram_mb` counter, commit 88ff547), which is computed from sizeof
# of the live structures — exact and independent of workload / thread count.
# So one t=1 run per (fixture, size) suffices, and we shrink the timed loop
# to a token YCSB_OPS_LIMIT just to reach report_mem; the full prefill is
# kept so Viper's CCEH splits realistically.
#
# HiOM's HotTier DRAM is capacity-bound (constant ≈272 MB regardless of how
# many entries land), so HiOM at 50M is intentionally omitted: its prefill
# livelocks past the HotTier cap (the known M4 back-pressure limitation),
# and the DRAM number would be the same constant anyway.
#
# Per CLAUDE.md this script never rm's /pmem0 — each fixture's InitMap does
# its own prefix-guarded cleanup. Output (JSON + logs) goes to /tmp only.
set -uo pipefail

BM="${YCSB_BM:-/root/viper/build/benchmark/ycsb_bm}"
OUT="${OUT_DIR:-/tmp/dram_matrix}"
PER_CELL_TIMEOUT_S="${PER_CELL_TIMEOUT_S:-900}"
mkdir -p "$OUT"

SIZES="1M 10M 33M 50M"
SUMMARY="$OUT/summary.txt"
: >"$SUMMARY"

run_cell() {
    local fx="$1" size="$2"
    local json="$OUT/${fx}_${size}.json"
    local log="$OUT/${fx}_${size}.log"
    echo ">>> ${fx}Fixture ${size} (t=1, ops-limited) ..."
    YCSB_SIZE_TAG="$size" YCSB_OPS_LIMIT=10000 \
        timeout --signal=KILL "$PER_CELL_TIMEOUT_S" \
        "$BM" \
        --benchmark_filter="${fx}Fixture.*100r_zipf_tp.*threads:1\$" \
        --benchmark_out="$json" --benchmark_out_format=json \
        >"$log" 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "${fx} ${size}: FAILED/TIMEOUT (rc=$rc)" | tee -a "$SUMMARY"
        return
    fi
    # White-box value is identical across reps; take the first occurrence.
    local val
    val=$(grep -oE '"fixture_dram_mb": *[0-9.eE+-]+' "$json" | head -1 \
          | grep -oE '[0-9.eE+-]+$')
    echo "${fx} ${size}: fixture_dram_mb=${val}" | tee -a "$SUMMARY"
}

for size in $SIZES; do
    run_cell Viper "$size"
    if [ "$size" != "50M" ]; then
        run_cell HiOM "$size"
    else
        echo "HiOM 50M: SKIPPED (prefill livelock; DRAM constant ≈ HiOM 33M)" \
            | tee -a "$SUMMARY"
    fi
done

echo ""
echo "===== SUMMARY ====="
cat "$SUMMARY"
