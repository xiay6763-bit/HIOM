# HiOM: Hierarchical Offset Map for Viper

Design document for a tiered offset-map extension to Viper [VLDB '21].
**Target venue (revised 2026-06-05): a master's-thesis companion paper**
(中文核心 / EI / CCF-C or an engineering-systems track) — *not* ICDE/CCF-A.
Scope is deliberately narrowed (see Win condition below): HiOM is a
**DRAM-efficient tiered offset map** for persistent-memory KV stores under a
constrained DRAM budget, trading a bounded throughput loss for large DRAM
savings and bounded-time recovery — not an across-the-board replacement for
Viper.

Code references use the form
`[viper.hpp:101-108](../include/viper/viper.hpp#L101-L108)`. Numbers ending
in `≈` are derived/projected; numbers without `≈` are verified from source
(see Appendix A).

---

## Status (2026-06-08)

- **Four-system read/write LATENCY measured (E2/E4 latency axis) — closes
  "Genuinely remaining (a)".** Dense t=1..24, 100r + YCSB-A/B, K8/V200 @10 M,
  3-rep median HDR percentiles, via a new `TS_METRICS=lat` axis in
  [run_thread_scaling.sh](../benchmark/run_thread_scaling.sh) (default `tp`
  byte-unchanged) + a latency branch in
  [thread_scaling_plot.py](../eval/thread_scaling_plot.py) (p99 solid + p50
  dashed, log-y). Figures: eval/charts/thread_scaling_{100r,ycsb_a,ycsb_b}_lat.pdf.
  NB lat-mode tput is lower (per-op HDR cost) — latency is read from `_lat`
  runs, throughput from `_tp`, never mixed.
  - **Read latency = the throughput win's mirror (strengthens C2).** HiOM read
    **p50 ≈ Viper across every workload and thread count (~0.72–0.97 µs)**, both
    far below the PM-resident camp. The gap vs PM-resident *widens* under write
    load: Dash/CCEH store each value via a per-op PMDK transaction that blocks
    reads, so their read **p99 explodes** — 100r ~1.5–2.8 µs, YCSB-B (5 % wr)
    3.8–7.9 µs, YCSB-A (50 % wr) **49–298 µs** at t=24 — while HiOM read p99
    stays **~1.3–3.2 µs** throughout. ⚠ That Dash/CCEH read-tail blow-up is
    amplified by the SAME fixture artifact as the E4 write caveat (per-op txn
    value store), so frame as "HiOM read latency ≪ PM-resident, partly
    fixture-amplified under writes," not a clean pure-index claim.
  - **vs Viper, the only read penalty is the single-thread 100r tail**: p99
    +~48 % (1.74 vs 1.18 µs), deep p9999 ~4× (14 vs 3.5 µs). Cause is the
    constant **~0.04 % HotTier→ColdTier miss** (hit 0.9996) landing past p99.96
    — workload-independent (zipf ≈ uniform, because the 2²¹-bucket HotTier
    covers the whole 10 M set in both). At **t ≥ 8 the read tail is
    parity-or-better** (shared PM queueing dominates; e.g. b_zipf t8 HiOM 1374
    < Viper 1546 ns). p50 is identical everywhere.
  - **Write/update latency mirrors the YCSB-A throughput cost (documented
    limitation, now also in latency).** HiOM update p50/p99 ≈ Viper at t ≤ 8
    (the in-place fixed-size skip-commit, d613cf9), but **inflates at t=24
    write-heavy** — a_zipf t24 HiOM **2139/6172 ns vs Viper 830/2891** (~2× p99)
    — the ColdTier durability-mirror cost under high write fan-in. YCSB-B
    (5 % wr) write latency ≈ Viper throughout. Consistent with E4 (YCSB-A
    0.46–0.74× tput).
  - **Net: latency corroborates the three-axis positioning** — read-axis win
    confirmed in latency (≈ Viper, ≪ Dash/CCEH, gap widening under writes),
    write a documented cost (t=24 tail inflation). No new short-coming surfaced.

---

## Status (2026-06-07)

- **Read-scaling wall ROOT-CAUSED and FIXED — supersedes the "t=24 HotTier
  per-slot lookup contention" diagnosis in the E2 bullet below.** The flat read
  throughput at ≥8 threads was **not** HotTier lookup contention (slot probes
  are pure `load`s that stay Shared and scale); it was a single global atomic on
  the `get()` hot path — `stats_.hot_hits.fetch_add(1, relaxed)` plus 6 sibling
  read counters, all packed on one `Stats` cache line. Every read RMW'd that one
  line, serializing all threads at the cache-line handoff rate (~12 Mops/s
  plateau, independent of thread count).
  - **Ablation (causal proof, 1M / 100r_uniform, mean M items/s):** compiling
    the 7 read-path increments out (`-DHIOM_READ_STATS=0`, HotTier lookup
    otherwise untouched) lifted HiOM t=24 from **12.3 → 36.1**, i.e. **95 % of
    Viper (37.9)**; the wall vanished and HiOM tracked Viper at every thread
    count (t8 18.4, t16 30.3). t=1 was unchanged (~2.7 for both) — the counter
    is free uncontended, so this is purely a many-core coherence effect.
  - **Fix (shipped):** per-Client telemetry shards. Each `HiOM::Client` bumps a
    plain (non-atomic) `read_shards_[slot_idx_]` on its own `alignas(64)` cache
    line — single-writer per the §Concurrency contract, so zero cross-core
    traffic — and `stats()` folds the shards into the `Stats` aggregate on
    demand, off the hot path. Exact, always-on hit/miss telemetry retained
    (`hot_hit_rate` still reported). Sharded HiOM t=24 = **36.6 (96.5 % of
    Viper)**; all `hiom_integration_test` cases pass (per-tier hit-accounting
    deltas intact). Code: [hiom.hpp](../include/viper/hiom/hiom.hpp) —
    `HIOM_RSTAT_INC` macro, `ReadStatShard`, `fold_read_shards_`. See Claude
    memory `hiom-read-stats-contention`.
  - **Positioning consequence — REMOVES the claimed limitation (confirmed
    @10 M, full 4-system re-measure).** The full 100r thread-scaling was re-run
    post-fix (zipf+uniform, t=1..24) — see the **E2 table below** and
    eval/charts/thread_scaling_100r.pdf. HiOM now **matches/exceeds Viper and
    beats Dash/CCEH at every thread count** (zipf t24 17.7→**46.8**, uniform
    19.9→**35.5**; hit_rate 0.9996 intact). (HiOM edges Viper on zipf because its
    verify reads key+value in one `hiom_read_at_offset`, vs Viper's separate
    key-check + value read.) **The read-axis Pareto win now holds at ALL
    concurrencies — the "scope to low/mid concurrency" limitation is RETRACTED.**
    Pre-shard HiOM curves preserved at
    results/thread_scaling/HiOMFixture_100r_*_10M_tp.json.preshard.
  - **NOT addressed by this fix (separate, still-open walls):** write/mixed
    (YCSB-A) and the undersized-HotTier capacity sweep have their own
    contention — `HotTier::size_`/`eviction_count_` global atomics on
    insert/evict, and the `mirror_into_hot` re-warm CAS storm. Read-path only
    here; the write path (delete wall, update redundant ColdTier re-write) was
    fixed separately afterward — see the next bullet + the refreshed E4 (YCSB-A
    0.46–0.74×, YCSB-B ≈ Viper).

- **Write-path scaling investigated + delete wall FIXED (follow-up to the read
  fix).** Same measure-then-ablate method on insert/delete/update (all_ops_bm,
  K16/V200 1M, t=1/8/24; update also via YCSB-A 1M):
  - **DELETE was contention-walled and is now fixed.** HiOM delete *regressed*
    8→24 threads (t8 3.0 → t24 2.56 M/s) while Viper scaled to 10.6. Ablation
    pinned the cause: NOT `HotTier::size_` (gating it barely moved the number)
    and NOT the commit-buffer `size_hint()` global walk — it was
    `push_commit`'s **`wake_all_flushers()` storm**. Once the flushers fall
    behind under a write storm the lane sits above the wake threshold, and the
    old `size_hint() >= high_watermark` test then fired wake_all_flushers (=
    kNumLanes wake-slot mutexes + futex wakes) on *every* push. Fix:
    `CommitBuffer::push()` now returns the lane's post-enqueue depth, and
    push_commit wakes only on the **rising edge through a per-lane watermark**
    (`depth == high_watermark/kNumLanes`) — at most once per drain-cycle, never
    every push, and never the kNumLanes-atomic `size_hint()` walk. (An interim
    `depth==1` edge-wake fixed t=24 but collapsed t=1 to 0.28 via a futex-wake
    *per op* when the flusher re-parks between ops — the exact-watermark edge
    avoids both.) Result: **delete t=24 2.56 → 5.21 M/s (2.03×), scales 8→24
    again** (≈ the 5.31 ceiling of "don't push to the commit buffer at all", so
    commit-path contention is essentially gone). t=1 unchanged. Code:
    [commit_buffer.hpp](../include/viper/hiom/commit_buffer.hpp) `push()`,
    [hiom.hpp](../include/viper/hiom/hiom.hpp) `push_commit` /
    `per_lane_high_watermark_`. All `hiom_integration_test` (incl. recovery +
    crash-injection) pass.
  - **INSERT — RESOLVED (2026-06-07 paper-scale re-measure): NOT an op-count
    artifact; this host's PM write path is the ceiling.** I first called it
    "bandwidth-bound", then (after the Fig.6 cross-check) suspected the 1M run
    was a small-sample artifact. Re-measuring at 10M inserts settles it: Viper
    insert t24 = **2.15** M/s (was 3.16 at 1M — *lower*, not higher), t36 = 2.71;
    HiOM 1.28–1.54 (≈0.5–0.7× Viper). More ops did NOT approach the paper's 15M
    PUT @ 36t — so it is **not** small-sample. Root cause is the **environment,
    not the code**: `/pmem0` is **ext4 + dax=always (FSDAX)** and a **shared
    mount** (ljw/zzk/roert/test/HGBTree/… all write it). Viper t36 = 2.71 M/s ×
    216 B = **0.59 GB/s**, below a single Optane DIMM's write BW, and scaling is
    non-monotonic (t8 1.91 > t16 1.58) — the signature of fsdax overhead +
    concurrent cross-tenant PM-write contention. The paper's 15M was on a
    dedicated 6-DIMM devdax machine.
    - **Consequence:** insert/write **absolute** throughput on this host is
      **not comparable to the Viper paper** and is noisy run-to-run. Report the
      host-robust **HiOM/Viper ratio (≈0.5–0.7×)** instead, which isolates
      HiOM's real algorithmic cost — the extra ColdTier mirror write per put —
      from the shared-hardware ceiling. Mechanism (PM-write-BW-bound for both
      systems) holds; only the ceiling is host-specific. See Claude memory
      [[hiom_bench_numa_topology]].
    - ⚠ The all_ops `update` cell is separately untrustworthy (reports 0.4–10
      G/s for *both* systems — impossible for real PM updates); use YCSB-A below
      for update.
  - **UPDATE: the all_ops `update` cell is a measurement artifact** (keys not in
    the updated range → no-op fast path, reports 0.4–10 G/s for both systems —
    ignore it). Real update = YCSB-A (50% update-in-place / 50% read). Remaining
    gap vs Viper = in-place PM write (= Viper) + resolver key-verify read. The
    **redundant ColdTier re-write** that used to sit on this path (in-place
    update keeps the same offset, so the flushed cold upsert was a no-op write)
    is now **eliminated — DONE 2026-06-07**: `Client::update` skips the
    commit-buffer push for in-place *fixed-size* updates (offset unchanged ⇒
    ColdTier already correct) and only refreshes the SIEVE bit via non-pinned
    `hot_.lookup`; variable-size `std::string` V (Viper may relocate it to a new
    offset) keeps the `mirror_write(kPut)` path via an
    `if constexpr (std::is_same_v<V, std::string>)` guard — crash-safe, since
    with no commit entry there is no unflushed window to replay. Net effect is
    the **post-opt @10 M YCSB-A/B E4 tables below** (a_zipf t24 0.46×, a_uni
    0.74×, b ≈ 0.92–1.05×); the interim @1 M figures (0.46–0.55× pre-opt) are
    superseded. `hiom_integration_test` (recovery + crash) still pass.

- **E2/E4 four-system main table landed (priority-5 baseline expansion)** —
  the §7 evaluation's last open block. `ycsb_bm` now runs HiOM + Viper + Dash +
  CCEH in one harness on stock PMDK (K8/V200, the paper's real workload);
  Dash/CCEH were re-enabled in `ALL_BMS`
  ([ycsb_bm.cpp:265-266](../benchmark/ycsb_bm.cpp#L265-L266)). A 10 M
  four-system sweep (6 workloads × t=1/8/24 × 3 reps, via
  [run_scaling_sweep.sh](../benchmark/run_scaling_sweep.sh) with new
  env-overridable `SWEEP_{FIXTURES,SIZES,WORKLOADS,METRICS}` axes) completed
  **12/12 cells clean, no timeouts**. Dash 10 M per-op-transaction prefill ≈90 s,
  so cells are 2–10 min each.
  - **Bug fixed en route**: `CcehFixture`'s YCSB GET counted a hit via
    `entry_ptr->second == record.value`, but YCSB READ records carry no value
    → **found=0 (all reads silently "missed")**. Fixed to match Viper/Dash
    (`!= null_value`),
    [cceh_fixture.hpp:269-279](../benchmark/fixtures/cceh_fixture.hpp#L269-L279).
    Smoke then clean for all four systems.

- **E2 (read, YCSB-C / 100r) @10 M — RE-MEASURED post-shard-fix (2026-06-07);
  HiOM now wins the read axis at ALL concurrencies.** Aggregate Mops/s (median,
  t=1/8/24), from results/thread_scaling/ via `thread_scaling_plot.py`
  (→ eval/charts/thread_scaling_100r.pdf). HiOM rows are post-shard;
  Viper/Dash/CCEH are unchanged by the fix (re-confirmed within run-to-run
  variance):

  | sys   | zipf t1 | t8 | t24 | uni t1 | t8 | t24 |
  |-------|--------:|---:|----:|-------:|---:|----:|
  | Viper | 2.6 | 22.6 | 43.2 | 2.6 | 18.1 | 35.9 |
  | HiOM  | 2.9 | 21.0 | **46.8** | 2.3 | 18.4 | **35.5** |
  | Dash  | 1.6 | 13.4 | 31.3 | 1.7 | 12.0 | 22.5 |
  | CCEH  | 1.2 | 8.2 | 23.8 | 0.9 | 7.1 | 19.9 |

  HiOM > Dash > CCEH at every thread count, and HiOM **matches/exceeds Viper at
  t=24** (zipf 46.8 vs 43.2; uniform 35.5 vs 35.9 — within noise; HiOM edges out
  on zipf because its verify reads key+value in one `hiom_read_at_offset`, vs
  Viper's separate key-check + value read). **The read-axis Pareto win holds at
  all concurrency — the earlier "inverts at t=24 / scope to low-mid concurrency"
  limitation is RETRACTED.** Pre-shard HiOM was flat 15→17→17.6 from t=8 (shown
  for the record below); that wall was the single contended
  `stats_.hot_hits.fetch_add`, NOT "HotTier per-slot lookup contention" as first
  guessed — fixed via per-Client stat shards (see top bullet). hot_hit_rate
  stays 0.9996.

  > Pre-shard (buggy) HiOM 100r for reference: zipf t8/16/24 = 15.1/17.0/**17.6**,
  > uniform = 16.1/18.8/19.9 — the flat wall this fix removed. Raw kept at
  > results/thread_scaling/HiOMFixture_100r_*_10M_tp.json.preshard.

- **E4 (write, YCSB-A/B) @10 M — RE-MEASURED post-fix (2026-06-07; update
  in-place skip-commit + rising-edge flusher wake + read-path shards).** Full
  4-system thread curves (1/2/4/8/16/24, same format as the 100r read figure)
  at eval/charts/thread_scaling_ycsb_{a,b}.pdf. Aggregate Mops/s (median):

  **YCSB-B (read-mostly, 95 % read / 5 % update):**

  | sys   | zipf t1 | t8 | t24 | uni t1 | t8 | t24 |
  |-------|--------:|---:|----:|-------:|---:|----:|
  | Viper | 2.8 | 18.0 | 45.1 | 2.3 | 17.3 | 32.9 |
  | HiOM  | 2.8 | **19.0** | 42.8 | 2.5 | 16.0 | 30.3 |
  | Dash  | 1.3 | 8.9 | 16.7 | 1.1 | 7.6 | 16.2 |
  | CCEH  | 0.9 | 6.8 | 17.0 | 0.7 | 5.3 | 13.8 |

  **YCSB-A (write-heavy, 50 % read / 50 % update):**

  | sys   | zipf t1 | t8 | t24 | uni t1 | t8 | t24 |
  |-------|--------:|---:|----:|-------:|---:|----:|
  | Viper | 2.9 | 17.5 | 25.5 | 2.6 | 14.3 | 19.1 |
  | HiOM  | 2.5 | 10.3 | 11.7 | 2.0 | 10.1 | 14.1 |
  | Dash  | 0.4 | 2.5 | 2.3 | 0.3 | 2.0 | 2.6 |
  | CCEH  | 0.3 | 2.0 | 2.1 | 0.3 | 1.9 | 2.4 |

  **YCSB-B (read-mostly): HiOM 0.92–1.05× Viper** — parity, and it even *edges*
  Viper at zipf t8 (19.0 vs 18.0); **~2.5× Dash/CCEH** at t24. Read-mostly is
  HiOM's comfort zone: the 5 % updates are nearly free now that the read half
  rides the per-Client shards and the update half skips the redundant commit
  push. **YCSB-A (write-heavy): HiOM 0.46× (zipf t24) – 0.74× (uniform t24)
  Viper** — the documented write cost (ColdTier durability mirror on the 50 %
  puts), but **up from the old 0.27–0.34× at t8** thanks to the update-opt +
  wake fix, and still ~5× Dash/CCEH. (zipf is the harder case: θ=0.99 piles the
  50 % writes onto a few hot keys → HotTier-slot + ColdTier-region contention;
  uniform spreads them, so a_uniform holds 0.74×.) **Caveat — state in the
  paper:** the Dash/CCEH *fixtures* persist every value via a per-op
  `pmem::obj::transaction` make_persistent into a side PM pool, so their write
  throughput is transaction-bound, not pure-index — HiOM's write lead over Dash
  is partly a fixture artifact. Frame E4 as "HiOM write < Viper (a documented
  design cost), read-mostly ≈ Viper"; do **not** claim a clean write win over
  Dash. Pre-fix t=8 snapshot kept for reference: a_zipf 4.64 (0.27×), a_uni 4.30
  (0.34×), b_zipf 13.28 (0.80×), b_uni 11.75 (0.81×).

- **Scoped out (2026-06-07 decision — not a gap)**: Dash/CCEH at 5/16/33 M was
  considered and *deliberately dropped*. vs-N's information value sits on the
  DRAM axis (E1 white-box, done) and the graceful-degradation story (Viper/HiOM
  already span 5–33 M); read/write throughput is near-flat in N (Viper/HiOM only
  gently decline 5→33 M), so PM-resident baselines at a single representative
  10 M point suffice as the read-throughput *control*. A full four-line vs-N
  curve would only restate "all four decline gently" — no Pareto consequence.
  Charts thus show Viper/HiOM as 5–33 M curves and Dash/CCEH as a 10 M reference
  point (`eval/charts/scaling_*.pdf`).
- **E1 white-box DRAM for CCEH — DONE (2026-06-07)**: `CcehFixture` now
  implements `fixture_dram_bytes()` (returns `dram_map_->dram_bytes()`) and the
  ctor capacity was corrected `1000000 → 131072`. The arg is `initCap` with
  directory depth = `log2(initCap)`, so `1000000` silently meant depth 19
  (524288 segments × 16 KiB ≈ 8 GB); `131072` = depth 17 = Viper's exact
  `cceh_init_cap`. Result: CCEH RSS **8.6 GB → 2.4 GB**, white-box index DRAM
  now **2052 MB = identical to Viper** (same cceh, same init cap). Read tput
  unaffected (smoke 1.13 → 1.57 M/s — slightly *faster* with the smaller,
  cache-friendlier directory). **E1 memory footprint — four-system stacked bar (Viper-paper Fig.9 style)**.
  Each bar stacks PMem (data + PM index) then DRAM (index) on top; chart at
  `eval/charts/footprint.pdf` (`eval/footprint_plot.py`). DRAM is white-box
  measured; PMem is analytical (data = N×216 B with VPage ≈ raw and per-entry
  PMDK alloc +~10% per Viper Fig.9; PM index from struct params). @10 M:

  | system | DRAM (index) | PMem (data + index) | total | DRAM:PMem |
  |--------|-------------:|--------------------:|------:|----------:|
  | Viper  | 2052 MB | 2.07 GB | **4.07 GB** | 1:1 |
  | CCEH   | 2052 MB | 2.32 GB | 4.33 GB | 1:1 |
  | HiOM   | **272 MB** | 2.16 GB (incl. 96 MB ColdTier) | **2.43 GB** | 1:8 |
  | Dash   | ~0 | 2.53 GB (incl. ~210 MB index) | 2.53 GB | 1:1295 |

  Story (Viper Fig.9's argument: **DRAM is scarce** — ~1/8 the capacity, ~9× the
  $/GB): HiOM cuts the scarce-DRAM footprint to **272 MB vs 2052 MB = −87%** vs
  the DRAM-index camp (Viper/CCEH), and total memory **2.43 vs ~4.1 GB = −40%**
  at 10 M, by moving the authoritative index to PMem (ColdTier) behind a small,
  *fixed-size* DRAM hot tier — HiOM's DRAM is constant in N while Viper's CCEH
  grows. The stacked bar lets Dash sit naturally at the PM-resident extreme
  (DRAM ≈ 0): that 0 is not free — it is paid back in read throughput (E2,
  Dash is the slowest reader), i.e. the opposite end of the DRAM×read Pareto.
  (At 10 M, Viper's DRAM:PMem is ~1:1 because CCEH's 2 GB is *pre-allocated*
  against ~2 GB of data; the Viper paper's 1:9 is at 100 M. HiOM's hot tier is
  likewise pre-allocated, so the comparison stays apples-to-apples.)

- **Genuinely remaining (this line of work)**: nothing blocking. **Baselines
  frozen (2026-06-08, thesis scope)**: run set = **Viper + Dash + CCEH (+ HiOM)**;
  **Halo optional** (the one DRAM+recovery opponent worth a time-boxed build —
  cite-acceptable otherwise). [(a) four-system `lat` metric — **DONE 2026-06-08**,
  see the Status (2026-06-08) block at top: read latency ≈ Viper / ≪ Dash-CCEH,
  write t=24 tail inflation mirrors YCSB-A. (b) secondary Halo-harness systems —
  **resolved: Halo optional, others dropped**, see §7.2 / §7.4.]

---

## Status (2026-06-05)

- **Paper repositioning — narrowed from ICDE/CCF-A to a master's-thesis
  companion paper** (中文核心 / EI / CCF-C or an engineering-systems
  track). Rationale: the current evidence supports a *scoped* claim, not
  an across-the-board win over Viper. Write-heavy throughput is weak
  (YCSB-A/B 0.24–0.45× at t=24), `a_zipf-33M` livelocks (M4 back-pressure),
  and (at the time of this repositioning) the Phase 2 DRAM numbers were
  computed by subtracting an *estimated* harness footprint rather than
  measured directly — **since resolved**: DRAM is now directly white-box measured
  (88ff547 / 7a2be60, see §Phase 2). Narrowed thesis: **"a DRAM-efficient
  tiered offset map for PM KV stores — trading acceptable throughput loss
  for large DRAM savings and bounded recovery, in DRAM-constrained
  read-heavy deployments."**
  - Rewrote the doc header, `## Three contributions` (DRAM-efficiency is
    now C1; recovery stated as O(tail) vs O(N), not a fixed 40×), and
    `## Win condition` (scoped to DRAM / read-heavy / recovery; write-heavy
    and `a_zipf-33M` moved to an explicit *out of scope / limitations*
    block).
  - **Next steps (priority order, 2026-06-05)**:
    1. ✅ this repositioning (header + contributions + win condition).
    2. ✅ **Direct fixture-DRAM measurement — white-box landed (88ff547);
       Phase 2 grid re-run + tables replaced (7a2be60)**.
       RSS-diff (loaded − baseline) collapses on `repeats:N` (glibc doesn't
       return freed arenas; `DeInitMap` leaves RSS elevated, so rep>0 reads
       a polluted baseline). Replaced by a white-box metric: each fixture
       reports its own index DRAM from `sizeof` of the live structures —
       CCEH / HotTier / ColdTier / HiOM gained `dram_bytes()`, surfaced as
       the authoritative `fixture_dram_mb` counter (RSS-diff kept as
       `fixture_dram_rss_mb` cross-check). 1M `100r_zipf` t=1 smoke:
       white-box **constant across 3 reps (cv=0%)** — Viper 2052 MB vs
       HiOM 272 MB = **−86.7%**, matching the ~2 GB CCEH vs 256 MB HotTier
       design point. **Done (7a2be60)**: the Phase 2 grid was re-run at t=1
       per dataset size (DRAM is workload-independent → one run suffices)
       and the harness-subtraction DRAM tables in §Phase 2 are now replaced
       with white-box numbers (−86.7%, flat across 5–50M).
    3. ✅ **Recovery 100M measured (2026-06-05)** — `hiom_recovery_bm`
       `--full` (single-thread prefill dodges the `a_zipf-33M` livelock),
       then a new `--open-only` mode for a deployment-fair open comparison
       reusing the prefilled PM state. **Result: ~25× faster cold-start
       open** (baseline ~2180 ms vs HiOM fair ~87 ms; ~15× warm-baseline
       conservative). The naïve `--full` 1.0× at 100M was an artifact of
       oversized CCEH (~277 ms) + 16 M-bucket HotTier (~1030 ms) init, not
       recovery work — see §M6.5 recovery wall-clock. **Tail-size /
       checkpoint-cadence sensitivity measured** (`--tail-sweep`,
       reuse-100M, 2026-06-05): tail-scan is **O(tail) linear**
       (49.8 µs/entry, t=1, 7 points 0–1 M); crossover with Viper's
       O(N) rebuild only at ≈736 K entries (~485 blocks), and the
       default cadence=4096 worst-case tail opens in 750 ms (2.9× vs
       2.14 s baseline) — a ~180× margin between cadence and crossover.
       See §M6.5 tail-size sensitivity.
    4. ✅ **HotTier capacity ablation (2026-06-05)** — `scan_hot_capacity.sh`
       sweeps `HIOM_HOT_BUCKETS_LOG2` 16–21 (1 M–33 M slots = 10%–330% of the
       10 M YCSB-C working set), 3-rep median, white-box
       `hot_tier_index_dram_mb` as the x-axis. **Hit rate climbs with the DRAM
       budget and skew pays off at tight budgets**: at 10% budget zipf hits
       0.62 vs uniform 0.19; at 40%, 0.74 vs 0.70; both saturate to ~1.0 once
       the budget reaches the working set (uniform overtakes zipf past ~84% as
       its cumulative coverage outpaces zipf's tail churn). t=8 full-capacity
       read-heavy ratio (HiOM/Viper): **uniform 0.90× / zipf 0.73×** — inside
       the 0.69–0.92 Win band, and clarifies the stale 0.27–0.58 from Status
       05-17 (predated prefill-then-rebuild + M6.6). **Measurement caveat**: at
       high thread counts the *small* capacities livelock in the read path
       (concurrent `mirror_into_hot` spinning on each other's PINNED slots
       during SIEVE eviction — a read-side analogue of `a_zipf-33M`), so the
       capacity curve is measured at t=1, where hit rate (thread-independent)
       is clean; see §Win condition limitations. Artifacts:
       `eval/charts/hot_capacity_{hitrate,throughput}.png`,
       `results/hot_scan/summary.csv`, `results/hot_scan/readheavy_ratio.txt`.
       Remaining priority-4 work: SIEVE vs LRU/random (needs a pluggable
       eviction interface — currently hard-wired), batch size, flusher count,
       checkpoint cadence.
    5. **Baseline expansion — two-tier harness; HiOM + Dash + CCEH all run
       (2026-06-06).** The original "via PiBench" plan was corrected twice:
       (a) `sfu-dis/pibench` is range-only (`tree_api.hpp`); the *hash* harness is
       **HNUSystemsLab/Halo's `hash_api.h`**. (b) For CCEH/Dash we do NOT need a
       custom-PMDK build — **viper's own `benchmark/fixtures/` already has
       `dash_fixture.hpp` + `cceh_fixture.hpp` on stock PMDK** (CMake sed-patches
       Dash's `*_addr` → stock `pmemobj_create`). Resulting plan:
       - **PRIMARY = viper's own `ycsb_bm` / `all_ops_bm`** (K8/V200, the paper's
         real workload), running **HiOM + Viper + Dash + CCEH in one harness on
         stock PMDK**. Dash/CCEH were merely commented out of `ALL_BMS`; re-enabled
         + **verified end-to-end** (all_ops 1M/1t, K16/V200: Dash get 1.87 M/s,
         CCEH get 1.46 M/s, both `found=1M`). Only fix needed: **`-mavx2`** in
         `benchmark/CMakeLists.txt:194` (was `-mtune=native`, which doesn't enable
         the ISA Dash's `epoch_reclaimer` AVX2 intrinsics need). Dash/CCEH value
         pools cut **80→24 GiB** (shared /pmem0 safety). NB `CcehFixture` is the
         *DRAM*-CCEH variant (viper's `cceh.hpp`) → DRAM-index camp, like Viper.
       - **SECONDARY = Halo `hash_api.h` harness** (u64/u64), only for systems
         viper lacks fixtures for — **CLevel / SOFT / Halo** (and a *PM-resident*
         CCEH/Dash if a same-harness cross-check is wanted). HiOM wrapper landed
         there too (`#ifdef HIOMT`; 8.92 vs 6.74 Mops/s read, 10 k/1 t smoke);
         repro persisted to [benchmark/hash_api/](../benchmark/hash_api/).
       - **Custom PMDK = dropped** (unneeded on the primary path). **Pending:**
         real E2/E4 via `ycsb_bm` (K8/V200, sized + thread sweep, flush between
         phases for write fairness); CLevel/SOFT/Halo via the Halo harness (SOFT
         needs libvmem); E1/E3 stay on white-box `fixture_dram_bytes()` /
         `hiom_recovery_bm`. Positioning unchanged: three-axis (DRAM × read ×
         recovery) Pareto, MetoHash/EEPH+ as related work.

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

  Fixture-only index DRAM — **superseded 2026-06-05 by direct white-box
  measurement** (`fixture_dram_mb`, commit 88ff547; driver
  [measure_dram.sh](../benchmark/measure_dram.sh)). The earlier
  RSS-minus-estimated-harness table is dropped: it conflated the shared
  YCSB vectors with fixture state and over-counted both systems. The
  numbers below are each fixture's *own* index DRAM, computed from `sizeof`
  of the live structures (Viper: CCEH directory + unique segments; HiOM:
  HotTier + ColdTier-DRAM + residual CCEH), at t=1 with full prefill (DRAM
  is workload/thread-independent, so one run per size suffices):

  | size | Viper (MB) | HiOM (MB) | HiOM/Viper | savings |
  |------|-----------:|----------:|-----------:|--------:|
  | 5 M  | 2052.0     | 272.0     | 13.3%      | **−86.7%** |
  | 10 M | 2052.0     | 272.0     | 13.3%      | **−86.7%** |
  | 16 M | 2052.0     | 272.0     | 13.3%      | **−86.7%** |
  | 33 M | 2053.0     | 272.0     | 13.2%      | **−86.7%** |
  | 50 M | 2064.8     | 272.0\*   | 13.2%      | **−86.8%** |

  \*HiOM 50 M not measured (prefill livelocks past the HotTier cap — the M4
  limitation); its index DRAM is the same 272 MB constant since HotTier
  capacity (2²¹ buckets) is fixed regardless of dataset size. HiOM 33 M was
  genuinely prefilled (`hot_tier_size` = 29.9 M, near the 33.5 M cap), so
  its 272 MB is a real measurement, not an extrapolation.

  **Both lines are essentially flat across the paper's size range, and the
  saving is structural (−86.7 %), not a function of dataset size:**
  - **Viper** is pinned at ~2052 MB by CCEH's 131 072-segment (≈2 GiB)
    eager pre-allocation — flat through 16 M (load < 25 %, no splits), and
    only creeping up (2053 → 2065 MB) at 33–50 M as segments begin to
    split. Past the 134 M-slot capacity it would climb steeply.
  - **HiOM** is pinned at 272 MB by the fixed HotTier capacity (ColdTier
    lives in PMem ≈ 0 DRAM; the commit buffer is transient). It does not
    grow with the dataset.

  Net: HiOM uses **13 % of Viper's index DRAM (−86.7 %)** at every measured
  size — comfortably beating the ≥ 50 % win-condition target, and the gap
  only widens once the dataset grows past CCEH's pre-allocated capacity.

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

  Mechanism (**confirmed 2026-05-20 19:00 via gdb `thread apply all
  bt` on a reproduced hang**, evidence at
  `/tmp/hiom_evidence/a_zipf_33M_hang_20260520_1858/`):
  At the snapshot point (t=10 min in the cell) and at the SIGKILL
  point (t=25 min), the running ycsb_bm process had **identical**
  thread distribution:
  - **17 of 24** YCSB worker threads parked at
    `__clock_nanosleep → ::Client::mirror_write_with_offset →
     HiOMFixture::run_ycsb` — i.e. inside the producer back-pressure
    spin at [hiom.hpp:578-637](../include/viper/hiom/hiom.hpp#L578-L637)
    (50 µs sleep + retry).
  - 7 of 24 workers parked at `benchmark::State::StartKeepRunning`
    — they finished their iteration share and are waiting for
    laggards at the GBenchmark barrier. So *some* producers are
    making progress; the catastrophe is partial-not-total
    starvation of the slow ones, who never catch up before the
    timer expires.
  - 4 flusher threads in `pthread_cond_clockwait` on
    `wake_slots_[i].cv` (the timed wait at
    [hiom.hpp:1376-1381](../include/viper/hiom/hiom.hpp#L1376-L1381))
    — they wake on the 5 ms timer and on producer notifies, drain
    a batch, then go back to wait. They are *not* starved; they
    just can't keep up with 17 sustained producers spinning on
    bucket-PINNED retry.

  The static walkthrough below is therefore the actual runtime
  mechanism, not a hypothesis:
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
     never drains to empty for the stuck 17 → those producers
     never return.

  **Reproducibility note**: the isolated `--benchmark_filter=
  '...a_zipf_tp.*threads:24$'` cell (single-threaded prefill +
  3 reps × threads:24 only) finished cleanly at 4.71 M/s in
  ~30 s. The hang only reproduces with the original sweep filter
  `threads:(1|8|24)$`, which runs threads:1 → threads:8 → threads:24
  back-to-back in one ycsb_bm process (each fixture cell does a
  full DeInitMap + InitMap rebuild of `/pmem0/hiom_bench/`, so
  this isn't in-process state leakage). The most likely confounder
  is cumulative PMem state — three sequential ~33M prefills exhaust
  the Optane DCPMM write-buffer freshness, slow the flusher's
  apply_batch latency, and shift the producer/flusher balance past
  the back-pressure tipping point that isolated threads:24 stays
  under. Workloads that completed at 33M in the same sweep
  (a_uniform, b_uniform, b_zipf) avoid the trigger because either
  writes spread uniformly or the write rate is too low to outrun
  the flusher even when it's slowed.

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
  - M6.5 recovery wall-clock (`hiom_recovery_bm`, `recovery_threads=32`,
    N=100M). Main result is the **fair open-only** comparison
    (`--open-only`, reusing one prefilled PM state, 3 cold-start runs,
    cv < 3%):

    | metric | value |
    |--------|-------|
    | baseline cold open (`recover_database`, full CCEH rebuild) | **~2180 ms** |
    | HiOM fair open (`cceh_init_cap=1` + fixed 256 MB / 2 M-bucket HotTier) | **~87 ms** |
    | **speedup** | **~25× (cold-start)** |

    Conservative floor: against a *warm* baseline (1302 ms, measured
    back-to-back right after prefill with caches hot) the win is still
    ~15×. Cold-start is the honest restart-recovery number, so ~25× is
    the headline. HiOM fair-open phase split (total ~87 ms): viper_open
    ~0.8 ms (`skip_recovery`, CCEH cap=1) · cold_open ~0.1 ms (lazy mmap
    of the 16 G `cold.bin` — not a cost) · hiom_ctor ~84 ms (HotTier
    alloc ~28 ms + tail replay + counter priming). The tail replay is
    the sole O(tail) term; everything else is data-size-independent
    fixed setup, while the baseline's ~2.2 s scales O(N).

    *Artifact note (why an earlier number read break-even):* the naïve
    end-to-end `--full` run showed only break-even performance (**1.0×
    at 100M**) because it included oversized CCEH (≈2 GB init, ~277 ms)
    and HotTier (16 M buckets, ~1030 ms — sized 2×N to suppress prefill
    eviction) initialization costs. The phase breakdown shows these
    costs are benchmark artifacts rather than recovery work — a
    standalone `[ref]` alloc confirms 16 M buckets ~1030 ms vs 2 M
    buckets ~28 ms. 1M/10M stay ≤1× for the same reason (fixed setup
    dwarfs the tiny tail at small N).
  - **M6.5 tail-size sensitivity** (`--tail-sweep`, non-destructive reuse of
    the same prefilled 100 M PM state; 3-rep median, `cceh_cap=1` + 2 M-bucket
    HotTier). Sweeps worst-case tail ∈ {0, 1 K, 4 K, 16 K, 64 K, 256 K, 1 M}
    entries at `recovery_threads` ∈ {1, 32}, anchoring each tail at the real
    data top (`probe_data_top` walks back past the ~50 empty trailing blocks
    above the write frontier). Confirms the **O(tail)** mechanism:

    | aspect | result |
    |--------|--------|
    | tail-scan replay (t=1) | **linear, 49.8 µs/entry** (7 points, 0–1 M) |
    | fixed floor (t=1 intercept) | ~500 ms cold-tier first-touch — new open + `cold.bin` mmap page-fault, *not* O(N) |
    | parallel speedup t1/t32 | ~1× at small tails (≤1 block/thread, PMem latency exposed) → **19× at 1 M** (bandwidth saturated) |
    | crossover vs O(N) rebuild (t=32 total) | ≈**736 K** replayed entries (~485 blocks) |
    | default cadence=4096 worst case (t=32) | **750 ms vs 2.14 s baseline = 2.9×**; ~180× margin to crossover |

    So even the worst-case unflushed tail at the default cadence recovers
    several× faster than Viper's full rebuild; one would need a tail ~180×
    larger than the cadence bound before tail-scan stops paying off. The
    headline ~25× (above) is the *best-case* tail≈0 open, this 2.9× is the
    *cadence-bounded worst case* — both wins, the gap being the ~500 ms
    cold-tier first-touch floor that dominates small-tail recovery.

    *Honest caveats.* (1) Pure-timing experiment: the swept tail's
    `cold_->upsert` re-inserts keys already present (prefill wrote all N into
    ColdTier), so each hits the idempotent "fingerprint exists" branch
    (1 persist) rather than a first-insert (CAS + 2 persists) — the
    49.8 µs/entry slope is thus mildly *optimistic* vs a true cold rebuild,
    though the **linear trend is unaffected**. (2) The per-point 100-key
    sanity read is consequently always 100/100 (ColdTier is full): a
    **liveness check, not a correctness proof** — recovery correctness is
    established by the M4 crash-injection harness (18/18). Artifacts:
    `results/recovery/{tail_sweep.csv,tail_sweep_meta.json,summary.txt}`,
    `eval/charts/recovery_{tail_scan,vs_baseline}.png`.
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

Framed for the narrowed scope above — DRAM-efficiency first, recovery
second; throughput is a *cost-bound*, not a contribution:

- **C1 — DRAM-efficient offset map**: HiOM caches *index entries (offsets)*,
  not key/value data, so a fixed DRAM budget bounds the index footprint
  regardless of dataset size. White-box index DRAM is **272 MB (HiOM) vs
  ~2052 MB (Viper CCEH) = −86.7%**, flat across 5–50 M (§Phase 2), while
  Viper's CCEH eagerly preallocates ≈2 GB on init.
- **C2 — Working-set-aware hot/cold index tiering**: a compact 8 B DRAM
  HotTier entry (4 B fingerprint + 4 B offset, with a ≤8 B vs. >8 B key
  case split, see §2.2) under SIEVE eviction, backed by an authoritative
  PMem ColdTier — the first application of working-set-aware index tiering
  to persistent hash indices in hybrid PM-DRAM KV stores. The capacity
  ablation (§Status 06-05, YCSB-C 10 M) quantifies the value of skew at
  tight DRAM budgets: a HotTier sized for **10% of the working set hits 62%
  on zipfian reads vs 19% uniform**, both rising to ~1.0 once the budget
  reaches the working set.
- **C3 — Crash-consistent group commit + bounded recovery**: pin invariants,
  A/B checkpoints, and a tail-scan recovery. HiOM turns index recovery from
  full VPage reconstruction, O(N), into checkpoint-bounded tail replay,
  O(tail). Measured at 100 M (open-only, deployment-fair config):
  **~25× faster cold-start open** (~87 ms vs ~2.2 s), ~15× against a warm
  baseline as a conservative floor. The previous 40× target is not reached,
  but the order-of-magnitude recovery win holds. The O(tail) claim is now
  directly measured (tail-size sweep, §M6.5): replay is linear at
  49.8 µs/entry (single-thread), and even the default cadence=4096
  worst-case tail opens ~2.9× faster than the O(N) rebuild — crossover only
  at ≈736 K replayed entries, ~180× the worst-case tail.

Supporting techniques: per-thread commit buffer using existing
`concurrentqueue`; 32-region linear hashing for parallel cold-tier load;
AVX-512 SIMD fingerprint compare.

## Win condition

**(revised 2026-06-05 — narrowed to a scoped claim, not a universal
throughput win.)** HiOM targets **DRAM-constrained, read-heavy,
recovery-sensitive** deployments:

- **DRAM (primary win)**: for working sets that exceed a fixed DRAM budget,
  HiOM keeps index DRAM bounded while Viper's CCEH grows linearly and OOMs
  past DRAM capacity (CCEH preallocates ≈2 GB on init). Target: ≥50%
  fixture-DRAM reduction. *White-box measured (§Phase 2): HiOM 272 MB vs
  Viper ~2052 MB = **−86.7%**, flat across 5–50 M.*
- **Read-heavy throughput (now a genuine win, not just acceptable-cost)**: on
  read-mostly workloads (YCSB-C analog, full HotTier) HiOM **matches or exceeds
  Viper at every thread count and beats both PM-resident baselines** —
  **re-measured 2026-06-07 @10 M, dense t=1..24**: 100r zipf t24 **46.8 vs Viper
  43.2 / Dash 31.3 / CCEH 23.8**; uniform t24 35.5 vs 35.9 (within noise). The
  earlier "0.69–0.92×, acceptable-cost not a win" (t=8, 2026-06-05) was a
  **measurement artifact** — a single contended stats `fetch_add` on the read
  hot path capped HiOM's scaling; per-Client shards removed it (Status top +
  Claude memory `hiom-read-stats-contention`). HiOM even edges Viper on zipf
  because its verify reads key+value in one `hiom_read_at_offset` vs Viper's
  separate key-check + value read. Net: **Viper-class (or better) reads at a
  fraction of the DRAM, and a clean win over PM-resident Dash/CCEH** — the "读多"
  pillar now stands on throughput, not just DRAM.
- **Recovery (secondary win)**: O(unflushed tail) vs Viper's O(all data)
  full CCEH rebuild — **~25× faster cold-start open at 100 M** (~87 ms vs
  ~2.2 s; ~15× conservative against a warm baseline).

**Explicitly out of scope / known limitations** (demoted from the previous
win claim, presented as limitations in the paper — not wins):

- **Write-heavy throughput is lower (a documented cost, not parity)**: YCSB-A
  (50 % update) runs **0.46× (zipf t24) – 0.74× (uniform t24) of Viper**
  (re-measured 2026-06-07 @10 M; up from the old 0.24–0.45× after the update
  in-place skip-commit + rising-edge wake fix) — HiOM maintains a secondary
  durability index (ColdTier mirror), so update/delete-heavy workloads pay for
  it. The paper does not claim write parity. **Read-mostly (YCSB-B, 5 % update)
  is NO LONGER a limitation** — there HiOM ≈ Viper (0.92–1.05×) and ~2.5×
  Dash/CCEH; the cost is confined to write-dominated mixes.
- **Skewed writes at HotTier capacity can livelock**: at working set ≥
  HotTier capacity *with* a skewed write workload (YCSB-A zipfian, e.g.
  `a_zipf-33M`), the bucket-PINNED back-pressure can starve slow writers
  (root cause confirmed via gdb thread backtraces, 2026-05-20). This is
  outside the current scoped win condition, but not outside the system's
  long-term target: the evaluation matrix labels the cell and discusses it
  as a limitation, with a per-region SIEVE clock-pacing fix as future work.
- **Small HotTier + high read concurrency can livelock**: when the read
  working set far exceeds HotTier capacity, concurrent `mirror_into_hot`
  inserts contend on SIEVE eviction and spin on each other's PINNED slots —
  the read-side analogue of `a_zipf-33M` (discovered 2026-06-05 during the
  capacity ablation). Reproduced at t=8 for budgets ≤40% of the working set
  (log2 ≤ 18); t=1 is unaffected (single-threaded eviction always finds an
  unpinned victim), so the capacity ablation measures hit rate at t=1 —
  thread-independent, hence no loss of generality. The same per-region SIEVE
  clock-pacing fix applies as future work.

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
| Concurrent read mirror into full HotTier (t>1) | SIEVE evict spins on peers' PINNED victims (read-side a_zipf-33M) | Measure hit-rate at t=1; clock-pacing fix future work |
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
- **§7 Evaluation Plan**: ✅ drafted below (baseline strategy + win-condition
  experiments). §3–§6 design detail is still pending; the evaluation plan is
  finalized early because the competitive landscape is now clear.

---

# §7 Evaluation Plan

*(Baseline strategy finalized 2026-06-06 from a literature sweep on
open-source, Optane-runnable PM hash indexes. §3–§6 design detail still TODO.)*

## 7.1 Platform & harness

- Real Intel Optane DCPMM, FS-DAX (`/pmem0`) + PMDK, **fixed-size K/V only**
  (Viper's variable-size recovery is unimplemented, §1.4). Baseline repos cloned
  to `/root/hiom-baselines/` (outside the viper git tree); our wrapper lives in
  the viper repo.
- **Harness — use the HNUSystemsLab/Halo `hash_api.h` benchmark, NOT
  `sfu-dis/pibench` (verified 2026-06-06 by cloning + reading source).**
  `sfu-dis/pibench` is the *range-index* benchmark (`tree_api.hpp`) — wrong tool
  for a hash index. The hash equivalent is the **Halo repo's `hash_api.h` +
  `benchmark.cpp`** driver, and it is turnkey: `third/` already bundles **CCEH,
  Dash, CLevel, SOFT, CLHT, PCLHT — and Viper** (`#ifdef VIPERT`), each picked by
  a `-DXXXT` define. So we adopt this one harness instead of porting six repos,
  getting the whole §7.2 set (minus Plush) behind a single driver + shared
  workload generator.
- **HiOM wrapper = one `#ifdef HIOMT` block** in `hash_api.h`, cloned from the
  existing `VIPERT` block (HiOM wraps Viper + owns the index, same `Client`
  API); add a Makefile target; repoint the hardcoded `/mnt/pmem/hash/` pool to
  `/pmem0/...`. The harness covers **E2 (read tput/scale) + E4 (write/scale)**;
  **E1 (DRAM) stays on our white-box `fixture_dram_bytes()`, E3 (recovery) on
  `hiom_recovery_bm`** — both already built; neither is what `hash_api.h`
  measures.
- **Build tiers (verified on this host 2026-06-06):** toolchain present —
  **g++-11 + clang-14** (default `gcc` is 7.5; use g++-11). The HALO target
  **compiles clean to object under g++-11** (smoke passed). Stock
  `libpmem`/`libpmemobj` suffice for **HALO / SOFT / CLEVEL / VIPER / HIOM**;
  **only CCEH + Dash need the customized PMDK fork** (XiangpengHao/pmdk,
  `MAP_FIXED_NOREPLACE`) + VeryPM epoch mgr — the one finicky build. All targets
  link `-lPCM` (`make -C pcm`; needs `modprobe msr` at runtime). **Plush** is
  separate (tum-db/Plush, own bindings) — optional, add last.
- ✅ **Optane generation confirmed: ADR-only** (`ndctl`: `persistence_domain =
  memory_controller`, no eADR; DIMM FW 02.02). MetoHash / Spash / SEPH
  (eADR-gated) are **unbuildable here → cite-only**, exactly as §7.5 assumes;
  the runnable set (CCEH/Dash/CLevel/SOFT/Halo/Viper/Plush) is all ADR-safe.

## 7.2 Baselines — and why these

Standard, **open-source, reproducible** PM-index set (the reviewer checklist;
all have public repos, most have PiBench bindings):

| system | venue | index | DRAM | camp |
|--------|-------|-------|------|------|
| **CCEH** | FAST'19 | DRAM *or* PM (configurable) | high / ~0 | both |
| **Dash** | VLDB'20 | PM | ~0 | PM-resident |
| **Viper** | VLDB'21 | DRAM (CCEH) | ~2 GB | DRAM-index (primary) |
| **Halo** | SIGMOD'22 | DRAM | high | DRAM-index, fast recovery |
| **Plush** | VLDB'22 | mostly PM | tiny | PM-resident, write-opt |

**Why not "newer / lower-tier" baselines** (deliberate, not an oversight): a
2026-06 sweep found recent (2024–26) PM work either (a) targets CXL /
memory-semantic SSD / RDMA-disaggregated memory and **cannot run on Optane
FS-DAX** (BonsaiKV+, TieredHM, NStore, CXL-KVS, Outback…), or (b) is a
same-platform competitor that is **not open-sourced** (MetoHash SC'25, EEPH+
TACO'25). Mid-tier hybrid-PM hashing papers (Eukv IEEE-Access'23, HASDH
ICCD'21, PMEH'23) have **no available source**. Open, reproducible code
clusters in top venues (artifact-evaluation culture), so chasing an obscure
weak baseline is a *red flag*, not an advantage.

**Run vs cite (thesis scope, 2026-06-08).** *Run* set = **Viper + Dash + CCEH
(+ HiOM)** (primary `ycsb_bm` / `all_ops_bm`, E1–E4 + latency @10 M) — sufficient
and balanced; each Pareto axis already has a clear baseline HiOM beats
(Viper/CCEH = DRAM-index ⇒ DRAM; Dash = strongest PM-resident hash, VLDB'20 ⇒
read tput/latency; Viper = O(N) ⇒ recovery). **Halo** is the one optional add
(modern DRAM-index + fast-recovery — contests HiOM's DRAM *and* recovery axes
head-on), run only if a time-boxed build succeeds; cite-acceptable otherwise. A
second PM-resident *read* point, if ever wanted (the camp claim currently rests
on Dash alone, since `CcehFixture` is DRAM-mode), is cheapest as **CCEH in
PM-mode** (same code, configurable), not a new system.

## 7.3 Positioning: a three-axis Pareto point

HiOM is **not** a uniform winner; it is Pareto-non-dominated on
{index-DRAM, read throughput, recovery}, with write/scalability as an
acknowledged cost. Each axis has a baseline HiOM beats:

- **Read throughput** → beats PM-resident designs (Dash, Plush, PM-mode CCEH):
  they pay PM random-read latency per lookup; HiOM's hot offset
  lives in DRAM.
- **Index DRAM** → beats the DRAM-index camp (Viper, Halo, DRAM-mode CCEH:
  ~2 GB vs HiOM's flat 272 MB).
- **Recovery** → beats Viper (O(N) rebuild). Halo is also sub-linear — state
  honestly that HiOM matches fast recovery *without* paying for a full DRAM
  index.

## 7.4 Win-condition experiments (→ C1 / C2 / C3)

| exp | claim | metric | expected result |
|-----|-------|--------|-----------------|
| **E1** DRAM vs N | C1 | index DRAM (MB) vs dataset size | HiOM flat 272 MB; Viper / Halo / DRAM-CCEH grow |
| **E2** iso-DRAM reads | C2 | read tput / hit-rate vs **DRAM budget** | HiOM usable under tight budget; DRAM-index camp OOMs, PM camp slower |
| **E3** recovery | C3 | open time vs N / tail size | O(tail) vs O(N); already 25× vs Viper, ≈736 K crossover |
| **E4** write / scale | limitation | YCSB-A/B tput vs threads | YCSB-B (read-mostly) ≈ Viper (0.92–1.05×); YCSB-A (write-heavy) 0.46–0.74×; a documented cost, not hidden |

**Measured (2026-06-07, 10 M, four-system K8/V200, dense t=1..24)** — full E2/E4
tables in Status (2026-06-07) above. Headlines: **E2 reads** — HiOM
matches/exceeds Viper and beats Dash > CCEH at **every** thread count (zipf t24
46.8 vs Viper 43.2); the earlier "Dash overtakes at t=24 → scope the read win to
low/mid concurrency" was the single contended stats `fetch_add`, fixed via
per-Client shards and now **RETRACTED**. **E4 writes** — **YCSB-B (read-mostly)
≈ Viper (0.92–1.05×, even edges it at zipf t8) and ~2.5× Dash/CCEH**; **YCSB-A
(write-heavy) 0.46× (zipf t24) – 0.74× (uniform t24) Viper** — up from the old
0.27–0.34× after the update in-place skip-commit + rising-edge wake fix — still
~5× Dash/CCEH but with a fixture caveat (the Dash/CCEH value store is per-op
transaction-bound, not pure-index — do not claim a clean write win over Dash).
E1/E3 unchanged (white-box DRAM / `hiom_recovery_bm`).
Scoped (2026-06-08, thesis): baseline set frozen to **Viper + Dash + CCEH
(+ HiOM)**; **Halo optional** (the one DRAM+recovery opponent worth a time-boxed
build, cite-acceptable otherwise). (Four-system `lat` — DONE
2026-06-08, see Status (2026-06-08): read latency ≈ Viper / ≪ Dash-CCEH,
write t=24 tail inflation mirrors YCSB-A.) Dash/CCEH
vs-N scaling deliberately scoped out (vs-N value is on the DRAM axis, covered by
E1; throughput is near-flat in N — a single 10 M point suffices as the
PM-resident read control).

## 7.5 Related work to cite (not run)

MetoHash (SC'25), EEPH+ (TACO'25), SEPH (OSDI'23), BonsaiKV+ (TC'24,
CXL/tiered), ERT (SIGMOD'23). **Differentiation**: HiOM uniquely combines
SIEVE **working-set-aware** hot/cold tiering, caches **offsets only** (not
records), and uses **O(tail) checkpoint recovery** — none of the above pairs
all three on Optane FS-DAX.

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
Required for the paper's order-of-magnitude faster-recovery claim
(measured ~25× at 100M open-only; see §M6.5 recovery wall-clock).

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
- [x] Run benchmark with `--full` (100M) + `--open-only` fair
      comparison (2026-06-05). The O(N)-vs-O(tail) gap is real but
      only visible under a deployment-fair config: naïve `--full` at
      100M reads 1.0× (artifact — oversized CCEH ~277 ms + 16 M-bucket
      HotTier ~1030 ms swamp the tail replay). `--open-only` with
      `cceh_init_cap=1` + a fixed 256 MB HotTier gives **~25×**
      (baseline ~2180 ms vs HiOM ~87 ms). Added `--open-only` mode
      (reuses prefilled PM state, per-phase timing). See §M6.5
      recovery wall-clock.

Exit criteria met: write-path retirement landed, all
correctness tests pass (including slot-accounting on Phase D).
The 100M datapoint (2026-06-05) turned out *not* to be a mere
formality — the naïve end-to-end run read 1.0× and a phase
breakdown was needed to separate the tail replay from oversized
CCEH/HotTier setup; fair-config open-only is ~25×. See §M6.5
recovery wall-clock.

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
