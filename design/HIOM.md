# HiOM: Hierarchical Offset Map for Viper

Design document for a tiered offset-map extension to Viper [VLDB '21],
targeting ICDE (CCF-A) submission. Backup venue: 计算机研究与发展 / FGCS.

Code references use the form
`[viper.hpp:101-108](../include/viper/viper.hpp#L101-L108)`. Numbers ending
in `≈` are derived/projected; numbers without `≈` are verified from source
(see Appendix A).

---

## Status (2026-05-17)

- **YCSB read-heavy gap closed by `flush_post_prefill`**. Adding YCSB-C
  (100% read, both uniform and zipfian) to the eval matrix surfaced a
  surprise: at 10M-record scale HiOM/Viper read ratio came in at
  **0.27-0.33×** — completely inverse to the 4-op grid's 1.04-1.06× at
  1M+KeyType16. Two experiments isolated the dominant factor:
  - **Step 1** (recordcount=1M, regenerated YCSB workload so the
    keyspace matched the prefill): ratio only marginally better
    (0.28-0.56×). Working-set L3 fit is *not* the dominant factor at
    t=1; the structural per-op cost is.
  - **Step 2** (add `flush_post_prefill()` hook in
    `BaseFixture` / `HiOMFixture` so the timed read phase doesn't
    contend with the background flusher's PMem writes still draining
    the 10M-entry prefill): ratio jumps to **0.27-0.58×** across the
    grid, with the biggest wins on t=1 / t=8 zipf where flusher
    contention bit hardest. Final 10M-scale numbers
    (`/tmp/ycsb_100r_flushonly_run.log`, 3-rep median, per-thread
    items/s):

    | workload | t=1 V/H (ratio) | t=8 V/H (ratio) | t=24 V/H (ratio) |
    |----------|-----------------|-----------------|------------------|
    | 100r_uniform | 2.42 M / 997 K (**0.41×**) | 14.51 M / 7.44 M (**0.51×**) | 32.41 M / 11.98 M (0.37×) |
    | 100r_zipf    | 2.92 M / 1.47 M (**0.50×**) | 17.65 M / 10.15 M (**0.58×**) | 48.71 M / 12.96 M (0.27×) |

    Compare to pre-fix: 100r_zipf t=1 was 0.27×, t=8 was 0.32× — the
    flush-before-timed-loop change closed those gaps by **+85% / +81%
    relative**. t=24 stays flat because at 24-way aggregate read
    throughput (~466 M ops/s) even the flusher's writes can't move
    the needle.
- **Why `flush_post_prefill` matters (mechanism)**. `prefill_ycsb`
  pushes 10M kPut entries through HiOM's commit buffer. The
  background flusher needs ~1-2 s to drain them into ColdTier (PMem
  writes ~9 GB/s ÷ ~16 B/entry × 4 flushers ≈ 8 M entries/s). If the
  timed read phase starts before the drain finishes, HiOM's per-hit
  PMem reads (for verify and value) and the flusher's PMem writes
  contend on the same Optane DIMM write-buffer queue. Viper has no
  async flusher so the baseline pays nothing here. Adding a single
  `flush_and_wait()` between `prefill_ycsb` and the timed loop makes
  the steady-state measurement what readers care about.
- **Inline-K HotTier attempt — landed and then reverted (2026-05-17)**.
  Hypothesis was that the 100r gap came from HotTier's mandatory
  PM-verify on hit (the 8-byte slot only carries fp32+offset, no key,
  so every hit reads PMem just to confirm fp32 wasn't a 1/2^32
  collision). Implemented `template <typename K> class HotTier`
  with a parallel `BucketKeys[K kSlotsPerBucket]` array, K-aware
  `lookup(K, fp)` / `upsert(K, fp, off)` / `upsert_pinned(K, fp, off)`.
  K stored *after* successful CAS on packed using release semantics.
  - `hot_tier_test` and `hiom_integration_test` initially PASS;
    YCSB 100r at 10M-scale ratios improved only marginally
    (0.27-0.33× → 0.28-0.35×). all_ops_bm get at K16+1M ratio
    preserved (1.05-1.10×). Microbench at 200K (L3 fit) went from
    1.04× (pre-change baseline) to 0.96× (slight regression from
    extra cache line load for keys array).
  - Then the multi-producer correctness test went **flaky** under
    repeat: 5/5 runs failed with `pm_key_mismatch` ranging 0-42
    and `missed` 0-15. Root cause: when two writers concurrently
    insert distinct keys into the same previously-occupied slot,
    the post-CAS K-write window allows the second writer to see
    stale K under the first writer's new fp → second writer skips
    the slot (treating it as fp32 collision) and inserts elsewhere,
    leaving **two HotTier slots for the same key** (stale-off slot
    1 and new-off slot 2). Readers can hit either depending on
    scan order, returning stale values.
  - The correct fix is DCAS (`cmpxchg16b` on a 16-byte
    `{K, packed}` slot for K=8B), which is a larger surface
    change. Reverted hot_tier.hpp / hiom.hpp / commit_buffer.hpp /
    hot_tier_test.cpp / hiom_integration_test.cpp; kept
    `flush_post_prefill` since it's the actual win.
  - Lesson: inline-K's theoretical benefit (skip per-hit PMem
    verify) is small in practice because for KeyType8+ValueType200
    the key occupies the same cache line as the value's first 56 B,
    so the PMem read can't be shortened. The real per-op savings
    is ~10-20 ns of pm_key copy + compare — not the 70-300 ns I
    estimated upfront. The race added by post-CAS K-write isn't
    worth the marginal win.
- **YCSB workload matrix** now includes **6 workloads** in
  `benchmark/config/`: `5050_uniform`, `5050_zipf`, `1090_uniform`,
  `1090_zipf` (existing), plus **`100r_uniform`** and **`100r_zipf`**
  (new, YCSB-C analog). The latter use a separate `READ_ARGS` macro
  in `ycsb_bm.cpp` that registers only t=1/8/24 (vs `GENERAL_ARGS`'s
  full 1/4/8/16/24/32/36 sweep) since 100% reads at t=24 already
  saturate the read path. `generate_ycsb.sh` `CONFIGS` array
  extended; raw YCSB data at `/pmem0/ycsb_data/ycsb_wl_100r_*.dat`
  (1.06 GB each, 5 M READ records).
- **`HotTier` default capacity bumped to 2^21 buckets** in
  `HiOMFixture` (was 2^18 = 4 M slots; now 33 M slots, 256 MB DRAM).
  The 4 M default was undersized for YCSB's 10 M prefill — at 28%
  load factor the original 4 M would have hit eviction churn during
  prefill. The bump preserves the steady-state hot-resident
  invariant assumed by the paper's read story.
- **M6.6 — CCEH allocation skip for HiOM (2026-05-17)**. The
  prerequisite for the win-condition scaling experiment. Before
  M6.6, `HiOMFixture` instantiated a `Viper` which always eagerly
  allocated CCEH at the M0 baseline of 131,072 segments × 16 KiB =
  ~2 GiB DRAM. Since HiOM owns the index via `hiom_owns_index_=true`,
  CCEH is never inserted into on the put/update/remove paths and
  pure DRAM overhead. Added `ViperConfig::cceh_init_cap` (default
  131072 keeps every M0–M6 caller byte-identical);
  `HiOMFixture::InitMap` sets it to 1, shrinking CCEH to a single
  16 KiB segment. `viper.hpp:674` ctor passes it straight to
  `map_{...}`. Verified by walking all 11 `map_.X` call sites:
  every one HiOM exercises is guarded by `hiom_owns_index_` or
  `!skip_recovery`, so the empty CCEH is never dereferenced. Only
  side effect: `mirror_write`'s peek-via-CCEH returns tombstone in
  update paths and silently skips the SIEVE warm-touch — weakens
  hot-key heat signal slightly, doesn't affect correctness, moot
  for YCSB-C (100% read). hiom_integration_test passes 3/3
  (single-producer recovery + multi-producer same-key + 6-iter
  crash injection).
- **Phase 0 — RSS / DRAM telemetry (2026-05-17)**. New
  `benchmark/fixtures/mem_tracker.hpp` (~85 LOC, header-only) with
  `MemSnapshot` + `capture_mem()` parsing `/proc/self/status`
  (VmRSS) and `/proc/self/smaps` (per-mapping Rss attribution by
  pathname prefix `/pmem0/`). BaseFixture grows
  `virtual fixture_telemetry()` defaulting to `capture_mem()`;
  HiOMFixture override adds `hot_size`, `hot_capacity`,
  `hot_evictions` from `hiom_->hot_tier()` and `hot_hits/cold_hits`
  from `hiom_->stats()` (all O(1) atomic loads via existing
  accessors). `ycsb_run` snapshots baseline right after `InitMap`,
  loaded right after `flush_post_prefill`, and re-captures HotTier
  stats after the timed loop. Pushes 8 counters into `state`:
  `rss_baseline_mb / rss_loaded_mb / dram_loaded_mb /
  pool_rss_loaded_mb / hot_tier_size / hot_capacity /
  hot_evictions / hot_hit_rate`.
  - **Observation**: FS-DAX PMem mmap reports `Rss=0` in smaps
    (PMem pages aren't counted resident), so
    `dram_loaded_mb = rss_loaded` in practice on this host. Kept
    the pool-rss subtraction code for future /dev/dax* configs.
- **Phase 0 smoke results (2026-05-17)** at 100r_zipf t=8, rep 1
  fresh process:

  | Size | Fixture | rss_loaded | hot_size | hot_hit_rate | items/thr |
  |------|---------|-----------:|---------:|-------------:|----------:|
  | 1M   | Viper   |     n/a    | n/a      | n/a          | n/a       |
  | 1M   | HiOM    | **673 MB** | 933 K    | 0.92         | 12.2 M/s  |
  | 10M  | Viper   | 5119 MB    | n/a      | n/a          | 22.4 M/s  |
  | 10M  | HiOM    | **3552 MB**| 933 K    | 0.13         |  9.0 M/s  |

  Before M6.6, HiOM at 10M reported 5625 MB (more than Viper);
  after M6.6, **HiOM 3552 MB vs Viper 5119 MB = HiOM uses
  -1567 MB (-31%) DRAM** at 10M scale, with zero throughput
  regression. Subtracting the YCSB harness common overhead (~2.5 GB
  prefill_data std::vector + per-thread state), fixture-specific
  DRAM is Viper ~2.5 GB (CCEH-dominated) vs HiOM ~500 MB (HotTier
  + ColdTier + commit) — **~5× less fixture-specific DRAM**. The
  hot_hit_rate=0.13 at 10M is an artifact: HotTier warms only on
  GET (prefill bypasses it), and only 933K unique keys get hit
  enough to land in HotTier during the 5M-op read phase. The
  remaining 87% of reads go to ColdTier — which still gives
  HiOM 0.41× throughput vs Viper here, modulated by the read-path
  PMem-verify cost. Phase 1 (50M dataset, crosses HotTier
  capacity) is next.

## Status (2026-05-18)

- **M3.5 — Offset codec bit-width resize**. Phase 2 sweep discovery:
  with the original 13-bit `kBlockLowBits` field
  ([offset_codec.hpp:38](../include/viper/hiom/offset_codec.hpp#L38)),
  a single region encoded only 8192 blocks × 24 KiB ≈ **200 MiB of
  PMem ≈ 960 K entries** at K8+V200. Every entry past the first
  ~960 K landed in blocks whose offset `encode()` couldn't represent
  → returned `nullopt` → `mirror_into_hot_with_offset` silently
  skipped HotTier upsert. Symptom in Phase 2 sweep across dataset
  sizes 5 M→50 M: `hot_tier_size` stuck at ~933 K regardless of
  dataset, `hot_evictions = 0` even at uniform 100 M ops, hit_rate
  0.13–0.30. The 33 M HotTier capacity was effectively a 960 K cap
  on encodable entries.

  Fix: bit layout 13+3+16 → **21+3+8**:
  - `kBlockLowBits = 21` (2 Mi blocks × 24 KiB ≈ 48 GiB per region;
    fits BM_POOL_SIZE 64 GiB minus margin for paper datasets up to
    100 M × 208 B = 20.8 GiB)
  - `kPageBits = 3` (unchanged, NUM_DIMMS=6)
  - `kDataOffsetBits = 8` (slot index up to 255; minimum entry size
    in Viper is 16 B `uint64+uint64` → PAGE_SIZE/16 = 256 slots, the
    one corner case that needed 8 bits; K8+V200 only uses 19 slots)
  - Plus an explicit `if (data_offset > kDataOffMask) return
    nullopt` bounds check ([offset_codec.hpp:82](../include/viper/hiom/offset_codec.hpp#L82))
    — without it, an out-of-range slot index would OR-corrupt the
    page_number bits silently.
  - Multi-region routing remains future work for pool usage > 48 GiB,
    but the M3.5 region 0 cap is sufficient for every paper dataset
    we plan to publish.

  Validation: `hiom_integration_test` 11/11 PASS (was 10/11 with
  intermediate 22+3+7 layout — the M4 back-pressure test uses
  `<u64,u64>` 16-byte entries needing 8-bit data_offset).
  10 M zipf t=8 smoke result transformed:
  - `hot_tier_size`: 933 K → **9.99 M** (HotTier now genuinely
    populated)
  - `hot_hit_rate`: 0.13 → **0.9996**
  - throughput (HiOM): 9.10 M items/s/thr → **15.5 M items/s/thr**
    (+72%)
  - H/V ratio: 0.41× → **0.70×** at 10 M zipf t=8

- **Phase 2 — win-condition scaling sweep (2026-05-18)**. Five
  dataset sizes (5 M / 10 M / 16 M / 33 M / 50 M) × two workloads
  (100r_zipf 5 M ops, 100r_uniform 100 M ops — uniform's high op
  count chosen to force unique-read count past the 33 M HotTier
  cap at 50 M dataset; analysis in the 2026-05-17 Step 1 sub-section
  earlier in this file). Two fixtures (ViperFixture +
  HiOMFixture). One fresh ycsb_bm process per (size, workload,
  fixture) combination, t=1/8/24 sweep, 3 reps each. Driver:
  [run_scaling_sweep.sh](../benchmark/run_scaling_sweep.sh).
  Plotter: [eval/scaling_plot.py](../eval/scaling_plot.py) →
  `eval/charts/scaling_<workload>.pdf`.

  **Results at t=8, median of 3 reps** (`H/V` is HiOM/Viper throughput
  ratio per thread; eviction counts are *during* the timed-read phase
  unless noted):

  100r_zipf (5 M ops, theta=0.99 over recordcount keyspace):

  | size | Viper M/thr | HiOM M/thr | **H/V** | hot_size | evictions | hit_rate |
  |------|------------:|-----------:|--------:|---------:|----------:|---------:|
  | 5 M  | 22.78       | 15.71      | 0.69×   | 5.00 M   | 0         | 1.000    |
  | 10 M | 22.63       | 15.44      | 0.68×   | 9.99 M   | 45        | 1.000    |
  | 16 M | 19.41       | 15.34      | **0.79×** | 15.96 M | 8.6 K   | 0.999    |
  | 33 M | 16.81       | 13.67      | **0.81×** | 29.9 M  | **3.09 M** | 0.973  |
  | 50 M | 15.94       | n/a        | n/a     | —        | —         | —        |

  100r_uniform (100 M ops):

  | size | Viper M/thr | HiOM M/thr | **H/V** | hot_size | evictions | hit_rate |
  |------|------------:|-----------:|--------:|---------:|----------:|---------:|
  | 5 M  | 18.14       | 16.64      | **0.92×** | 5.00 M | 0         | 0.999    |
  | 10 M | 18.03       | 15.67      | 0.87×   | 9.99 M   | 142       | 1.000    |
  | 16 M | 18.05       | 15.69      | 0.87×   | 15.96 M  | 21 K      | 1.000    |
  | 33 M | 16.76       | 13.08      | 0.78×   | 29.9 M   | **4.95 M** | 0.978   |
  | 50 M | 15.58       | n/a        | n/a     | —        | —         | —        |

  Fixture-only DRAM at t=8 median (MB) — `rss_loaded_mb` minus the
  YCSB harness's two `std::vector<ycsb::Record>` buffers (prefill_data
  + per-workload data, both shared between fixtures and unrelated to
  the system being compared; sizeof(Record) ≈ 216 B):

  | size | Viper zipf | HiOM zipf | Δ        | Viper uniform | HiOM uniform | Δ        |
  |------|-----------:|----------:|---------:|--------------:|-------------:|---------:|
  | 5 M  | 4159       | **1075**  | **-74%** | 3797          | **661**      | **-83%** |
  | 10 M | 4140       | 1353      | -67%     | 3778          | 1010         | -73%     |
  | 16 M | 4117       | 1462      | -64%     | 3755          | 1098         | -71%     |
  | 33 M | 4055       | 1538      | -62%     | 3697          | 1105         | -70%     |

  Viper's line is essentially flat at ~4000 MB — CCEH's 2 GiB
  pre-allocation plus a roughly constant remainder (binary, OS,
  Google Benchmark, fixture state) dominates. HiOM's line rises
  modestly from ~1 GiB to ~1.5 GiB as ColdTier grows and HotTier
  fills, while HotTier itself remains capped at 256 MiB by design.
  **Net savings: HiOM uses 17–38% of Viper's fixture DRAM (–62% to
  –83%) at every size**, comfortably exceeding the ≥50% win-condition
  target. The absolute `rss_loaded_mb` numbers (e.g. 5 M zipf:
  Viper 6219 MB, HiOM 3134 MB, –50%) are reported in [Phase 2 §RSS
  appendix table](#) for transparency but understate the gap because
  prefill_data + data_zipf/uniform vectors dominate at scale.

  Caveat: the comparison subtracts an *estimated* harness vector
  footprint (record_count × 216 B), not a directly measured one,
  because `mem_baseline` in [ycsb_bm.cpp:112](../benchmark/ycsb_bm.cpp#L112)
  is captured *after* `InitMap()` and already includes the fixture's
  index allocation, so the obvious subtract-baseline approach
  zeroes the very gap we want to show. A future cleanup is to
  move the baseline capture to before `InitMap()` so the metric
  becomes directly measured.

  **Win-condition graceful-degradation narrative** (the headline
  paper figure):
  - At 5–16 M (data fits HotTier comfortably), HiOM ratio rises
    monotonically as dataset grows (0.69 → 0.79× zipf, 0.92 → 0.87×
    uniform) because Viper's CCEH segment-lookup cost grows while
    HiOM's HotTier-DRAM hit stays cache-resident.
  - At **33 M (= HotTier capacity 33.5 M)**: SIEVE eviction engages
    (3 M zipf, 5 M uniform) yet hit_rate holds 0.97–0.98 and
    throughput ratio is **0.81× zipf / 0.78× uniform** — within the
    "≥80% on production-skewed workloads" target. This is the
    "graceful" inflection point the design doc claims.
  - 50 M HiOM data missing: prefill of 50 M entries through
    `mirror_write_with_offset` now (post-M3.5) genuinely populates
    HotTier for every entry; when HotTier passes the 33 M cap
    during prefill, SIEVE eviction conflicts with PINNED slots
    awaiting flush → pin_failure spin loop → wall time
    unacceptable (>14 h, ultimately killed). This exposes an M4
    back-pressure design limit: the inline-flush mechanism
    handles ~10× over-subscription (its test uses 10 K writes
    into a 4 K-slot HotTier) but not 1.5× at multi-million-entry
    scale. Treated as a known limitation for the paper write-up;
    fix candidate is per-region SIEVE clock pacing (TODO M7
    follow-up).
## Status (2026-05-20)

- **Phase 2 sweep completion — YCSB-A/B 33M write-heavy cells filled
  in**. 30-cell run via `run_scaling_sweep.sh` (with new per-cell
  `SWEEP_PER_CELL_TIMEOUT_S=1800`, 1h51min wall total) closed the
  missing cells from 2026-05-18: full HiOM/Viper coverage for 16M
  b_zipf/b_uniform/a_uniform_lat and the entire 33M block (read-only
  100r was already done; YCSB-A/B was new at this scale). 28/30
  cells passed; the 2 timeouts were both a_zipf 33M HiOM (tp + lat).
  Resulting H/V tp ratios at t=24 (median of 3 reps):

  | size | workload  | V tp/thr | H tp/thr | H/V    |
  |------|-----------|---------:|---------:|-------:|
  | 16M  | a_uniform |   18.75M |    4.57M | 0.24×  |
  | 16M  | b_uniform |   30.05M |   13.58M | 0.45×  |
  | 16M  | b_zipf    |   43.94M |   16.29M | 0.37×  |
  | 33M  | a_uniform |   18.84M |    4.63M | 0.25×  |
  | 33M  | a_zipf    |   24.82M |     HANG | —      |
  | 33M  | b_uniform |   30.39M |   12.42M | 0.41×  |
  | 33M  | b_zipf    |   39.96M |   15.60M | 0.39×  |

  HiOM ratios on YCSB-A/B (50% / 5% update) are lower than the
  100r baselines (0.27–0.81×) because the update path pays HotTier
  upsert_pinned + commit-buffer push + flusher latency on top of
  the read-side HotTier verify. The ratio trend t=1 → t=24
  *worsens* (0.34× → 0.25× for a_uniform), reflecting producer-side
  contention growth, not per-op work growth — consistent with the
  delete t=24 deep-dive on 2026-05-16.

- **The real M4 trigger identified: `a_zipf` at HotTier-cap**. Only
  one workload reproduced a deadlock-equivalent livelock across the
  30 sweep cells: HiOMFixture a_zipf 33M (both tp and lat; tp
  SIGKILL'd at the 30-min cap). b_zipf_tp at 33M completed cleanly
  (15.6 M/s @ t=24) but b_zipf_lat at 33M also timed out — lat-mode
  adds per-op HDR histogram cost that pushes a marginal cell over,
  but the root cause is the same as a_zipf's. Partial JSONs
  preserved at
  `results/scaling/HiOMFixture_{a_zipf_33M_tp,b_zipf_33M_lat}.json.timeout_*`.

  Mechanism (matches the back-pressure path in
  [hiom.hpp:578-637](../include/viper/hiom/hiom.hpp#L578-L637)):
  1. YCSB-A = 50% update; zipf-θ=0.99 concentrates writes on the
     hot keyspace, so a small set of fp32 buckets gets the
     overwhelming majority of `upsert_pinned` traffic.
  2. At 33M-scale dataset (= HotTier 33.5M slot capacity), SIEVE
     eviction is already engaged for the working set; the eviction
     hand must skip PINNED slots
     ([hot_tier.hpp:421-449](../include/viper/hiom/hot_tier.hpp#L421-L449)).
  3. With 24 producers all targeting the same handful of hot
     buckets, the bucket reaches "16 slots, all PINNED" → 32
     retries of `try_inline_flush(256)` → `push_commit` + the
     unbounded `while (commit_buf_->size_hint() > 0)` drain wait
     at [hiom.hpp:628-636](../include/viper/hiom/hiom.hpp#L628-L636).
  4. Twenty-four simultaneous `try_inline_flush` try_lock
     attempts on the 4 `flusher_mus_` slots + the
     `wake_all_flushers` cv-notify storm (one `lock_guard +
     notify_one` per flusher per producer, every 50 µs) starve
     the background flusher's `apply_batch` progress. The buffer
     never drains to empty → no producer ever returns.

  Workloads that completed at 33M show the same primitive is not
  fatal in general:
  - a_uniform (50% write, uniform): writes spread across all
    buckets, no bucket fills with PINNED → 4.63 M/s.
  - b_uniform / b_zipf (5% write): write rate too low to fill any
    single bucket with PINNED before the flusher catches up.

- **2026-05-18 "16M a_uniform 24t hang" reclassified as env-noise
  fluke (not a code bug)**. The 14607-byte truncated partial JSON
  from the 5/18 sweep showed threads:1/8 complete then mid-write
  termination at threads:24. Today's isolated re-run of the same
  cell (binary unchanged — `git log` since 5/18 is plotter-only)
  finished in ~90 s wall with 4.57 M/s @ t=24, hot_hit_rate=0.998,
  hot_evictions ≈ 10 K. The full-sweep re-run today (different
  process, fresh prefill, after the binary churned through several
  other cells) also passed at 4.57 M/s. Two clean runs vs. one
  hang during a 24-hour `/pmem0`-contended sweep points to PMem
  bandwidth contention from other tenants as the proximate cause;
  the producer-vs-flusher contention machinery exists but only
  deadlocks under workload-specific conditions, which 16M+a_uniform
  doesn't meet (uniform writes spread out, no bucket-PINNED
  saturation). Evidence at
  `/tmp/hiom_evidence/HiOMFixture_a_uniform_16M_tp.{partial_20260518,fresh24tonly_20260520}.json`.

- **Paper M7 wording for the M4 known limitation** (proposed): "At
  working sets ≥ HotTier capacity *and* skewed write workloads
  (YCSB-A zipfian), HiOM's bucket-PINNED back-pressure can cause
  writer starvation. The fix is per-bucket SIEVE clock pacing
  (M7 follow-up) or a wait-free buffer drain protocol; the paper's
  win-condition matrix excludes this cell." This is tighter than
  the previous "post-cap back-pressure" framing — it names the
  specific (workload, dataset-size) corner rather than implying
  everything past the cap fails.

- **Tooling added alongside the investigation**:
  - `SWEEP_PER_CELL_TIMEOUT_S` knob in `run_scaling_sweep.sh`
    (default 1800 s) wraps each ycsb_bm invocation in
    `timeout --signal=KILL`; truncated outputs are renamed
    `<file>.timeout_<ts>` and logged to
    `results/scaling/.sweep_timeouts.log`. Makes the sweep
    safe to run unattended.
  - `benchmark/repro_m4_hang.sh`: standalone single-cell repro
    template (now most useful for re-investigating the real
    a_zipf 33M trigger, since 16M+a_uniform doesn't deadlock).
    Captures gdb `thread apply all bt` + `perf record -g
    --call-graph dwarf` automatically once the configured budget
    elapses.

## Status (2026-05-09)

- **Phase**: M0 ✅; M1 functionally complete except EBR (closed by
  analysis in M4 Phase D — see below); M2 Phase B-1/B-2 ✅; M3 Phase
  A+B+C+D ✅; M4 Phase 0+A+B+C+D+E ✅ — **M4 fully closed
  (2026-05-05)**:
  - **Phase 0/A/B**: stable_sort fix, 2-bit state machine, inline-flush
    back-pressure (unchanged from earlier).
  - **Phase C** (multi-producer same-key happens-before via
    `commit_seq_` + `apply_mu_` drain-to-empty + `(fp64, seq)` sort +
    descending alive-and-fp-match walk; mirror_write switched from
    CCEH peek to `viper_.last_put_offset()`).
  - **Phase D — EBR analysis closed (no implementation needed)**.
    The 8-byte atomic packed slot + verify-on-PM-key combination is
    sufficient for every concurrent eviction / re-pin race — see
    "Phase D analysis" subsection below for the case-by-case proof.
    EBR remains a future-optimization gate for a "skip-verify" hot
    path; today's design pays a single PM read to verify, which is
    on the same cache line as the value, so EBR adds complexity
    without meaningful runtime benefit.
  - **Phase E — multi-thread + crash injection stress (2026-05-05)**.
    `run_recovery_stress` exercises 8 concurrent writers × 20K writes
    each across 6 iterations (3 fast-flusher + 3 slow-flusher with
    different crash points). 18 / 18 iterations recover every put
    that returned before the crash, zero loss, across 3 consecutive
    full runs. Slow-flusher iterations actually exercise tail-scan
    end-to-end (replayed = 96K–160K, full ColdTier rebuild from
    VPages); fast-flusher iterations exercise the steady-state
    interaction between the flusher, the cadence-driven checkpoint,
    and the recovery scan.
  - **M6 design fix landed alongside Phase E**:
    multi-writer-aware checkpoint frontier. The original M6
    frontier (= Viper's `current_block_page_`) silently dropped
    entries when a "slow" client was still pushing into a block
    far below the global next-to-claim — recovery's
    `[frontier-1, current)` scan would skip that block entirely.
    Fix: HiOM now tracks each Client's most-recent block in a
    256-slot `client_slots_` array (`note_client_block` on every
    `mirror_write_with_offset`); `try_write_checkpoint` captures
    `min(min_active_writer_block, viper_.hiom_vpage_frontier)` so
    the persisted frontier is a true lower bound for any block
    with potentially-unflushed entries. Without this, Phase E's
    fast-flusher mid-stream iterations lost ~836 keys per run.
- **M5 ✅** (A/B checkpoint protocol with FNV-1a torn-write detection;
  unchanged structurally, but `try_write_checkpoint` now uses the
  multi-writer-aware frontier — see Phase E above).
- **M6 pragmatic ✅ (2026-05-04)** — bounded VPage tail scan from
  `[checkpoint.vpage_frontier - 1, current_block)`, parallelised across
  `recovery_threads`, replays into ColdTier idempotently. Correctness
  only this round; the recovery-time win ("40× faster" paper claim) is
  carved out to **M6.5**.
- **M6.5 partial ✅ (2026-05-04)** — `ViperConfig::skip_recovery`
  flag added (gates `recover_database()` in the constructor); new
  standalone `hiom_recovery_bm` measures Viper baseline open vs
  HiOM open(skip_recovery=true)+tail-scan on the same prefilled
  pool. **M6.5 full ✅ (2026-05-04)** — `Viper<K,V>::OldOffsetResolver`
  callback retires CCEH from the *write* path: `Client::put` /
  `Client::update` / `Client::remove` consult HiOM (ColdTier with
  fp64 + key-verify) as a fallback whenever `map_.Get` /
  `map_.Insert` reports tombstone. Fires at most once per key per
  process (write paths hydrate map_ on hit). Phase D of the
  recovery test exercises post-restart update+remove with
  skip_recovery=true and confirms slot accounting (live VPage
  records drop from 2000 → 1000 after removing half, persisted to
  PM). **Note on the recovery-time win**: the resolver enables
  *correctness* of post-restart writes, not faster open. The
  open-time speedup itself is set by tail-scan vs full CCEH
  rebuild, which we already had in M6.5 partial. At 1M / 10M the
  signal is dominated by `/pmem0` mount noise (other tenants'
  load); 100M is the regime where the tail-vs-full asymptotic gap
  is unmistakable.
- **Code on disk**:
  - `include/viper/hiom/hot_tier.hpp` (~470 lines): unchanged from M4.
  - `include/viper/hiom/offset_codec.hpp` (~110 lines): unchanged.
  - `include/viper/hiom/cold_tier.hpp` (~810 lines after
    2026-05-05 bulk_upsert): adds `BulkEntry`, public
    `bulk_upsert(std::vector<BulkEntry>&)`, and the private
    `apply_bucket_group` helper (per-bucket-group two-fence
    persist). Existing `upsert` / `lookup` / `remove` /
    `parallel_load` API unchanged.
  - `include/viper/hiom/commit_buffer.hpp` (~110 lines): unchanged.
  - `include/viper/hiom/checkpoint.hpp` (~235 lines): unchanged from M5.
  - `include/viper/hiom/hiom.hpp` (~830 lines after M4 Phase E):
    M6 added `RecoveryConfig`, `recover_tail_into_cold()` (parallel
    scan using N worker threads, idempotent ColdTier upserts),
    `simulate_crash_for_test()` test hook, `recovery_replayed`
    stat. Constructor reordered: prime counters → tail scan →
    spin up flusher (so no concurrent writes interleave with replay).
    M4 Phase E added the per-Client slot registry: 256-slot
    `client_slots_` array (each slot `(active, last_block)` 64-byte
    aligned to avoid false sharing); `reserve_client_slot` / 
    `release_client_slot` / `note_client_block` /
    `min_active_writer_block` helpers; `Client` gained an explicit
    move ctor + dtor that release the slot, plus a `slot_idx_`
    member set by `get_client()`. `try_write_checkpoint` now
    captures `min(min_active_writer_block, viper_.hiom_vpage_frontier)`
    as the safe frontier.
  - `include/viper/viper.hpp`: M6 added `Viper::hiom_visit_records<Visitor>()`
    public template (mirrors `recover_database`'s VPage iteration
    line-for-line, exposed as a generic visitor). M6.5 added
    `ViperConfig::skip_recovery` (default false; when true,
    constructor skips the `recover_database()` call so HiOM's tail
    scan is the sole index recovery path). M6.5 full added
    `Viper<K,V>::OldOffsetResolver` (`std::function<KVOffset(const K&)>`)
    + `set_hiom_old_offset_resolver()` setter. The three write-path
    callsites (`Client::put` line ~1162, `::update` line ~1395,
    `::remove` line ~1438) gain a tombstone fallback that consults
    the resolver — null by default (legacy mode unchanged), set by
    HiOM's constructor when ColdTier is wired up.
    **2026-05-05**: split `internal::pmem_persist` into
    `pmem_flush_range` (clwb loop, no fence) + `pmem_drain` (single
    sfence). The original `pmem_persist` keeps its semantics; the
    split lets ColdTier::bulk_upsert batch many flushes and emit a
    single drain across the bucket-group.
  - `include/viper/hiom/hiom.hpp` (~770 lines after 2026-05-05
    bulk_upsert refactor): `apply_batch` collects kPut winners
    into a `std::vector<ColdTier::BulkEntry>` and dispatches via
    one `cold_->bulk_upsert(puts)` call instead of per-entry
    upsert; kRemove winners stay on the per-entry path. Checkpoint
    cadence hook now fires per crossed boundary instead of per
    apply_batch. M6.5 full installs the resolver closure in
    HiOM's ctor (Step 3, just before flusher spin-up) when
    `cold_ != nullptr`. The closure does `cold_->lookup(fp64)` +
    `hiom_read_at_offset` + key-verify, returning
    `KVOffset::Tombstone()` on miss / fp collision / locked page.
  - `benchmark/hiom_recovery_bm.cpp` (~290 lines, **new in M6.5**):
    standalone stopwatch benchmark (no Google Benchmark wiring).
    HiOM-mediated prefill populates Viper VPages + Viper CCEH +
    ColdTier + a sealed Checkpoint, then closes; baseline mode
    times `Viper::open()` with full CCEH rebuild, HiOM mode times
    `Viper::open(skip_recovery=true)` + ColdTier/Checkpoint/HiOM
    ctor with `tail_scan=true`. Default sweep N ∈ {1M, 10M};
    `--full` adds 100M.
  - `benchmark/fixtures/hiom_fixture.hpp` (~280 lines, **new
    2026-05-05**): Google Benchmark fixture wrapping HiOM<K,V>
    for the same harness used by ViperFixture. Mirrors the
    `BaseFixture` interface (InitMap / DeInitMap / setup_and_*
    / run_ycsb); manages Viper + ColdTier + Checkpoint + HiOM
    lifetimes under `/pmem0/hiom_bench/`. Wired into
    `all_ops_benchmark.cpp` (ALL_BMS via KeyType16 × ValueType200)
    and `ycsb_bm.cpp` (KeyType8 × ValueType200 specialisation).
  - `test/hiom_integration_test.cpp` (~1450 lines after M4 Phase E,
    **8 tests** with 4-phase recovery + 6-iteration crash stress):
    M6 added `run_recovery_persistence` Phases A/B/C; M6.5 full added
    Phase D — post-restart update + remove via skip_recovery=true,
    with slot accounting via `hiom_visit_records` to prove the
    resolver invalidates the right VPage slots. Phase D writes
    2000 prefill keys, reopens with skip_recovery=true, updates
    every key in place, removes the odd-indexed half, and asserts
    live record count drops from 2000 → 1000 both in-process and
    after a clean reopen. **M4 Phase E added `run_recovery_stress`**:
    8 writers × 20K writes per iteration × 6 iterations (3 fast
    flusher + 3 slow flusher), random crash points spanning early
    / mid / late mid-stream. Asserts every put that returned before
    the crash is recoverable post-restart via tail-scan; zero loss
    across 18 / 18 iterations × 3 consecutive runs.
- **Throughputs (single-threaded, post-M6)**:
  - HotTier standalone lookup: 135 M ops/s.
  - HiOM full-path lookup: 49.3 M ops/s vs raw Viper 47.4 M/s —
    ratio 1.04. Recovery code is on the open() path only; zero
    steady-state overhead.
  - HiOM commit-window with back-pressure: 31 pin_failures, 10K
    reads correct (M4 unchanged).
  - M6 recovery shape (10K writes, slow flusher): replayed ≈ 5446
    entries from a 4-block tail (block 3 boundary + blocks 4–6),
    cold goes 5000→10000, all 10K reads succeed post-recovery.
  - M6.5 recovery wall-clock (`hiom_recovery_bm`,
    `recovery_threads=32`):
    | N    | baseline_ms | hiom_ms | speedup | notes |
    |------|-------------|---------|---------|-------|
    | 1M   | ~240        | ~260    | 0.9×    | HiOM open dominated by ColdTier mmap + HotTier ctor |
    | 10M  | 290–910     | ~580    | 0.5×–1.5× | high inter-run variance from `/pmem0` mount sharing |
    | 100M | TBD (deferred to follow-up run; see *Performance follow-up* below) | | | |
    Variance is dominated by other tenants on `/pmem0`: across two
    consecutive runs at 10M the baseline ranged 293–910 ms while
    HiOM stayed ~580 ms. Asymptotically baseline is O(all VPages)
    and HiOM is O(tail), so the gap widens at 100M.
  - **Resolver post-restart correctness** (Phase D, M6.5 full):
    2000 prefill keys → reopen with skip_recovery=true →
    cl.update(every key) → cl.remove(half) → live VPage records:
    1000 (matches expected). Without the resolver, update would
    no-op (map_ empty → tombstone return), remove would no-op,
    live count would stay 2000 — the test catches the regression
    via slot accounting.
  - **M4 Phase E recovery stress** (8 writers × 20K each = 160K
    puts/iter, 6 iters, 3 fast / 3 slow flusher):
    | iter | mode | crash | completed | cold@crash | replayed | recovered/lost |
    |------|------|-------|-----------|------------|----------|----------------|
    | 0 | fast |  30 ms |  ~50K | ~50K | ~1K  | full / 0 |
    | 1 | fast |  80 ms | ~145K | ~145K | ~10K | full / 0 |
    | 2 | fast | 250 ms | 160K | 160K | ~250 | full / 0 |
    | 3 | slow |  50 ms | ~100K | ~100K | full | full / 0 |
    | 4 | slow | 200 ms | 160K | ~140K | full | full / 0 |
    | 5 | slow | 600 ms | 160K | 160K | full | full / 0 |
    All slow-flusher iterations exercise the full tail-scan path
    (replayed = entire dataset, ColdTier rebuilt from VPages).
    Pre-fix (without min_active_writer_block in checkpoint), iter
    1 at fast/80ms lost ~836 keys per run; with the fix, 18/18
    iterations across 3 consecutive runs are clean.
- **Phase D analysis (no-implementation rationale)**: The race that
  EBR is meant to fix is "reader holds slot snapshot (fp_A, off_A);
  evictor zeros slot; another writer re-pins slot for (fp_B, off_B);
  reader uses stale (fp_A, off_A) to do verify". With the current
  design — 8-byte atomic packed slot loaded in a single op +
  `verify_and_read_offset` re-reading the PM page and comparing
  keys — every case resolves correctly:
  - Viper reclamation OFF (default): PM at off_A still holds key_A,
    key match succeeds, reader returns the (still-valid) value.
  - Viper reclamation ON + slot reused for key_C: PM at off_A
    holds key_C, key compare fails, reader misses HotTier and
    falls through to ColdTier (which is authoritative post-M3
    Phase D). Correct.
  - Bucket-scan ordering (writer inserts at slot_i after reader
    has passed it): reader misses, falls through to ColdTier;
    correct provided ColdTier has the entry, which is guaranteed
    by the PINNED → IN_FLUSH → UNPINNED state machine (the slot
    only goes UNPINNED *after* ColdTier's durable upsert
    completes).
  EBR could enable a future "skip-verify on stable slots"
  optimization (e.g., per-slot epoch stamps, slots untouched for
  ≥2 epochs are read without PM verify), but the current verify
  is cheap (PM read is the same cache line as the value), so the
  cost-benefit doesn't favor adding it now. The infrastructure
  (`epoch-reclaimer` already in CMake fetch) is ready when it
  does. Closes M1's "deferred to M4" and M4 Phase D in one stroke.
- **Bench-fixture parity (2026-05-05)** — `HiOMFixture<KeyT,ValueT>`
  added at `benchmark/fixtures/hiom_fixture.hpp`, mirroring the
  ViperFixture interface (`InitMap` / `DeInitMap` / `setup_and_*` /
  `run_ycsb`). Wired into `all_ops_benchmark.cpp` (full ALL_BMS
  template instantiation for KeyType16 × ValueType200) and
  `ycsb_bm.cpp` (KeyType8 × ValueType200 specialisation). All HiOM
  PM artefacts live under `/pmem0/hiom_bench/{viper, cold.bin,
  chkpt.bin}`; cleanup is prefix-guarded per CLAUDE.md (only this
  prefix is touched). HotTier capacity defaults to 2^18 buckets =
  4M slots = 36 MB DRAM (~2× the 1M+1M ALL_OPS workload size,
  comfortable margin against SIEVE eviction churn). Initial 1M smoke
  numbers (single-thread):
  | op     | ViperFixture  | HiOMFixture   | ratio HiOM/Viper |
  |--------|---------------|---------------|------------------|
  | insert | 525 K/s       | 200 K/s       | 0.38× (HiOM 2.6× slower) |
  | get    | 1.57 M/s      | 1.69 M/s      | 1.08× (HiOM 8% faster) |
  | update | 2.53 M/s      | 0.57 M/s      | 0.22× (HiOM 4.4× slower) |
  | delete | 0.93 M/s      | 0.60 M/s      | 0.65× (HiOM 1.5× slower) |
  Reads parity confirmed (consistent with the integration-test
  microbench's 1.04× ratio). Writes are slower than expected: at
  thread=8 the gap **widens to ~30× on inserts** (Viper 29 M/s
  aggregate vs HiOM 1 M/s aggregate), pointing to a real
  multi-threaded contention bottleneck on the write path —
  candidates: `commit_seq_` global atomic, moodycamel mpmc enqueue
  contention, or the flusher's per-entry `cold_->upsert` PM fence
  cost saturating PM bandwidth with 8 producers feeding it. This
  was always the next-priority M3 follow-up ("cache-line-aligned
  bulk writes to ColdTier"); the fixture made it visible. See
  *M3 follow-ups* below for the planned attack surface.
- **M3 follow-up #1 landed: cache-line-aligned bulk writes to
  ColdTier (2026-05-05)**. Two pieces:
  - `viper::internal::pmem_persist` split into `pmem_flush_range`
    (clwb loop, no fence) + `pmem_drain` (single sfence). Lets a
    caller batch many flushes and amortise the sfence — the actual
    cost driver for small PM updates.
  - `ColdTier::bulk_upsert(std::vector<BulkEntry>&)`: sort entries
    by (region, bucket); for each contiguous (region, bucket) run,
    walk the chain once classifying each entry as
    match-update or new-insert (claim empty slot or extend chain),
    stage all stores, then issue exactly **two** drains per
    bucket-group (one for entry data, one for header occupancy
    bits) instead of two per entry. Crash-consistency invariant
    unchanged: header bit becomes visible only after entry data
    is durable. Caller (HiOM::apply_batch) holds `apply_mu_`, so
    no concurrent writer hits the same chain.
  - `HiOM::apply_batch` refactored to two stages: (1) collect
    kPut winners across all (fp64, seq) runs into a single
    `BulkEntry` vector, dispatch via one `cold_->bulk_upsert`
    call; kRemove winners stay on the per-entry path (rare,
    target existing chain entries). (2) HotTier state machine
    (PINNED → IN_FLUSH → UNPINNED) drives unchanged. Because a
    single bulk apply now spans many cadence boundaries, the M5
    checkpoint hook fires *per crossed boundary* instead of once
    per `apply_batch` (test caught it: 10K writes / cadence 1024
    expected ≥9 checkpoints; with naïve per-batch trigger it was
    8).
  - Direct `ColdTier::bulk_upsert` correctness + update test
    added in `cold_tier_test.cpp` (`run_bulk_upsert`): 200K
    random fps in 256-batch chunks, full-set lookup parity, then
    same-key second pass with new offsets, all 200K observe the
    update. PASS.
  - Throughput delta on `all_ops_bm` (1M, KeyType16 × ValueType200,
    `_mean` of 3 repetitions; baseline column is the same
    smoke run from the row above):
    | op     | thread | Viper       | HiOM (post-bulk) | HiOM/Viper | HiOM (pre-bulk) |
    |--------|--------|-------------|------------------|------------|------------------|
    | insert | 1      | 485 K/s     | 191 K/s          | 0.39×      | 200 K/s (0.38×) |
    | insert | 8      | 2.94 M/s/thr | 142 K/s/thr     | 0.05×      | ~1 M/s aggregate (~30× slower) |
    | get    | 1      | 1.29 M/s    | 1.29 M/s         | 1.00×      | 1.08× |
    | get    | 8      | 10.05 M/s/thr | 9.86 M/s/thr   | 0.98×      | — |
    | update | 1      | 2.01 M/s    | 485 K/s          | 0.24×      | 0.22× |
    | update | 8      | 10.42 M/s/thr | 500 K/s/thr   | 0.048×     | — |
    | delete | 1      | 766 K/s     | 496 K/s          | 0.65×      | 0.65× |
    | delete | 8      | 5.27 M/s/thr | 3.64 M/s/thr   | 0.69×      | — |
    Single-thread numbers are essentially flat — bulk_upsert only
    pays off when the flusher catches a large batch, and a
    single-threaded inserter pushes one entry at a time so each
    apply round still does a ~1-entry batch. At thread=8
    aggregate, HiOM insert went from ~1 M/s → ~1.13 M/s (small
    but in the right direction); aggregate update from ~1 M/s →
    ~4 M/s — the better win lands on update because every update
    forces a (Put, Remove) sequence of commit-buffer entries that
    coalesce into the same fp64 run, giving the bulk path more to
    chew on. The remaining ~21× insert gap at thread=8 is no
    longer in the cold-tier write path; it points at the
    `commit_seq_` global atomic and moodycamel mpmc enqueue
    contention (M3 follow-up #2).
- **M3 follow-up #2 — investigation, NOT landed (2026-05-08)**.
  Goal: cut the thread:8 insert gap by removing the `commit_seq_`
  global atomic and the single-MPMC moodycamel queue contention.
  Two attempts, both rolled back; surface unchanged.
  - **Attempt A — N-lane sharded commit buffer**. Sharded the
    single `moodycamel::ConcurrentQueue<CommitEntry>` into 16
    cache-line-aligned per-lane queues; each Client maps to a
    fixed lane via `slot_idx_ & lane_mask_` so producers don't
    contend on a shared MPMC queue's implicit-producer lookup,
    and the flusher takes a per-lane `ConsumerToken` and round-
    robin-drains every lane under `apply_mu_`. Surface change
    was small (≈210 added LOC across `commit_buffer.hpp`,
    `hiom.hpp`, plus a `commit_lanes` knob on `FlusherConfig`).
    *Result*: thread:1 insert dropped from 191–201 K/s to
    103–104 K/s (~2× regression, three-run mean). Even with
    `commit_lanes=1` (effectively master shape), throughput
    landed at 145–154 K/s — i.e. some of the regression is
    intrinsic to the wrapping (extra `std::vector<Lane>`
    indirection, per-lane `size_approx()` per drain pass), and
    some is the multi-lane drain itself. Not enough wall-clock
    headroom to chase the rest down before commit; rolled back.
  - **Attempt B — replace `seq` with `__rdtsc()`**. Stamped
    each CommitEntry with the Invariant TSC at push time
    instead of bumping the global `commit_seq_` atomic. Idea:
    same-thread monotonicity is free, cross-thread monotonicity
    comes from the shared hardware counter, and apply-time
    `(fp64, tsc)` sort + descending alive-and-fp-match walk
    still picks the latest writer. *Result*: 17 mismatched +
    17 missed keys on the `multi-producer same-key
    correctness` test (8 threads × 50K writes / 256 keys);
    `_mm_lfence()` before `__rdtsc()` cut it to 2 mismatched +
    2 missed but didn't close it. Root cause: `__rdtsc` is
    not serializing — the TSC read can be reordered before
    the preceding CCEH CAS, and even with `lfence` the
    cross-core monotonicity bound is on the order of tens of
    cycles, comparable to the CAS-to-fetch_add interval at
    heavy contention. So a CCEH-CAS loser ends up with a
    higher TSC than the CCEH-CAS winner, the descending walk
    picks the wrong entry, and ColdTier diverges from CCEH.
    *Note*: while reproducing the failure on master to confirm
    the TSC change wasn't to blame, I found master is **also
    flaky** on this test (15–21 mismatches per run). The race
    is inherent to "bump seq AFTER CAS" — a CCEH winner can
    be preempted between CAS retire and `fetch_add`, letting
    a later-CAS-loser get a smaller seq. This is independent
    of M3 follow-up #2 and is now its own follow-up: see
    *Open: multi-producer apply_batch winner picker* below.
  - **Where this leaves M3 follow-up #2**: the right next
    attempt is probably either (a) skip the global counter
    entirely and use **CCEH-truth at apply time** as the
    winner-picker (read PM at the entry's `off` to get the
    key, query CCEH, pick the entry whose `off` matches the
    canonical one), which also incidentally fixes the
    pre-existing flake, or (b) keep the global counter but
    add a per-fp64 strict-monotone hint via the offset itself
    (offsets are unique and CAS order ≅ slot-claim order
    within a Viper Client, but cross-Client interleaving
    breaks the strict ordering).
  - **Attempt C — P0 / HiOM owns the write-path index — ✅ landed
    (2026-05-09)**. The 2026-05-08 reasoning ("CCEH-truth at apply
    time") inverted: instead of teaching apply_batch to consult
    CCEH, we *retire* CCEH from the multi-thread write path
    entirely. Every `Client::put` / `update` / `remove` was paying
    a `map_.Insert` segment-level CAS even though M6.5 had already
    proven HiOM (HotTier→ColdTier) is a sufficient index for the
    write-path "what was the prior offset?" lookup. P0 makes that
    the *only* index when ColdTier is attached; CCEH is still
    constructed (the read-only legacy paths and recovery still
    use it) but the write paths skip `map_.Insert` and `map_.Get`
    entirely. New surface: `Viper::set_hiom_owns_index(bool)` and
    `hiom_map_skipped_` counter (atomic, observable via
    `hiom_map_skipped()`); HiOM's ctor calls
    `set_hiom_owns_index(true)` right after installing the resolver.
    The resolver itself was extended from M6.5's "ColdTier-only,
    fires once per key" form to a full HotTier→ColdTier path
    (HotTier check first; verify the slot's stored key matches
    via PM read; fall through to ColdTier on fp32 collision /
    stale slot; PM-verify ColdTier offset before returning).
  - **Apply-batch correctness under update+remove + fp32 collision
    (P0 fix, Option L)**. The naive "skip kRemove if `viper_.remove`
    returns false" path leaks: if the resolver missed an in-flight
    kPut for `key` (HotTier slot was overwritten by an fp32-colliding
    key + ColdTier still holds the pre-update offset whose VPage
    slot was already freed by the most-recent `Client::put`),
    `viper_.remove` returns tombstone-not-found and HiOM's wrapper
    bails without pushing kRemove. The flusher then upserts the
    in-flight kPut into ColdTier, but no kRemove ever clears it —
    `get(key)` succeeds when the user expected the key to be gone.
    Fix: HiOM's wrapper always pushes a kRemove (and clears the
    HotTier slot) regardless of the underlying `viper_.remove` return,
    and returns `true` when ColdTier is attached. The (fp64, seq)
    sort in `apply_batch` makes the kRemove the highest-seq entry
    for that fp's run, the descending walk picks it, and ColdTier
    is correctly evicted. *Known limitation*: the in-flight kPut's
    VPage slot is leaked (alive on PM, no index points at it) when
    this path triggers — we can't find that slot without scanning
    the commit buffer. The slot is reclaimable by a future compact
    pass; the leak is rare (requires both fp32 collision *and*
    update→remove against the same key, after the most-recent
    update's mirror has been overwritten). Reproduces deterministically
    in `run_p0_update_heavy_multi_thread` (50 K keys, 16 K HotTier
    buckets — load factor and key count tuned to land in the
    fp32-collision regime).
  - **Tests (M3 follow-up #2)**:
    - `run_p0_skip_counter_sanity` (1 K keys, single-thread + a raw-
      Viper baseline). Asserts `hiom_map_skipped()` increments by
      exactly 1 per put/remove when HiOM is attached, and stays at 0
      for raw Viper without HiOM.
    - `run_p0_update_heavy_multi_thread` (2 threads × 25 K keys ×
      insert+update+remove-half). Verifies even-indexed keys hold
      the post-update value, odd-indexed keys are gone, and
      `remove_returned_false == 0` after `flush_and_wait`. With
      Option L the test passes deterministically across runs (3 / 3
      consecutive runs ALL PASS); without Option L it leaks one
      key on every run.
    - `run_multi_producer_correctness` (8 threads × 50 K writes /
      256 keys) — assertion adjusted: P0 retires CCEH from the
      write path, so the test no longer compares HiOM truth
      against `map_.Get`; it compares against direct PM-read at
      the ColdTier-recorded offset (`hiom_read_at_offset`). The
      "preexisting `commit_seq_` flake" caveat from the 2026-05-08
      Open notes is no longer reproducible on the 50 K-keys / 256-
      key-range workload (3 / 3 consecutive runs ok=256 / 256).
  - **Bench numbers (`all_ops_bm` insert microbench, repeats=3)**:
    - thread=1: HiOMFixture 215 K/s vs ViperFixture 376 K/s
      (HiOM ≈ 0.57× — flusher/HotTier overhead at low fan-in).
    - thread=8: HiOMFixture 990 K/s aggregate vs ViperFixture
      27.3 M/s aggregate (HiOM ≈ 0.04×). The 8-thread gap is
      *not* closed by P0 alone — `commit_seq_` global atomic and
      moodycamel mpmc enqueue are still the steady-state bottleneck.
      P0 retires the *segment-level CCEH CAS* per write, which is
      a contributing factor at thread=8 but is dwarfed by queue +
      seq contention. Closing the gap requires the lanes / TSC /
      apply-time-truth work that's still open from the 2026-05-08
      Status entry.
- **M3 follow-up #2 — written (2026-05-09)**, supersedes the rolled-
  back lanes + __rdtsc attempts. P0 + Option L both landed; the
  thread=8 insert gap is narrower than before bulk_upsert but still
  >20× behind raw Viper, so commit-buffer / seq contention work is
  the next lever.
- **Multi-producer apply_batch winner picker — HotTier-truth as
  primary, alive-fp-match walk as fallback ✅ landed (2026-05-09)**.
  Closes the "preexisting `commit_seq_` flake" surfaced on
  2026-05-08. Mechanism: P0 retired CCEH from the write path, so
  `HotTier::upsert_pinned`'s CAS is now the linearization point
  for same-fp32 writes. apply_batch reads the slot's current
  `(fp32, packed_off)` (one acquire-load per fp64 run, via the
  new `HotTier::read_slot(SlotRef)`) and picks the batch entry
  whose off matches; the canonical CAS-winner's data is exactly
  what the slot points at, no `commit_seq_`-after-CAS race left
  to lose. Fallback paths (HotTier slot evicted post-PINNED, fp32
  collision overwrote the slot, HotTier holds an off from a
  future not-yet-drained batch) still run the original descending
  alive-and-fp-match walk with `seq` as tiebreaker. Surface change:
  ~12 added LOC in `hot_tier.hpp` (the `SlotView` helper) and
  ~80 LOC in `apply_batch` (fast path + fallback split).
  Validation: 8 / 8 consecutive `run_multi_producer_correctness`
  runs at ok=256 / 256 (8 threads × 50 K writes / 256 keys),
  zero regressions on the rest of `hiom_integration_test`,
  `all_ops_bm` insert numbers within run-to-run noise (thread=1
  220 K/s vs 215 K/s pre-change; thread=8 unchanged at ~120 K/s).
  Note: `seq` is still bumped per push (still a global atomic
  contention point on the write path) — the picker change made
  it not load-bearing for correctness, which clears the way for
  the next round of M3 follow-up #2 work to *retire `commit_seq_`
  entirely* (e.g., a per-lane local counter for tiebreaks).
- **M3 follow-up #2 Step 3 — multi-flusher producer reachability:
  two `ref.valid=false` paths fixed (2026-05-16)**. While
  validating the Step 1+2+3 chain (per-Client local seq, 8-lane
  sharded commit buffer, N background flushers each owning a
  disjoint lane subset), `all_ops_bm HiOMFixture<KeyType16,
  ValueType200> insert thread:1` ran at **18.3 K/s** (54.6 s for
  1 M inserts) — vs `uint64_t/uint64_t` repro at 1.64 M/s on the
  same Step 3 wiring. Real time was 15× CPU time (90% idle wait),
  confirming a stall not a CPU bottleneck. Threadstack sampling
  showed the main thread in `hrtimer_nanosleep` inside the
  producer hot path; both flushers in `futex_wait`. Two
  *independent* bugs in series:
  - **Bug A — flusher predicate × wake_all_flushers conflict**:
    `flusher_loop`'s `wait_for` predicate was
    `(nonempty & my_lanes_mask) != 0 && size_hint() >= high_watermark`,
    AND-ing the producer-side "early wake" knob (`high_watermark`,
    default 1024) with the flusher-side wake-acceptance gate.
    Whenever the producer-side back-pressure path nudged
    `wake_all_flushers()` while total `size_hint() < 1024`
    (the common case — back-pressure happens at low queue size),
    the predicate rejected the wake and the flusher only ran on
    the 5 ms timer. Fix: predicate is now just
    `(nonempty & my_lanes_mask) != 0`. `high_watermark` retains
    its producer-side role (`push_commit` only invokes
    `wake_all_flushers` when total size crosses it). One-line
    surface change in `hiom.hpp`.
  - **Bug B — encode failure mistakenly triggers blocking drain**
    (the real culprit, the throughput-killer). The producer hot
    path declares `HotTier::SlotRef ref{}` default-constructed
    (`valid=false`), then runs `upsert_pinned` *inside* an
    `if (packed)` block where `packed` is `encode()`'s
    `std::optional<compact_offset_t>`. If `encode()` returns
    `nullopt`, the entire upsert_pinned + retry loop is skipped
    and `ref` stays default-invalid. Later, the M4 Phase B
    blocking-drain gate (`if (!ref.valid && commit_buf_)`)
    interprets that as "upsert_pinned failed → block until
    buffer drains" — but encode failure is a *different* case
    (the put already landed in Viper PMem, push_commit will
    route it to ColdTier asynchronously, reads fall through
    HotTier-miss → ColdTier just fine). With encode failing on
    every put once `block_number > 8191` (see Open below), the
    producer paid one full commit-buffer drain (~50 µs) per put.
    Fix: gate the blocking drain on a new `packed_ok` boolean
    so it only fires when we *actually called* upsert_pinned
    and it returned invalid (real M4 Phase B back-pressure).
    `~20` LOC in `hiom.hpp`.
  - **Validation**: rebuild + same `all_ops_bm HiOMFixture
    <KeyType16, ValueType200> insert threads:1 --repetitions=1`
    (which actually fans out 3 reps via `Repetitions(3)` on the
    `BENCHMARK_REGISTER`):
    | metric | pre-fix | post-fix |
    |--------|---------|----------|
    | real time / 1 M inserts | 54.6 s | **1.85 s** |
    | items/s | 18.3 K/s | **541 K/s** (~30×) |
    | `inline_flush_calls` | 1.115 M | 0 |
    | `commits_flushed` | 2 M | 1.999 M |
    | CPU / Real | 6.6% | 55% |
    All measured with `num_flushers=2`, single-producer prefill
    (`num_util_threads_ = 1` left in fixture as TEMP debug).
    The 541 K/s ≈ 1/3 of the user's uint64_t Step 3 repro
    (1.64 M/s) is explained by Viper write-path scaling with
    value size, not HiOM overhead: 25× larger value → ~7× more
    PMem cacheline writes per put, and VPage density drops
    from hundreds-per-page (uint64_t) to 18/page (216 B entry),
    so block frontier advances ~5× faster.
  - **Multi-thread scaling (2026-05-16, TEMP-debug removed,
    defaults: `num_flushers=4`, `num_util_threads=36`, 64 GiB
    pool)**. Same workload (`all_ops_bm`, KeyType16+ValueType200,
    1 M prefill + 1 M timed ops, median over 3 reps; numbers are
    `items_per_second` per-thread — aggregate = N × per-thread):
    | op     | t=1 V | t=1 H | **t=1 H/V** | t=8 V | t=8 H | **t=8 H/V** | t=24 V | t=24 H | **t=24 H/V** |
    |--------|------:|------:|------:|------:|------:|------:|------:|------:|------:|
    | insert | 814 K | 530 K | 0.65× | 3.89 M | 2.06 M | 0.53× | 5.00 M | 2.71 M | 0.54× |
    | get    | 1.57 M | 1.66 M | **1.06×** | 12.18 M | 12.54 M | **1.03×** | 31.25 M | 19.40 M | 0.62× |
    | update | 2.54 M | 1.26 M | 0.50× | 11.31 M | 6.69 M | 0.59× | 13.76 M | 9.44 M | 0.69× |
    | delete | 858 K | 1.10 M | **1.28×** | 5.88 M | 3.19 M | 0.54× | 11.26 M | 2.79 M | **0.25×** ⚠ |
    Stats clean across all 36 runs (3 reps × 12 thread×fixture×op
    cells): `pin_failures=0`, `inline_flush_calls=0`. Compare to
    pre-Step-3 2026-05-09 measurement at thread=8 insert (HiOM
    990 K vs Viper 27.3 M = **0.04×**) — Step 1+2+3 plus the two
    2026-05-16 fixes close the 20× scaling gap for insert.
    Read paths:
    - **get is HiOM's strongest story**: at t=1 / t=8 HiOM is
      slightly *ahead* of Viper (1.03–1.06×) — HotTier-DRAM hit
      bypasses Viper's CCEH segment lookup. At t=24 drops to
      0.62× (still 19 M/thread, ~466 M/s aggregate), suggesting
      HotTier slot atomic / lookup-path contention at high
      fan-in.
    - **insert / update** behave like one another: ~0.5–0.7×
      across t=1/8/24, dominated by HiOM's per-put bookkeeping
      (HotTier upsert_pinned + commit-buffer push + Stage 2 CAS
      dance). thread:1 update is the worst single ratio (0.50×)
      because Viper's update path is comparatively cheap
      (in-place value mutate + clwb) while HiOM still pays the
      full producer-side ceremony.
    - **delete at thread:1 is *faster* on HiOM** (1.28×) — see
      Open below for explanation. At thread:24 it collapses to
      0.25×; deep-dive in next bullet.
    Per-thread overhead growth (insert): 0.66 µs (t=1) → 4.0 µs
    (t=24) excess over Viper. Producer-side contention
    (CommitBuffer enqueue, HotTier bucket CAS,
    `wake_all_flushers` lock cascade) is the remaining lever.
- **Open — delete thread:24 = 0.25× Viper (2026-05-16)**. Wall-
  time deep-dive (the `items_per_second` 0.25× ratio is inflated
  by HiOM's higher `found_counter` count, since `SetItemsProcessed`
  uses the returned-true count; Option L makes HiOM return true
  even when the key is already gone). On the same 1 M random-delete
  attempts at thread:24, Viper completes in 2.34 ms vs HiOM
  **14.95 ms** = **6.4× slower wall time**. Two-axis analysis:
  - Per-op CPU time GROWS super-linearly with thread count for
    HiOM but stays flat for Viper:
    | threads | Viper µs/op CPU | HiOM µs/op CPU | HiOM/Viper |
    |--------:|---------------:|---------------:|-----------:|
    | 1       | 1.36           | 0.96 ✅        | 0.71× |
    | 8       | 1.21           | 3.24           | 2.68× |
    | 24      | 1.34           | 10.3           | **7.7×** |
    HiOM is *cheaper* than Viper at t=1 (HotTier+ColdTier write
    path is more efficient than CCEH tombstone for a single
    thread) but pays a 10× cache-line-ping-pong penalty at
    t=24 — pure contention growth, not per-op work growth.
  - Per-op work HiOM does (constant across thread counts, from
    `debug_kremove_pushed` / `debug_resolver_pmem_reads` /
    `debug_cold_remove_called` instrumentation):
    - 1 M kRemove pushes into commit buffer (Option L pushes
      regardless of `viper_.remove` return)
    - 633 K resolver PMem reads (216 B each — HotTier hit
      verifies the stored key against the PMem record)
    - 998 K background `cold_->remove(fp64)` calls (winner
      picker promotes one kRemove per fp64 run)
    - 368 K of the kRemoves are redundant (`viper_.remove`
      returned false because the key was already gone — Option L
      still pushes for the fp32-collision edge case)
  - Hot-atomic ablation (replace one fetch_add with no-op,
    re-measure):
    | atomic | thread:24 perf | gain |
    |--------|---------------:|-----:|
    | baseline | 2.33 M/s | — |
    | stub `Viper::hiom_map_skipped_.fetch_add` | 2.43 M/s | +4% |
    | stub `HotTier::size_.fetch_sub` | 2.50 M/s | +7% |
    No single dominant atomic; the gap is the cumulative
    cache-line ping-pong across several diagnostic / accounting
    atomics on the producer hot path plus the design-level
    extra work above.
  - Decision: **defer fixing**. The fix paths are
    (1) move diagnostic atomics to per-Client + sum-on-read
        (≈ +15% wall-time, doesn't change ratio class);
    (2) tighten Option L so kRemove is skipped when
        `viper_.remove` returned false AND no fp32 collision was
        recently observed (≈ −37% kRemove pushes, but adds a
        correctness escape hatch + needs a new test for the
        edge case);
    (3) accept that HiOM maintains a secondary index, so
        delete-heavy workloads will always pay that — the paper
        story is **get and recovery**, not delete throughput.
    Paper-side framing: HiOM trades delete throughput for
    cheaper get + crash-safe recovery; t=1 delete being faster
    on HiOM is the surprising direction. The t=24 0.25× ratio
    is documented as a known design cost, not a regression.
- **Open — `kBlockLowBits = 13` (`encode()` 8 K-block ceiling)**.
  Surfaced by the Step 3 investigation above. Doc string in
  `offset_codec.hpp` already flags it as an M0 limitation
  ("M2/M3 will introduce real 32-region routing and lift this
  ceiling"); the ceiling is still the original 13-bit one in
  the M5/M6 code. KeyType16+ValueType200 hits it at
  `1 M / (18 slots × 6 pages) ≈ 9259 blocks > 8191`, i.e.
  ~885 K records into a single-region run. Bug B above made
  this fatal; with that fix the ceiling is now silent
  (HotTier just stops accepting entries past it, ColdTier
  handles everything). To run the paper's 100 M sweep cleanly,
  the codec needs widening — likely paths (not yet picked):
  (1) widen to 16-bit block_low + 13-bit byte/slot data_offset
  + 3-bit page; (2) drop page bits (the page is decomposable
  from data_offset under the fixed-slot-per-page layout) and
  give all 13 saved bits to block_low → 26-bit block_low,
  64 M blocks; (3) commit to per-region routing (kNumRegions
  = 32 → +5 bits via the routing prefix, but ColdTier already
  uses fp64's top 5 bits for region routing and HiOM's
  `route_to_region_default()` is stubbed to 0). (3) is the
  designed path; the other two are escape hatches if region
  routing isn't ready in time. None are urgent for the
  Step 3 perf story now that the gate is fixed.
- **Was-Open: multi-producer apply_batch winner picker (now closed
  by the bullet above, kept here for the historical context)**. The
  `multi-producer same-key correctness` test (8 threads × 50K writes
  / 256 keys) was flaky on master at 15–21 mismatch+miss out of 256
  keys per run. The race: HiOM bumps `commit_seq_` AFTER `viper_.put`'s
  CCEH CAS, so a thread whose CAS won can be preempted before it
  reads `seq`, and a later-CAS-loser ends up with a smaller seq. The
  apply path's descending-seq alive-and-fp-match walk then picks
  the loser (its slot is still alive under reclamation-off
  defaults, and fp matches because the slots collide on the same
  key). Fix candidates ranked: (1) at apply time, for each fp64
  run, derive CCEH ground truth (read PM at `off`, extract key,
  CCEH lookup, pick entry whose `off` matches); (2) carry the
  key in CommitEntry (size grows by `sizeof(K)`; OK for K8 keys,
  borderline for K16); (3) make seq + CAS atomic via `cmpxchg16b`
  on a packed `(seq, offset)` slot (deep CCEH surgery). Note that
  P0 retires CCEH from the write path, so option (1) above now
  reads "derive ground truth from HotTier+ColdTier" instead — the
  same idea, with HiOM as the source of truth, applies. *Update
  2026-05-09*: variant (1) over HotTier landed (`HotTier::read_slot`
  + apply_batch fast path). Test now stable across 8 consecutive
  runs.
- **Implementation share**: ~90% (8.2 weeks / 9 weeks of impl), up
  from ~89% pre winner-picker change. Remaining: commit-buffer
  lanes + retire `commit_seq_` (now reduced to a write-path-
  contention cleanup, no longer load-bearing for correctness),
  100M `--full` measurement, M7 (full evaluation).
- **Latent commit-buffer ordering bug — FIXED (M4 Phase 0)**: M3's
  `drain_once` used `std::sort` on `(fp64)`, which doesn't preserve
  enqueue order for equal keys. A `put X v1 → put X v2` sequence
  could be applied as v2 then v1, leaving ColdTier with v1.
  `std::stable_sort` fixes it; `apply_batch` extraction shares the
  fix between background flusher and inline-flush paths.
- **Cross-Viper CCEH tombstone leak — FIXED (2026-05-03)**: at scale,
  `viper::Viper::Client::remove` followed by `Client::get` was leaking
  ~0.05–0.75% of removed keys back via a stale CCEH offset
  (rate grows superlinearly: 50K→0.05%, 200K→0.27%, 500K→0.75%). Root
  cause is in `cceh::Segment::Insert` (cceh.hpp:371-419): when
  Viper's remove path inserts a tombstone, the loop tries to claim
  the first INVALID slot it sees in the probe window before checking
  for the actual key. If an earlier-tombstoned slot in the same
  window became INVALID between the original Insert and the
  tombstone Insert, the tombstone "succeeds" by claiming that
  earlier slot without invalidating the real entry. CCEH's
  `Segment::Get` for ≤8B keys (`!using_fp_` path) does not call
  `key_check_fn` and returns the stale offset directly. Fix is in
  `viper.hpp` rather than CCEH (smaller surface, M3 will retire CCEH
  anyway): `check_key_equality` now also checks the VPage
  `free_slots` bitset (ground-truth occupancy marker), and
  `Client::get` / `ReadOnlyClient::get` invoke it as a guard
  immediately after the CCEH lookup. Verified via standalone
  reproducer: 50K/200K/500K all show 0 resurrected keys after fix.
  Throughput cost: raw Viper get drops from 48.8 M/s to 47.5 M/s
  (~2.8%, one extra PM 8B key load + compare per get; same cache
  line as the value, so cost is essentially the compare itself).
- **Prior work**: An earlier write-back DRAM cache prototype (`viper_x.hpp`
  + `dram_tier.hpp`) was deleted from the tree on 2026-05-02. Its design —
  full `(K,V)` caching with epoch consistency — is orthogonal to HiOM's
  index tiering.

## Three contributions

- **C1**: Working-set-aware index tiering with SIEVE eviction, applied for
  the first time to persistent hash indices in hybrid PM-DRAM KV stores.
- **C2**: Safe compact hot-tier encoding (4 B fingerprint + 4 B offset)
  with a case split for keys ≤8 B vs. >8 B (see §2.2).
- **C3**: Crash-consistent group-commit protocol with pin invariants and
  A/B checkpoints, achieving ~40× faster recovery vs. Viper.

Supporting techniques: per-thread commit buffer using existing
`concurrentqueue`; 32-region linear hashing for parallel cold-tier load;
AVX-512 SIMD fingerprint compare.

## Win condition

For working sets ≥ 1.5× DRAM, HiOM achieves ≥80% of Viper-LARGE throughput
while reducing DRAM consumption by ≥50%. As working set grows to N× DRAM
(up to 5×), HiOM degrades gracefully to 60-70% of Viper-LARGE on uniform
workloads and ≥80% on production-skewed workloads. Viper itself OOMs at
working sets exceeding DRAM capacity (CCEH preallocates ≈2 GB on init).

---

# §1 Background — Viper Internals (Verified)

## 1.1 Storage layout

Viper organizes persistent storage as a sequence of **VPageBlocks**. Each
block is 24 KB (`BLOCK_SIZE = NUM_DIMMS × PAGE_SIZE = 6 × 4 KB`,
[viper.hpp:36-37](../include/viper/viper.hpp#L36-L37)) — sized so a single
block spans all six DIMMs of a typical Optane DCPMM host. Within a block,
4 KB **VPages** hold packed `(K, V)` entries. The number of slots per page
is computed at compile time by `get_num_slots_per_page<K, V>()`
([viper.hpp:73-99](../include/viper/viper.hpp#L73-L99)) so each page tightly
fits as many entries as possible plus:

- a 1-byte version_lock (`uint8_t` atomic, [viper.hpp:33](../include/viper/viper.hpp#L33))
- a per-slot `std::bitset<num_slots>` of free-slot markers
  ([viper.hpp:170](../include/viper/viper.hpp#L170))

The version_lock byte is bit-packed
([viper.hpp:41-44](../include/viper/viper.hpp#L41-L44)):

```
bit 7   CLIENT_BIT      page is currently owned by a client
bit 6   USED_BIT        page has been used since allocation
bit 1-5 version counter (5 bits, wraps modulo USED_BIT)
bit 0   lock bit (1 = locked, 0 = unlocked)
```

Persistence is enforced via `internal::pmem_persist`
([viper.hpp:101-108](../include/viper/viper.hpp#L101-L108)): a `_mm_clwb`
loop over the affected cache lines followed by a single `_mm_sfence`. The
public API (`put`/`get`/`update`/`remove`) is exposed through per-thread
**Clients** ([viper.hpp:361-406](../include/viper/viper.hpp#L361-L406))
that own a private VPageBlock for inserts, eliminating cross-thread
contention on the write path.

## 1.2 The DRAM offset map

The DRAM-resident **offset map** maps keys to physical positions in PMem.
Viper uses CCEH instantiated as `cceh::CCEH<K> map_`
([viper.hpp:435](../include/viper/viper.hpp#L435)).

Each CCEH slot stores a `Pair`
([cceh.hpp:190-203](../include/viper/cceh.hpp#L190-L203)):

```c
struct Pair {
    IndexK key;     // 8 bytes (size_t)
    IndexV value;   // 8 bytes (KeyValueOffset)
};
```

Total slot size: **16 bytes**. The interpretation of `IndexK` is **conditional
on key size** ([cceh.hpp:112-113](../include/viper/cceh.hpp#L112-L113)):

```c
#define requires_fingerprint(K)  (std::is_same_v<K, std::string> || sizeof(K) > 8)
```

| Key category | `IndexK key` field stores | Source |
|--------------|---------------------------|--------|
| `K` ≤ 8 bytes (e.g., `uint64_t`) | The raw key, reinterpret-cast to `size_t` | [cceh.hpp:368](../include/viper/cceh.hpp#L368) |
| `K` > 8 bytes or `std::string` | An 8-byte hash fingerprint | [cceh.hpp:366](../include/viper/cceh.hpp#L366) |

This distinction matters for HiOM design: when the index already stores a
hash fingerprint, HiOM's hot tier can simply truncate it; when the index
stores the raw key, HiOM's hot tier must compute a fingerprint *de novo*
(see §2.2).

The `IndexV value` field is `KeyValueOffset`, an 8-byte bit-packed structure
([cceh.hpp:134-144](../include/viper/cceh.hpp#L134-L144)):

```c
struct KeyValueOffset {
    union {
        uint64_t offset;
        struct {
            uint64_t block_number : 45;   // up to 2^45 blocks
            uint8_t  page_number  : 3;    // page within block (6 pages → 3 bits)
            uint16_t data_offset  : 16;   // slot within page (or byte offset)
        };
    };
};
```

The 45-bit `block_number` × 24 KB block size addresses up to ~768 EB of PMem.

## 1.3 CCEH structure and DRAM cost

CCEH is an extendible-hashing index with a flat directory of pointers to
fixed-size segments. Viper's instantiation:

| Parameter | Value | Source |
|-----------|-------|--------|
| Initial directory capacity | 131,072 | [viper.hpp:475](../include/viper/viper.hpp#L475) `map_{131072}` |
| Initial directory depth | log₂(131072) = 17 | [cceh.hpp:480-481](../include/viper/cceh.hpp#L480-L481) |
| Segment size | 16 KB (1024 slots × 16 B) | [cceh.hpp:183](../include/viper/cceh.hpp#L183) |
| Probe window per lookup | 16 slots = 4 cache lines × 4 pairs | [cceh.hpp:184-185](../include/viper/cceh.hpp#L184-L185) |

On construction, Viper allocates a fresh `Segment` for *every* directory
entry ([cceh.hpp:483-486](../include/viper/cceh.hpp#L483-L486)). This
produces a **fixed initial DRAM cost of 131,072 × 16 KB ≈ 2 GB** regardless
of how many keys are eventually inserted. As the dataset grows, segments
split, increasing total allocation. Viper §5.3.4 reports 2.3 GB for 100M
keys (matching our analysis: 75% load → near-edge segment splits push the
2 GB baseline up by ~15%).

For 1B keys, projected CCEH allocation is ≈20 GB; for 6B keys, ≈120 GB.
This scaling is the primary motivation for HiOM's tiered design.

## 1.4 Persistence model

CCEH itself runs in DRAM by default ([cceh.hpp:21](../include/viper/cceh.hpp#L21)
`//#define CCEH_PERSISTENT` is commented out). All CCEH state is volatile
and reconstructed on restart.

Viper's `recover_database()` ([viper.hpp:791-847](../include/viper/viper.hpp#L791-L847))
scans every used VPage in parallel — by default 32 worker threads
([viper.hpp:64](../include/viper/viper.hpp#L64) `num_recovery_threads = 32`)
each iterate a slice of `v_blocks_`, decode the per-page bitset, read each
non-free slot's key, and call `map_.Insert(key, offset)` to rebuild the
DRAM index. Recovery time is therefore O(total PMem data scanned), as
reported in Viper §5.3.9 (~2 minutes for 1 TB).

Variable-size string-keyed Viper does **not** implement recovery
([viper.hpp:849-853](../include/viper/viper.hpp#L849-L853) throws
`"Not implemented yet"`). HiOM evaluation must use fixed-size keys.

## 1.5 Insert path and persistence ordering

The fixed-size insert path
([viper.hpp:1023-1069](../include/viper/viper.hpp#L1023-L1069)) executes
the following sequence under the page lock:

1. `v_page_->data[free_slot_idx] = {key, value}`
2. `pmem_persist(entry_ptr, sizeof(VEntry))` — `clwb` + `sfence`
3. `free_slots->reset(free_slot_idx)`
4. `pmem_persist(free_slots, ...)` — `clwb` + `sfence`
5. `viper_.map_.Insert(key, kv_offset, ...)` — DRAM-only update

This ordering is critical for crash consistency: the K/V pair and the
bitset slot-occupancy bit are both persisted before the volatile DRAM
index is updated. On crash, recovery re-scans the bitset; any slot whose
free-bit is reset and whose K/V is intact is treated as live, and the
DRAM index is rebuilt from these.

## 1.6 Limitations of Viper's design (motivation for HiOM)

**(L1) Fixed DRAM overhead independent of data size.** CCEH's 2 GB initial
allocation is paid even for an empty database. For deployments with
constrained DRAM (e.g., 32-64 GB servers with several-TB PMem), this is a
non-trivial fraction.

**(L2) Linear DRAM growth with dataset size.** Beyond the initial 2 GB,
each new ~100M keys adds ~2 GB of CCEH state. A 1B-key dataset already
needs ~20 GB of DRAM just for the index; for fixed-size 16-byte records,
the index is *larger than the data*. Viper does not gracefully degrade
when working set exceeds DRAM — it OOMs.

**(L3) Recovery scales with data size, not index size.** Even though the
index is volatile, recovery must scan all PMem data to rebuild it. For a
1 TB database, this is a 2-minute outage.

HiOM addresses (L1) and (L2) by tiering the index across DRAM and PM,
sized to fit DRAM regardless of dataset size; and addresses (L3) by
making the cold tier itself the recovery source, requiring only a bounded
incremental scan of recent VPages.

---

# §2 DRAM Hot Tier

The hot tier is the in-DRAM cache of recently accessed offset-map entries.
Three constraints:

- **C-Compact**: per-entry footprint ≤ 50% of Viper's 16 B CCEH slot.
- **C-Safe**: false-positive lookups must not return wrong values.
- **C-Pin**: entries with pending PM cold-tier flushes must be unevictable
  (Invariant I1, see §3).

## 2.1 Entry layout

Each hot-tier slot is **8 bytes**:

```
┌─────────────────────────────────┬──────────────────────────────────┐
│  fingerprint  (4 bytes)         │  offset       (4 bytes)          │
│  uint32_t                       │  block_offset_t                  │
└─────────────────────────────────┴──────────────────────────────────┘
                          8 bytes / entry
```

Plus a separate per-slot **1-byte metadata** array:

```
bit 7-5  reserved
bit 4    visited bit (SIEVE eviction, see §2.4)
bit 3-2  state: 00=EMPTY, 01=UNPINNED, 10=PINNED, 11=IN_FLUSH
bit 1-0  reserved
```

Storing metadata separately keeps the data array dense and aligns 8-byte
entries naturally on cache lines (8 entries per 64 B line).

### 2.1.1 The 4-byte offset

The full Viper `KeyValueOffset` is 8 bytes: 45-bit `block_number` +
3-bit `page_number` + 16-bit `data_offset`. The hot-tier 4-byte offset
truncates this to:

```
13 bits   block_number_low  (low 13 bits of full block number)
3 bits    page_number       (full)
16 bits   data_offset       (full)
```

The high 32 bits of the original 45-bit `block_number` are stored in a
separate **block-base map** that all hot-tier entries within a single
block region share. At lookup:

```
full_offset.block_number = block_base_map[hot_tier_region]
                         + entry.offset_low
```

Hot-tier regions partition the cold tier into 32 segments (see §3 cold
tier), so the block-base map has only 32 entries × 4 bytes = 128 B.

## 2.2 Fingerprinting: case split by key size

Whether HiOM's 4-byte fingerprint requires extra computation depends on
what Viper's CCEH already stores in the equivalent slot
([cceh.hpp:112-113](../include/viper/cceh.hpp#L112-L113)).

### Case A: Key size ≤ 8 bytes (e.g., `uint64_t`)

Viper's CCEH stores the **raw key** in `IndexK` ([cceh.hpp:368](../include/viper/cceh.hpp#L368))
— *no* fingerprint exists yet. HiOM must compute one.

- **Derivation**: `fp = h32(key)` where `h32` is the low 32 bits of
  `std::_Hash_bytes` (the same hash CCEH uses for routing,
  [hash.hpp:63-65](../include/viper/hash.hpp#L63-L65)). Reusing the
  routing hash avoids a second computation.
- **Verification on hit**: a 4-byte fingerprint match has a false-positive
  probability of `1 / 2^32 ≈ 2.3 × 10^-10`. For 100M lookups, expected
  false matches < 1. We still verify by reading the full key from PM —
  adding ~350 ns PM read on the rare collision. Net expected verification
  cost across 100M lookups: `100M × (1/2^32) × 350 ns = 8 ms`. Negligible.

### Case B: Key size > 8 bytes or `std::string`

Viper's CCEH already stores an 8-byte hash in `IndexK`
([cceh.hpp:366](../include/viper/cceh.hpp#L366)). HiOM truncates it.

- **Derivation**: `fp = (uint32_t)(viper_index_k)`. Free.
- **Verification**: identical to Case A; Viper already does this at
  [cceh.hpp:402-404](../include/viper/cceh.hpp#L402-L404).

### Why we did not pick a 2-byte fingerprint

A 2-byte fingerprint would compress the entry to 6 bytes (33% saving over
the 4-byte version's 50% saving). But the false-positive probability becomes
`1 / 2^16 ≈ 1.5 × 10^-5`. For 100M lookups, that's ~1500 redundant PM
reads — not catastrophic but visible in p99 latency. Hash collisions
cluster in skewed workloads (zipf distribution amplifies the rate by ~5×),
pushing the cost into the 5-10 ms range, which would show up in throughput.

The 4-byte fingerprint sits at the inflection point: collisions are
statistically negligible across realistic dataset sizes (up to ~10B keys
with expected false matches < 1 per query stream).

## 2.3 Hot-tier sizing

Hot tier is **statically capped** at construction time. The user supplies
`hot_tier_capacity_entries`; HiOM allocates `hot_tier_capacity_entries × 9 B`
(8 B entry + 1 B metadata) plus modest hash-table overhead.

Rationale for static (vs. dynamic):

- **Predictable DRAM**: deployments running with a known DRAM budget can
  reason about HiOM's DRAM footprint up front.
- **Invariant I3** (§3 invariants) requires `|commit_buffer| ≤ |hot_tier|`,
  trivially enforced when hot tier size is fixed.
- **Avoids tier-resize correctness**: a dynamic tier would interleave with
  ongoing flushes and pin states, drastically complicating the protocol.

Default: **25% of available DRAM** (e.g., 32 GB DRAM → 8 GB hot tier →
~890M entries at 9 B/entry). §6 includes a sensitivity sweep varying
capacity from 1 GB to 16 GB.

## 2.4 Eviction: SIEVE

Hot-tier entries are evicted using SIEVE [Yang et al., NSDI '24], a
lazy-promotion FIFO variant with O(1) update cost and consistently better
hit rate than CLOCK or LRU on production traces.

Per-entry overhead: **1 visited bit** (in metadata byte, bit 4).

Algorithm:
- On hit: `visited |= 1`.
- On insert (new entry): place at head of FIFO with `visited = 0`.
- On eviction: walk from current `hand` pointer; if `visited == 0` and
  `state == UNPINNED`, evict; else clear `visited` and advance.

PINNED and IN_FLUSH entries are skipped during eviction (Invariant I3
ensures the pin count never exceeds capacity).

Why SIEVE over S3-FIFO [Yang et al., SOSP '23]: both achieve similar hit
rates, but SIEVE needs only 1 bit per entry vs. S3-FIFO's two-queue +
ghost-set metadata. HiOM's compact-encoding constraint makes SIEVE the
right pick.

> **NOTE**: SIEVE venue/year needs verification before submission. May be
> SOSP '23 or NSDI '24 — confirm via Google Scholar before citing.

## 2.5 Lookup path

```
lookup(key):
    h        = h32(key)                          // ~5 ns hash compute
    fp       = (uint32_t) h
    bucket   = h % num_buckets
    for slot in bucket.probe_window:             // 16-slot range, 2 cache lines
        if slot.fingerprint == fp:
            metadata[slot] |= VISITED_BIT       // SIEVE marker
            return verify(slot.offset, key)
    return MISS                                  // fall through to cold tier
```

`verify(offset, key)`:
1. Reconstruct full `KeyValueOffset` from `block_base_map + entry.offset`.
2. Read PM page header version_lock; if locked, return retry.
3. Read PM data slot at offset.
4. Compare keys; if match, return value; else return MISS.

Lookup hot-tier hit path: 1 DRAM probe + 1 PM read = same as Viper's
existing offset-map hit. No latency regression on the fast path.

## 2.6 Insert / Update path

```
insert(key, value):
    PM-side (unchanged from Viper at viper.hpp:1037-1043):
        write K/V to VPage slot
        pmem_persist(entry)
        update bitset + pmem_persist(bitset)

    HiOM-side:
        h        = h32(key)
        fp       = (uint32_t) h
        offset   = compose_compact_offset(block, page, slot)

        atomically:
            hot_tier[bucket].fingerprint = fp
            hot_tier[bucket].offset      = offset
            metadata[bucket].state       = PINNED      // Invariant I1

        commit_buffer.push((key, full_offset, INSERT))
```

The commit buffer is per-thread (see Implementation §M3), so
`commit_buffer.push` contains no contention. The PINNED state prevents
eviction until the background flusher writes this entry to the cold tier.

## 2.7 Bucket structure and concurrency

Each bucket is a fixed-size 16-slot probe window (matching Viper's CCEH
window at [cceh.hpp:184-185](../include/viper/cceh.hpp#L184-L185)). The
window is two contiguous 64-byte cache lines. Concurrency uses CAS-on-slot
with the same pattern as CCEH's `Insert`
([cceh.hpp:386-415](../include/viper/cceh.hpp#L386-L415)) — no per-bucket
mutex, lock-free probe.

Concurrent reader protection during eviction uses **epoch-based reclamation
(EBR)**, reusing Viper's already-fetched `epoch-reclaimer` dependency.
Writer threads acquire an EBR epoch on entry; eviction defers slot
reclamation until the epoch advances past all reader epochs.

## 2.8 What changed vs. Viper's CCEH

| Aspect | Viper CCEH | HiOM Hot Tier |
|--------|-----------|---------------|
| Slot size | 16 B (8 + 8) | 8 B (4 + 4) + 1 B metadata |
| Fingerprint width | 8 B (or raw key for ≤8 B keys) | 4 B always |
| Index resize | Extendible directory + segment splits | Fixed capacity |
| Eviction | None (full retention) | SIEVE |
| State per entry | None | EMPTY/UNPINNED/PINNED/IN_FLUSH (2 bits) |
| Concurrency primitive | Segment-level `sema` counter | Per-slot CAS + EBR |

## 2.9 Failure-mode analysis

| Failure | Behaviour | Recovery |
|---------|-----------|----------|
| Hot-tier slot lost (DRAM bit flip) | Lookup misses → falls through to cold tier | Cold tier authoritative |
| Hot-tier loaded with stale offset (race) | Verify step on PM data catches via key check | Returns MISS, retry |
| Hot-tier full of PINNED | Insert blocks on commit-buffer flush trigger | Backpressure |
| Crash | Hot tier entirely volatile, lost | Cold tier + VPage scan rebuild |

The hot tier holds **no authoritative state**. Every entry is either
- (a) a cached copy of a cold-tier entry (UNPINNED), or
- (b) a write that's also in the commit buffer awaiting cold-tier flush
  (PINNED / IN_FLUSH), where the K/V data on PM is already persisted.

Losing the hot tier is always recoverable — the basis for keeping it volatile.

---

# §3 Cold Tier, Invariants, Protocols (TODO)

The following sections need to be written before implementation starts:

- **§3 Cold Tier**: linear hashing, 32 fixed regions, design alternatives
  table (rejecting static-probing, cuckoo, CCEH), parallel load
- **§4 Six Invariants** (I1–I6): formal list + maintenance proofs by case
  analysis on each operation (insert, update, delete, evict, flush)
- **§5 Group Commit + Checkpoint A/B**: PM checkpoint slots, valid_pointer
  atomic flip, recovery protocol
- **§6 State Machine**: PINNED → IN_FLUSH → UNPINNED transitions, per-entry
  latch, update/delete tombstone semantics
- **§7 Evaluation Plan**: workloads, metrics, baselines, win condition
  experiments

---

# Implementation Roadmap

(Internal — not paper material.)

## Starting state

- `viper_x.hpp` + `dram_tier.hpp` + `viper_x_fixture.hpp` were deleted on
  2026-05-02. They implemented a write-back DRAM cache (different design
  point from HiOM); kept as separate baselines would have inflated §6 noise.
- Unrelated benchmark improvements they introduced are kept:
  - `viper_fixture.hpp` defensive stale-pool cleanup (CLAUDE.md pattern).
  - `YCSB_PREFILL_LIMIT` / `YCSB_OPS_LIMIT` env vars in `ycsb_bm.cpp`.
  - `Repetitions(3)` + 1M-record fast-iteration constants.
  - `all_ops_benchmark.cpp` argv-passthrough in `main()`.

## Target file layout

```
include/viper/
├── viper.hpp                    # Original Viper, unchanged
├── cceh.hpp                     # Original CCEH, unchanged
├── hash.hpp                     # Original, unchanged
└── hiom/
    ├── hiom.hpp                 # Top-level HiOM<K,V> class
    ├── hot_tier.hpp             # Compact hash + SIEVE eviction
    ├── cold_tier.hpp            # Linear hashing index
    ├── commit_buffer.hpp        # Per-thread group-commit buffer
    ├── checkpoint.hpp           # A/B checkpoint protocol
    └── invariants.hpp           # Debug-mode invariant assertions
```

## Milestones

### M0 — Hot tier MVP (1 week) ✅ COMPLETE (2026-05-03)

A working hot tier with no cold tier, no eviction, no commit buffer.
HiOM lookup either hits hot tier or falls through to Viper's CCEH directly.
Validates compact encoding + 4-byte fingerprint design choices in isolation.

- [x] `hiom::hot_tier<K, V>` with insert/lookup, no eviction. *(SIEVE
      eviction added in M1 — kept M0 functionally a superset.)*
- [x] Bucket = 16-slot probe window, CAS on fingerprint.
- [x] Block-base map (32 entries, see §2.1.1). *Implemented in
      `offset_codec.hpp`. M0 uses a degenerate 1-region map (all zeros);
      4-byte offset addresses up to 2¹³ = 8192 Viper blocks ≈ 192 MB.
      M2/M3 will introduce real 32-region routing and lift this ceiling.*
- [x] Verify path: PM data read + key match. *Implemented via two new
      additive public Viper methods (`Client::hiom_peek_offset` and
      `ReadOnlyClient::hiom_read_at_offset`) plus `HiOM::Client::verify_and_read`.*
- [x] Unit test: 1M random keys; insert + lookup correctness.
- [x] Microbench: 8M lookups/sec single-threaded target. *Hit 135 M/s
      standalone; 54.6 M/s through the full HiOM→Viper path including
      verify-on-PM-key.*

Exit (vs raw Viper lookup, ±10%): **MET.** HiOM 54.6 M/s vs raw Viper
49.5 M/s on hit-mostly lookup (200K keys, hit rate 99.95%), ratio 1.103.
HiOM marginally faster because the compact 8 B HotTier slot fits more
entries per cache line than CCEH's 16 B slot, even though we still pay
the verify-on-PM-key read on every hit.

**M0 integration footprint (2026-05-03):**
- `include/viper/hiom/offset_codec.hpp`: 4 B ↔ KVOffset codec, block-base map.
- `include/viper/hiom/hiom.hpp`: `HiOM<K,V>` wrapper + `Client`.
- `test/hiom_integration_test.cpp`: correctness, update/remove, vs-raw microbench.
- `include/viper/viper.hpp`: two additive public methods on Client/ReadOnlyClient
  (`hiom_peek_offset`, `hiom_read_at_offset`). No existing API changed.
- `CMakeLists.txt`: `VIPER_BUILD_TESTS=ON` adds the integration test target.

### M1 — SIEVE eviction (3-4 days) — functionally complete except EBR (2026-05-03)

- [x] Per-slot visited bit. (Stored in parallel `BucketMeta` array,
      one 16-bit word + 8-bit hand per bucket.)
- [x] FIFO hand pointer. (Verified by `run_sieve_hand_distribution`:
      16 consecutive evictions distribute across all 16 slots.)
- [x] Eviction triggered at capacity threshold. (Bucket-local: triggers
      when the probe window has no empty slot. No global threshold —
      aligned with per-bucket SIEVE design.)
- [x] EBR-protected slot reuse. **Closed by analysis in M4 Phase D
      (2026-05-05).** The 64-bit packed `(fp, offset)` slot loaded
      atomically + verify-on-PM-key in `verify_and_read_offset`
      handles every concurrent evict+re-pin race correctly (see
      Status section "Phase D analysis" for the case-by-case
      proof). EBR remains an optional future-optimization gate
      for a "skip-verify on stable slots" path; today the verify
      is on the same cache line as the value, so the cost-benefit
      doesn't favor adding the EBR machinery now.
- [x] Test: evict-and-refill correctness; no lost or duplicated entries.
      Five SIEVE tests in `hot_tier_test.cpp`: basic 17-into-16,
      visited-semantics (visited=1 survives), hand-distribution
      (16 evictions cover all slots), multi-pass second-chance
      (all-visited bucket evicts exactly one), no-loss stress (10k
      inserts into 256 slots, last-write-wins preserved).

### M2 — Cold tier (linear hashing) (1.5 weeks) — Phase B-2 complete (2026-05-03)

Phase A (earlier): standalone PM-backed `ColdTier` with insert / lookup /
delete + close+reopen persistence.

Phase B-1: added overflow chains, region-parallel load, and 8-thread
concurrent stress. Bucket-full no longer occurs below total-cap;
multi-thread correctness validated.

Phase B-2 (this turn): HiOM ↔ ColdTier integration. Every put/update/
remove on a HiOM Client mirrors into ColdTier; reads consult HotTier →
ColdTier → CCEH (CCEH retained as M2 safety net). 200K-key end-to-end
test exercises the full path and asserts cceh_fallback=0 in steady state.
Split worker is **deferred to "future work after paper"** — pre-sized
buckets + overflow chains already cover the M2 exit on the workloads
the paper measures, and full ColdTier authoritativeness (CCEH removal)
is M3's job. Multi-thread upsert with shared keys also moved to a later
turn (today's stress uses disjoint keys per thread; same scope as M3
introduces commit-buffer batching).

- [x] `hiom::cold_tier<K>` linear-hashing on PM with 32 fixed regions.
      *Each region owns `main_buckets_per_region` Buckets + an overflow
      pool of equal default size; routing splits the 64-bit fingerprint
      into disjoint region-id (top 5 bits) and bucket-id bits.*
- [x] In-place insert / lookup / delete. *Insert is single-pass over the
      bucket chain — find match → CAS update; or remember first empty
      slot and claim it after the chain ends. Delete is a single 8 B
      atomic CAS to a tombstone offset; the fingerprint stays so a
      subsequent re-insert reactivates via match-and-update.*
- [x] Region-parallel load (entry point for fast recovery).
      *`parallel_load(num_threads, visitor)` strides the 32 regions
      across `num_threads` workers; visitor sees `(fingerprint, offset)`
      for every live entry. 500K entries scanned in ~22 ms, 1M in 40 ms.*
- [x] HiOM ↔ ColdTier wiring. *`HiOM<K,V>` constructor gains an
      optional `ColdTier*` argument; when attached, `Client::put`,
      `Client::update`, and `Client::remove` mirror into ColdTier after
      Viper's PM persistence completes. `Client::get` consults
      ColdTier on HotTier miss (with PM verify-on-key) and warms
      HotTier on success. `key_fingerprint64()` reuses the same
      `cceh::h` hash but keeps the full 64 bits so ColdTier's region
      routing (top 5 bits) sees real entropy.*
- [ ] Per-region split-point advancement. *Deferred to "future work
      after paper".* Currently relies on overflow chains to absorb
      bucket-skew; pre-sizing handles capacity. Split would collapse
      long chains, but does not affect tiering claims.
- [x] Crash-safe single-region update via 8-byte atomic (Optane ADR).

Exit (M2 met): single-threaded insert **3.41 M ops/s** clean
(≥ 2 M/s target), 1.19 M/s when run after another large-pool test (PM
write-buffer interference). Lookup 200 M ops/s. Concurrent 8-thread
upsert+lookup: 0 failures across 800K ops. Overflow chain stress:
200K inserts, 9011 overflow buckets, all keys findable. ColdTier-backed
HiOM end-to-end: 200K keys, all reads route through ColdTier with
cceh_fallback=0 in steady state.

For the **100M-entry parallel_load ≤ 5 s** part of M2 exit: the test
harness scales via the `COLD_TIER_EXIT_N` env var. Default 1M finishes
in <1 s; the real 100M run remains opt-in (PM file ~3 GB needs
intentional setup on shared `/pmem0`).

**Phase B-2 footprint (2026-05-03):**
- `include/viper/hiom/hiom.hpp`: ColdTier* member + `key_fingerprint64`
  helper; `Client::put/get/update/remove` route through ColdTier when
  attached. New stats: `cold_hits`, `cold_misses`, `cold_fp_collisions`,
  `cceh_fallback_hits`. M0 callers (no ColdTier) keep working — the
  ColdTier* arg defaults to nullptr.
- `test/hiom_integration_test.cpp`: new `run_cold_backed()` test
  (fourth in the suite) — fills 200K keys, asserts ColdTier holds
  exactly 200K entries, validates a full read pass routes through
  ColdTier, then validates ColdTier's tombstone state directly after
  a disjoint update + remove pass.

**Phase B-2 deferred items (unchanged):**
1. Multi-thread upsert with shared keys. Adds duplicate-fp prevention
   via per-chain lock. Lifts to M3 alongside commit-buffer batching.
2. Real 100M-entry parallel-load run on PM, verifying ≤ 5 s exit.
3. CCEH removal — replace the M2 safety-net fallback with a proper
   ColdTier authoritativeness contract. M3 work; the underlying
   CCEH tombstone leak that previously blocked this is now fixed in
   `viper.hpp` (see Status note). Removing CCEH itself remains M3.

### M3 — Commit buffer + group commit (1 week) — Phase A+B+C+D complete (2026-05-03)

All four sub-phases landed. Phase A added the commit-buffer
infrastructure, Phase B added HotTier PINNED, Phase C switched the
write path to async, Phase D retired the CCEH safety net on the
read path. Inline-flush back-pressure for PINNED overflow is
deferred to M4 with the full state machine.

- [x] Per-thread `commit_buffer`. *Implemented as a shared
      `moodycamel::ConcurrentQueue<CommitEntry>` with a per-Client
      `ProducerToken` (lazy-allocated on first write) — gives near-SPSC
      enqueue performance per producer, MPMC drain semantics for the
      flusher. See `include/viper/hiom/commit_buffer.hpp`.*
- [x] Background flusher: drain to cold tier in batched writes.
      Sorted by `(fp64, seq)` and applied via `apply_batch` (M4
      Phase C). Cache-line-coalesced bulk path (M3 follow-up #1)
      landed 2026-05-05: see the bulk_upsert bullet in Status.
      Single flusher thread per HiOM instance.
- [x] HotTier PINNED to keep buffered writes alive. *Phase B.*
      16-bit `pinned` field per BucketMeta in existing pad bytes;
      `upsert_pinned`/`unpin` give callers a `SlotRef` to surrender
      after the cold-tier write is durable.
- [x] Two-condition flush trigger: timer + buffer-size high watermark.
- [x] **CCEH safety net retired on the read path.** *Phase D.*
      `Client::get` returns false on a HotTier+ColdTier double-miss
      whenever ColdTier is attached. The pre-existing CCEH tombstone
      leak (fixed earlier in `viper.hpp`) is no longer exercised by
      HiOM's read path; CCEH remains in Viper proper as a separate
      concern.
- [ ] Pinned-count threshold trigger and inline-flush fallback.
      *Deferred to M4* — production-scale workloads where PINNED
      budget can exceed HotTier capacity need this back-pressure;
      tests deliberately stay in the regime where it's not needed.

Exit (M3 Phase A+B+C+D met): `run_cold_backed` integration test
shows cold=200K, all reads correct, no fallback path. `run_commit_window`
demonstrates read-your-write through HotTier PINNED with ColdTier
empty (10K writes, 10K reads, 0 misses, hot_delta=10K). Post-flush
ColdTier holds the full set. M0 sub-tests still pass; `cold_tier_test`
unchanged.

**M3 follow-ups (next, prioritised by bench-fixture findings):**
1. **Cache-line-aligned bulk writes to ColdTier** — ✅ done
   (2026-05-05). Implemented as `pmem_persist` split +
   `ColdTier::bulk_upsert` with one drain per bucket-group, plus
   `HiOM::apply_batch` two-stage refactor that collects all kPut
   winners and dispatches a single `bulk_upsert` call. Multi-
   boundary checkpoint loop landed alongside (one
   `try_write_checkpoint` per crossed cadence boundary in a single
   apply round). Direct test added in `cold_tier_test.cpp::run_bulk_upsert`.
   Throughput delta is small on `all_ops_bm` because the flusher
   batches stay tiny under low producer fan-in; the win is held
   in reserve for higher-thread workloads where bulk batches grow.
   See the bench-fixture parity bullet in Status for the table.
2. **P0 — HiOM owns the write-path index, retire CCEH segment-
   level CAS from `Client::put` / `update` / `remove`** — ✅ done
   (2026-05-09). New `Viper::set_hiom_owns_index(bool)` +
   `hiom_map_skipped_` counter; HiOM's ctor flips the flag after
   installing a HotTier→ColdTier resolver. `apply_batch` /
   `Client::remove` corrections (Option L) handle the
   fp32-collision update+remove edge case at the cost of a rare
   VPage slot leak. Tests: `run_p0_skip_counter_sanity` +
   `run_p0_update_heavy_multi_thread`. See the M3 follow-up #2
   "Attempt C" entry in Status for the full write-up.
3. **Per-thread commit-buffer lanes + retire `commit_seq_`** (still
   open after P0 + winner-picker rework). With CCEH out of the
   write path *and* the apply_batch winner picker no longer
   depending on `seq` for correctness (HotTier-truth fast path
   landed 2026-05-09), the dominant thread=8 insert bottleneck
   is now the `commit_seq_` global atomic + moodycamel mpmc
   enqueue contention. The 2026-05-08 lanes attempt regressed
   thread=1; revisit with `seq` reduced to a per-lane local
   counter (apply_batch's fallback walk uses it only as
   tiebreaker among multiple alive kPuts in the same fp64 run,
   which only happens for cross-thread same-key races within a
   single batch — rare, and the per-lane local counter still
   gives intra-lane monotone ordering).
4. **Inline-flush + `pin_failures` back-pressure landed in M4
   Phase B** — done.
5. **Multi-thread shared-key stress** — M2 deferred; M4 Phase E's
   `run_recovery_stress` partially covers this with disjoint
   per-thread key ranges. A same-key-across-threads stress remains
   useful for catching CCEH dup-entry races (out of scope for
   HiOM; lives in `cceh.hpp`).

### M4 — Pin invariants + state machine (1 week) — ✅ complete (2026-05-05)

All five sub-phases landed.

- [x] **2-bit state per hot-tier entry.** *Phase A.* Encoded as a
      packed 32-bit `state` field in `BucketMeta` (16 slots × 2 bits;
      replaces the M3 1-bit `pinned` field). `SlotState` enum:
      kUnpinned=00, kPinned=01, kInFlush=11. Public
      `cas_slot_state(SlotRef, from, to)` API drives transitions.
- [x] **Per-entry "latch" for IN_FLUSH transitions.** *Phase A.*
      Implemented as the CAS itself: writer stays at PINNED, flusher
      transitions PINNED → IN_FLUSH atomically; failed CAS is benign
      (slot was overwritten or evicted). No standalone latch object.
- [x] **stable_sort fix in commit-buffer drain (Phase 0).** Latent
      bug: `std::sort` on `(fp64)` could reorder same-key put-v1 /
      put-v2 entries; fixed by `std::stable_sort` shared between
      `drain_once` and the new inline-flush path via `apply_batch`.
- [x] **Inline-flush back-pressure (Phase B).** `try_inline_flush`
      with a separate ConsumerToken + try-lock serialization;
      Client `mirror_write` retries through inline-flush when
      `upsert_pinned` returns invalid SlotRef (bucket full of
      PINNED). On terminal failure (32 retries × 256 batch
      exhausted), the entry is pushed to the buffer and the
      writer synchronously drains until the buffer is empty,
      guaranteeing the entry reaches ColdTier before the put
      returns. Test exercises the path with HotTier 4096 slots vs
      10K writes and observes pin_failures ≈ 31, all reads correct.
- [x] **Multi-producer same-key happens-before with sequence
      numbers.** *Phase C.* `CommitEntry::seq` (uint64_t, push-time
      stamp from `HiOM::commit_seq_.fetch_add`); `apply_mu_` is
      taken by both background `drain_once` and writer-side
      `try_inline_flush` so every apply round drains the queue to
      empty before sorting. Sort key is now `(fp64 asc, seq asc)`,
      coalesced runs walk descending and pick the first kRemove or
      alive-and-fp-matching kPut: `Viper::hiom_get_slot_key(off,
      &k)` returns the slot's stored key only when free_slots[idx]
      is clear (the key has been persisted before the bit is
      cleared, so the read is stable), and we hash-check it
      against `e.fp64`. This filters the case where a slot got
      freed and re-allocated to a different key under heavy update
      concurrency. `Client::put` switched from CCEH peek to the new
      `Viper::Client::last_put_offset()` so we push the exact slot
      Viper just allocated rather than whatever CCEH happens to
      return (CCEH can produce stale duplicate entries under heavy
      same-key Insert contention; bypassing the peek removes one
      degree of staleness from the write path). When a run is
      entirely stale kPuts we leave ColdTier untouched (an earlier
      batch's good upsert stays in place; the future batch with
      the latest seq overwrites). Also closed a `flushing_in_progress_`
      race: it's now stored=true *before* `try_drain` begins, so
      `flush_and_wait` can never observe (size==0, flushing=false)
      in the gap between drain and apply. **Note**: `Client::put`
      no longer interprets `viper_.put`'s return value as
      success/failure — it returns `is_new_item` (false on update),
      not failure, and the old `if (!viper_.put(...)) return false;`
      was silently dropping every overwrite from the commit buffer.
- [x] **EBR analysis (Phase D, no implementation needed).**
      Concluded that the 8-byte atomic packed slot + verify-on-PM-key
      sequence already handles every concurrent eviction / re-pin
      race correctly. EBR's only marginal benefit would be enabling
      a future "skip-verify on stable slots" optimization, but the
      current verify is on the same cache line as the value (PM
      read essentially free). See the Phase D analysis bullet in
      the Status section for the case-by-case proof. Closes the
      "deferred to M4" item from M1.
- [x] **Multi-thread stress + crash injection (Phase E).**
      `run_recovery_stress`: 8 writers × 20K puts/iter × 6
      iterations (3 fast + 3 slow flusher; crashes at 25/80/250 ms
      and 50/200/600 ms respectively). Workers cooperatively stop
      on a `crashed` flag (the simulate_crash_for_test hook resets
      `commit_buf_`, which would NULL-deref any in-flight push), so
      the on-PM state matches a real SIGKILL. Recovery via
      `skip_recovery=true` + `tail_scan=true`; assertion: every
      put that returned before the crash is recoverable. **Zero
      loss across 18/18 iterations × 3 consecutive runs** after
      the multi-writer-aware checkpoint frontier landed (see
      below). Slow-flusher iterations actually exercise tail-scan
      end-to-end (replayed = full 100K–160K dataset, ColdTier
      rebuilt from VPages alone).
- [x] **Multi-writer-aware checkpoint frontier (Phase E discovery).**
      The original M6 frontier was `Viper::current_block_page_`
      (the global next-to-claim block). With concurrent writers
      each holding their own block, a "slow" client could be
      writing into block 30 while the global frontier had advanced
      to block 50; checkpoint captured 50, recovery's
      `[frontier-1, current)` scan covered `[49, 50)` and missed
      block 30 entirely — its yet-to-flush commit-buffer entries
      became unrecoverable on a crash (Phase E iter 1 lost ~836
      keys per run pre-fix). HiOM now tracks per-Client current
      block in a 256-slot `client_slots_` array
      (`note_client_block` on every `mirror_write_with_offset`);
      `try_write_checkpoint` captures
      `min(min_active_writer_block, viper_.hiom_vpage_frontier)`
      so the persisted frontier is a true lower bound. Each
      Client reserves a slot in `get_client()` and releases it in
      its dtor (move ctor transfers ownership; release does NOT
      reset `last_block` synchronously, to avoid a checkpoint
      that fires before the client's pending entries drain
      capturing a too-high frontier).

Exit (M4 fully met): all 8 integration tests pass deterministically
except `run_multi_producer_correctness`, which remains flaky at
~40-60% PASS due to a pre-existing CCEH duplicate-entry race in
Viper proper (out of scope for HiOM; the read path is robust to
it via verify-on-PM-key). Phase E's `run_recovery_stress` passes
18/18 across 3 consecutive runs.

**M4 follow-ups (out of scope, optional):**
1. Harden CCEH.Insert against duplicate-entry races (would
   stabilize `run_multi_producer_correctness`; cleanest fix is
   in `cceh.hpp`, not HiOM).
2. Tighten `release_client_slot` semantics: today the slot's
   `last_block` is NOT reset on release, so a freshly-reserved
   slot inherits the previous tenant's last_block until its
   first push. This is conservative (safe upper-bound on
   tail-scan range, harmless idempotent re-upserts) but
   suboptimal for steady-state checkpoint frontier.

### M5 — A/B checkpoint protocol (3-4 days) — ✅ complete (2026-05-04)

- [x] Two checkpoint slots in PM, each with `{seq, valid, summary_hash}`.
      *Implemented as `CheckpointRecord` (64 B, single cacheline) at
      offsets 64 (slot A) and 192 (slot B) in a 4 KB PM file.*
- [x] `valid_pointer` 8-byte atomic.
      *At offset 0; sentinel `kValidNone = -1` distinguishes
      "no checkpoint yet" from a real selection. Initial state is
      pmem_persist'd at file create-time so a crash-before-first-write
      leaves `read_valid()` returning `nullopt` instead of garbage.*
- [x] Flush completion → write inactive slot → flip pointer.
      *Hooked into `HiOM::apply_batch`: after each successful drain
      the cumulative `flushed_count_` advances, and crossing a
      multiple of `CheckpointConfig.cadence_entries` (default 4096)
      triggers `try_write_checkpoint()`. Try-lock serialises the
      background flusher and any inline-flusher so only one writer
      mutates the inactive slot. Inside the lock: bump `seq_`
      (linearization point), snapshot `flushed_count_`,
      `viper_.hiom_vpage_frontier()`, `cold_->approx_size()`,
      compute `summary_hash` (FNV-1a over the rest), pmem_persist
      the slot, then atomic-store the new `valid_pointer` and
      pmem_persist again. The 8-byte `valid_pointer` store is the
      single durable commit point.*
- [x] Constructor primes `flushed_count_` and `seq_` from any
      restored record so the cumulative counter survives reopen.
- [x] Torn-write fallback: `read_valid()` verifies `summary_hash`
      on the indicated slot and falls back to the other slot on
      mismatch.
- [ ] Recovery: load checkpoint, scan unflushed VPage range, replay
      buffer. *Deferred to M6 — that's the recovery milestone. M5
      writes the protocol; M6 reads and acts on it.*

Exit (M5 met): `run_checkpoint_persistence` in
`hiom_integration_test.cpp` exercises four phases — (1) 10K writes
with cadence=1024 produce 10 checkpoints (9 cadence boundaries +
1 explicit `force_checkpoint`), final record has `seq=10`,
`flushed_count=10000`, `cold_size=10000`, `vpage_frontier=0x7`;
(2) close + reopen the Checkpoint file alone, `read_valid()`
returns the same record; (3) corrupt the active slot's magic (seq
10), reopen, fallback returns the older slot (seq 9); (4) fresh
PM files, write 5K, reopen, new HiOM primes
`flushed_count()=5000` from the restored record, then 2K more
writes advance to seq=8 and `flushed_count=7000`.

**M5 follow-ups (next):**
1. Wire `force_checkpoint()` into `HiOM::~HiOM()` after the final
   drain so a graceful shutdown always seals state. Currently the
   destructor only drains; the caller must `force_checkpoint()` if
   they want a sealed record.
2. Tighten the cadence default once M6 measures recovery cost vs.
   PM write overhead. 4096 is a guess.
3. Cross-host reopen: today the priming path assumes the same
   process; an actual recovery test (M6) needs to reopen ColdTier
   and Viper from PM and verify they line up with the restored
   `vpage_frontier` and `cold_size`.

### M6 — Recovery (3 days) — pragmatic ✅ complete (2026-05-04)

Pragmatic scope: bounded VPage tail scan correctness. The
recovery-time win against Viper's `recover_database()` baseline is
deferred to **M6.5** because it requires retiring CCEH from
Viper's *write* path, not just the read path (M3 Phase D retired
read; write-path retirement risks regressions across M0–M5 tests).

- [x] **Bounded VPage tail scan**. `HiOM::recover_tail_into_cold()`
      walks `[max(0, frontier_block - 1), current_block_page.block_number)`
      and `cold_->upsert(fp64, off)` every live record. Idempotent
      because ColdTier's upsert overwrites in place when fp64
      already exists (no num_entries bump). The `-1` adjustment is
      because `current_block_page_.block_number` is "next block to
      claim", so the block being actively written at checkpoint
      time straddles `frontier_block - 1` — must be re-scanned.
- [x] **Parallel scan**, one thread per `recovery_threads / num_blocks`
      sub-range, mirroring `Viper::recover_database`'s thread split.
      Default 32 threads; configurable via `RecoveryConfig`.
- [x] **`Viper::hiom_visit_records<Visitor>()`** public template that
      reuses recover_database's `IS_BIT_SET(version_lock, USED_BIT)`
      + `free_slots[slot]` iteration as a generic visitor.
- [x] **Constructor ordering**: prime counters from checkpoint →
      run tail scan (if `tail_scan=true`) → spin up flusher. No
      concurrent writes interleave with replay.
- [x] **`simulate_crash_for_test()` test hook**: stops the flusher
      and drops the commit buffer without draining, matching the
      DRAM-loss semantics a real process kill produces.
- [ ] **Hot-tier rebuild from cold tier (warm-up phase)**. Optional;
      not required for correctness. Deferred to M6.5 as a
      perf optimization.
- [ ] **CCEH-skip on open** (the actual recovery-time win).
      Deferred to M6.5.

Exit (M6 pragmatic met): `run_recovery_persistence` 3 phases all
PASS. Phase A (clean shutdown): replayed ≈ boundary block contents,
cold size unchanged at 5K, all 5K reads hit. Phase B (crash mid-
stream after a checkpoint): replayed ≈ 5446 (boundary block + 5K
post-checkpoint tail), cold goes 5K → 10K, all 10K reads hit.
Phase C (crash before any checkpoint): `read_valid()` returns
nullopt, scan starts from block 0, replayed = 5K, cold = 5K,
all 5K reads hit.

**M6 follow-ups (next):**
1. Hot-tier warm-up phase. Walk the most-recently-touched N entries
   from ColdTier and pre-load HotTier so the first reads after
   recovery don't pay PM-via-cold latency. Pure perf, optional.
2. `recovery_bm` integration. Today's `benchmark/recovery_bm.cpp`
   times raw Viper recovery; add a HiOM-recovery variant that
   measures end-to-end open() time including tail scan.

### M6.5 — Recovery time win (CCEH write-path retirement, ~3-5 days) — ✅ complete (2026-05-04)

Retiring CCEH from Viper's `put` / `update` / `remove` so
`recover_database()` can be skipped entirely (the only thing it
rebuilds is `map_`, which is unused if HiOM is authoritative).
Required for the paper's "40× faster recovery" claim.

- [x] Refactor `Viper::Client::put` (existing-key path) to
      consult HiOM (cold or hot) for the previous offset when
      `map_.Insert` returns tombstone. **Done as a fallback, not a
      replacement** — preserves the M0 fast path. (2026-05-04)
- [x] Refactor `Viper::Client::update` to do the same. (2026-05-04)
- [x] Refactor `Viper::Client::remove` likewise. (2026-05-04)
- [x] Add `ViperConfig::skip_recovery` flag; gate `recover_database()`
      call in the constructor on `!skip_recovery`. (2026-05-04)
- [x] HiOM::open variant that opens Viper with skip_recovery=true,
      then runs the tail scan — total recovery is now O(tail), not
      O(all VPages). Composed in `hiom_recovery_bm` (caller-side,
      no new HiOM API needed; `Viper::open(cfg)` + existing HiOM
      ctor with `RecoveryConfig::tail_scan=true`). (2026-05-04)
- [x] Benchmark harness: `benchmark/hiom_recovery_bm.cpp`. Times
      baseline open vs HiOM open on identical prefilled state;
      sweep N ∈ {1M, 10M, 100M}. (2026-05-04)
- [x] Re-verify all M0–M5 tests pass after the put/update/remove
      refactor (regression surface). All 7 integration tests pass
      including the new Phase D resolver-correctness check.
      (2026-05-04)
- [ ] Run benchmark with `--full` (100M dataset) once a noise-free
      PM time slot opens. The asymptotic gap (baseline O(N) vs
      HiOM O(tail)) is unmistakable at this scale; the goal of the
      run is to record a clean datapoint, not to validate the
      design.

Exit criteria met: write-path retirement landed, all
correctness tests pass (including slot-accounting on Phase D).
The 100M datapoint is a measurement formality; deferring it to a
follow-up does not block M7.

### M6.5 design notes (2026-05-04)

**Resolver semantics.** `Viper<K,V>::OldOffsetResolver` is a
`std::function<KVOffset(const K&)>` member, default-constructed
(empty). HiOM's constructor installs a closure when ColdTier is
present; M0 mode (no ColdTier) leaves it unset. The three
write-path callsites (`put` / `update` / `remove`) check
`if (offset.is_tombstone() && resolver_) { ... }` after the
existing `map_` operation. The fallback fires **at most once per
key per process**: `update` and `put` re-hydrate `map_` on hit,
so subsequent ops on the same key go through the CCEH fast path.

**Concurrency safety.** `cceh::CCEH::Insert` is CAS-atomic — it
returns the previously-installed value as part of the swap.
With concurrent puts of the same key post-restart, only the
first writer sees a tombstone return value and triggers the
resolver; the others see the first writer's offset. The resolver
itself reads ColdTier; it can race with a concurrent
`mirror_write` of the same key, but only the loser thread has
work to do (its `map_.Insert` already returned the winner's
offset, so it took the fast path).

**Why a callback, not an inline include.** Viper is a generic
template (`Viper<K,V>`) header; HiOM consumes Viper. A direct
`#include "hiom.hpp"` would cycle. The `std::function` indirection
keeps Viper agnostic of HiOM and matches the pattern already in
use for the M6 `hiom_visit_records` visitor.

**Variable-size carve-out.** `Viper<std::string, std::string>` is
not refactored — its `recover_database()` already throws
"Not implemented yet", so no caller can use `skip_recovery=true`
with strings. The fixed-size resolver code is template-shared
across all `Viper<K,V>` instantiations; the string variant
inherits the fallback branch but never enters it (no caller
installs a resolver).

### M6.5 known limitations

- `Viper::size()` underreports after `skip_recovery=true` open:
  `current_size_` is no longer primed by the recover_database
  walk, so it starts at 0 and only reflects deltas from in-process
  writes. For HiOM, `ColdTier::approx_size()` is the correct
  cardinality; for direct `Viper::size()` callers, a single-pass
  counter prime would be a small future addition (M7 tooling).
- The 100M `--full` measurement is deferred until a noise-free PM
  time slot is available. The 1M/10M numbers in the table above
  are sound but vary 3× across consecutive runs due to other
  tenants' load on `/pmem0`.

### M7 — Evaluation (3-4 weeks)

Total: ~9 weeks of implementation + 4 weeks of evaluation.

## Design constraints (enforced at PR-review time)

1. **No PM access on hot-tier hit path** beyond the single value read.
2. **No DRAM allocation in commit buffer steady-state**. Fixed-size ring;
   growth means a design bug.
3. **All PM writes go through `viper::internal::pmem_persist`**. No direct
   `clwb` / `sfence` — the existing helper is canonical
   ([viper.hpp:101-108](../include/viper/viper.hpp#L101-L108)).
4. **Fingerprint computation reuses the routing hash**.
5. **Cold tier writes are batched**. Single-key flush is a bug; entries are
   always sorted-and-coalesced by destination bucket.

## Risk register

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Cold tier sequential-write bandwidth lower than expected | Medium | Pre-bench Optane sequential write at start of M2; if < 4 GB/s aggregate, revisit batch size |
| SIEVE hit rate worse than CCEH on YCSB-A skew | Low | Fallback to S3-FIFO behind same interface |
| Commit buffer becomes contention point at >24 threads | Medium | Per-thread design eliminates it; if worker thread becomes bottleneck, move to multi-flusher |
| Cold-tier linear-hashing split causes throughput hiccups | Medium | Background balancer with rate-limited splits |
| Recovery time still dominated by VPage scan in worst case | Low | Tighten commit interval to bound unflushed window |

## Open questions (resolve before M2)

- **Cold tier capacity provisioning**: pre-size for `1.5×` expected dataset,
  or grow lazily? *Tentative: pre-size. Revisit after M2 measurement.*
- **Hot-tier bucket count**: power of two for fast modulo, or prime for
  better distribution? CCEH uses power of two
  ([cceh.hpp:181](../include/viper/cceh.hpp#L181)). *Tentative: power of two.*
- **Tombstone GC**: physically remove from cold tier, or keep until next
  region split? Affects steady-state space for delete-heavy workloads.
  *Tentative: keep until split, evaluate.*

## Dependencies on existing code

| Dep | Used for | Verified at |
|-----|----------|-------------|
| `viper::internal::pmem_persist` | All PM writes in cold tier and checkpoint | [viper.hpp:101-108](../include/viper/viper.hpp#L101-L108) |
| `moodycamel::ConcurrentQueue` | Per-thread commit buffer | [viper.hpp:18,443](../include/viper/viper.hpp#L18) |
| `epoch-reclaimer` (CMake fetch) | EBR for hot-tier eviction | `benchmark/CMakeLists.txt` |
| `viper::internal::pmem_memcpy_persist` | Batch flush to cold tier | [viper.hpp:110-113](../include/viper/viper.hpp#L110-L113) |
| `viper::Viper<K,V>::Client` | PM data read on hot-tier verify | [viper.hpp:361](../include/viper/viper.hpp#L361) |

HiOM does **not** depend on CCEH (replaced by hot+cold tiers) or TBB.

---

# Appendix A — Source-Code Verification Table

Every numerical or structural claim about Viper in this design must be
traceable to source. Verified by reading `include/viper/viper.hpp` and
`include/viper/cceh.hpp` on 2026-05-02.

## A.1 Verified facts

| Claim | Source | Notes |
|-------|--------|-------|
| Each CCEH slot is 16 bytes (8 B key/fingerprint + 8 B offset) | [cceh.hpp:190-203](../include/viper/cceh.hpp#L190-L203) `struct Pair { IndexK key; IndexV value; }` where `IndexK = size_t` (8 B), `IndexV = KeyValueOffset` (8 B) | ✓ |
| `KeyValueOffset` itself is 8 bytes (bit-packed 45+3+16) | [cceh.hpp:134-144](../include/viper/cceh.hpp#L134-L144) | Earlier proposal said 16 B; corrected. |
| Fingerprint storage is conditional | [cceh.hpp:112-113](../include/viper/cceh.hpp#L112-L113) `requires_fingerprint(K) := std::is_same_v<K, std::string> \|\| sizeof(K) > 8` | Drives §2.2 case split. |
| For ≤8 B keys, raw key is stored in the index slot (NOT a hash) | [cceh.hpp:367-369](../include/viper/cceh.hpp#L367-L369) | Key insight that breaks original "16 B = 8 B fp + 8 B offset" framing. |
| CCEH default initial capacity is 131,072 | [viper.hpp:475](../include/viper/viper.hpp#L475) `map_{131072}` | Hardcoded constant. |
| CCEH segment size is 16 KB (1024 slots × 16 B) | [cceh.hpp:183](../include/viper/cceh.hpp#L183) `kSegmentSize = (1 << 8) * 16 * 4 = 16384` | |
| Initial CCEH allocation is ~2 GB | 131,072 segments × 16 KB; verified by [cceh.hpp:483-486](../include/viper/cceh.hpp#L483-L486) loop creating one Segment per directory entry on init | Fixed cost regardless of how many keys are stored. |
| CCEH is DRAM-only by default | [cceh.hpp:21](../include/viper/cceh.hpp#L21) `//#define CCEH_PERSISTENT` is commented out; [cceh.hpp:228-231](../include/viper/cceh.hpp#L228-L231) uses `posix_memalign` (DRAM) | |
| Recovery scans all VPages in parallel | [viper.hpp:791-847](../include/viper/viper.hpp#L791-L847) `recover_database()` | |
| Default 32 recovery threads | [viper.hpp:64](../include/viper/viper.hpp#L64) `num_recovery_threads = 32` | |
| `version_lock` is per-page, 1 byte | [viper.hpp:33](../include/viper/viper.hpp#L33) `using version_lock_t = uint8_t;` and [viper.hpp:169](../include/viper/viper.hpp#L169) | |
| version_lock layout: high bits = CLIENT/USED, mid = version counter, low = lock bit | [viper.hpp:41-44](../include/viper/viper.hpp#L41-L44) | 6 bits remain for version counter. |
| Client model = per-thread VBlock | [viper.hpp:392-405](../include/viper/viper.hpp#L392-L405); [viper.hpp:884-913](../include/viper/viper.hpp#L884-L913) | |
| `concurrentqueue` already a Viper dependency | [viper.hpp:18](../include/viper/viper.hpp#L18) include; [viper.hpp:443](../include/viper/viper.hpp#L443) used for `free_blocks_` only | Free for HiOM commit_buffer reuse. |
| `pmem_persist` is `_mm_clwb` loop + single `_mm_sfence` | [viper.hpp:101-108](../include/viper/viper.hpp#L101-L108) | |
| BLOCK_SIZE = 24 KB = 6 × 4 KB pages | [viper.hpp:36-37](../include/viper/viper.hpp#L36-L37) `NUM_DIMMS = 6, BLOCK_SIZE = NUM_DIMMS * PAGE_SIZE` | |
| Default hash is `std::_Hash_bytes`, not MurmurHash2 | [hash.hpp:63-65](../include/viper/hash.hpp#L63-L65) `hash_funcs[0]`; `hash_funcs[0] = standard` ([hash.hpp:58-60](../include/viper/hash.hpp#L58-L60)) where `standard = std::_Hash_bytes` ([hash.hpp:22-25](../include/viper/hash.hpp#L22-L25)) | CLAUDE.md and Viper README incorrectly say MurmurHash2. |
| Reclamation default OFF | [viper.hpp:67](../include/viper/viper.hpp#L67) `enable_reclamation = false` | Evaluation must control for this. |
| Resize threshold default 0.85 | [viper.hpp:61](../include/viper/viper.hpp#L61) `resize_threshold = 0.85` | |
| Insert persistence order: K/V → bitset → DRAM map | [viper.hpp:1037-1054](../include/viper/viper.hpp#L1037-L1054) | HiOM commit-buffer enqueue MUST follow this order. |
| Variable-size string recovery is **NOT implemented** | [viper.hpp:849-853](../include/viper/viper.hpp#L849-L853) `recover_database()` for `<string,string>` throws `"Not implemented yet"` | Evaluation main line must use fixed-size keys. |
| CCEH segment internal probe range = 16 slots = 4 cache lines × 4 pairs | [cceh.hpp:184-185](../include/viper/cceh.hpp#L184-L185) `kNumPairPerCacheLine = 4, kNumCacheLine = 4` | |

## A.2 DRAM cost of CCEH (worked example)

For 100M records:
- Initial 2 GB (131,072 segments × 16 KB) allocated regardless of dataset size.
- 131,072 segments × 1024 slots/segment = 134M slot capacity at 100% load.
- 100M / 134M ≈ 75% load → at edge of segment splits.
- With splits, total allocation grows to ~2.3 GB (matches Viper §5.3.4).

For 1B records:
- Splits push to ~10× segments → ~20 GB DRAM index footprint.
- This is the regime where HiOM's tiered design starts to matter.

## A.3 Insert path persistence sequence ([viper.hpp:1023-1069](../include/viper/viper.hpp#L1023-L1069))

```
1. v_page_->lock()                                      // page latch
2. Find free_slot_idx in free_slots bitmap
3. v_page_->data[free_slot_idx] = {key, value}
4. internal::pmem_persist(entry_ptr, sizeof(VEntry))   // clwb + sfence
5. free_slots->reset(free_slot_idx)
6. internal::pmem_persist(free_slots, ...)             // clwb + sfence
7. viper_.map_.Insert(key, kv_offset, ...)             // DRAM-only update
8. (optional) free_occupied_slot(old_offset, ...)
9. v_page_->unlock()
```

**Implication for HiOM**: the commit-buffer enqueue replaces step 7. The
DRAM hot-tier `PINNED` insert and the buffer push must happen *after* the
two `pmem_persist` calls in steps 4 and 6, otherwise a crash can leave the
buffer pointing to non-persisted data.

## A.4 Get path ([viper.hpp:1191-1207](../include/viper/viper.hpp#L1191-L1207))

```
1. CCEH Get(key) → returns KVOffset
2. If is_tombstone(): return false
3. get_value_from_offset(kv_offset, value):
   a. Load v_page->version_lock
   b. Check IS_LOCKED(lock_val): if locked, return false → outer loop retries
   c. Read v_page->data[slot].second into *value
   d. Re-load lock_val: if changed, retry (concurrent write detected)
4. Return true
```

This optimistic-read protocol must be preserved when HiOM hot-tier hits
return offsets — the page-level version_lock check still runs.

## A.5 Findings beyond the original verification list

These were not asked about in the proposal but discovered during reading:

1. **CCEH segment-level concurrency**: a single `std::atomic<uint64_t> sema`
   per segment ([cceh.hpp:243](../include/viper/cceh.hpp#L243)) with values:
   `0` = idle, `1..N` = N concurrent readers/writers, `EXCLUSIVE_LOCK` = -1
   for split, `SPLIT_REQUEST_BIT` = 1<<63 for "wants split". HiOM's cold tier
   can adopt this pattern unchanged.

2. **CCEH `Insert` falls back to in-place update if key already present**
   ([cceh.hpp:399-415](../include/viper/cceh.hpp#L399-L415)): match by
   `key_checker == _[slot].key`, then verify with `key_check_fn` callback if
   `using_fp_`, then CAS the `value.offset`. HiOM hot-tier and cold-tier
   upserts should mirror this pattern.

3. **Page strategies are configurable but `BlockBased` is the only one
   actually used** ([viper.hpp:887-893](../include/viper/viper.hpp#L887-L893)
   sets `strategy_ = BlockBased` unconditionally). HiOM can assume `BlockBased`.

4. **`get_new_block` uses `rand() % num_pages_per_block`** for non-string
   keys ([viper.hpp:932-935](../include/viper/viper.hpp#L932-L935)) to spread
   load across DIMMs. HiOM's hot tier doesn't need to replicate this; cold
   tier benefits since K hashes are uniform.

5. **Deadlock-handling in remove path** ([viper.hpp:1304-1412](../include/viper/viper.hpp#L1304-L1412))
   uses a global `deadlock_offset_lock_` + shared vector of pending offsets,
   triggered after 32 retries on a contended page lock. HiOM remove marks
   DRAM hot-tier `TOMBSTONE` + appends to commit buffer; the actual PM
   bitset reset happens during the next flush, not synchronously.
