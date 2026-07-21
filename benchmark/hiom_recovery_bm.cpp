// HiOM recovery benchmark (M6.5).
//
// Measures the open-time win from skipping Viper::recover_database()
// when HiOM is authoritative for the index. The same prefill workload
// is used for both modes (HiOM-mediated, so both ColdTier and CCEH
// are populated at close time); the asymmetry is purely in the
// open() path.
//
// Modes (per dataset size):
//   - Baseline: Viper::open() with default config -> recover_database()
//     walks every VPage, rebuilds CCEH. Time = O(all_records).
//   - HiOM:     Viper::open(skip_recovery=true) skips that rebuild;
//     HiOM constructor does a tail scan from
//     [checkpoint.vpage_frontier - 1, current_block) and upserts into
//     ColdTier idempotently (M6 mechanism). Time = O(tail).
//
// The benchmark prints a table:
//   N      baseline_ms   hiom_ms   speedup
//
// Sanity check: after the HiOM open, do 100 random key lookups via
// HiOM's Client::get; all must hit. If any miss, the speedup number
// is meaningless and we abort.
//
// Why not Google Benchmark: this is a one-shot recovery measurement
// per dataset size, not a microbench. Stopwatch + printf is enough
// for the M6.5 deliverable; promote to GBench when wired into M7.
//
// Why not ViperFixture: that fixture pre-supposes a Viper-only DB,
// no ColdTier or Checkpoint. We need HiOM-mediated prefill so both
// the baseline and HiOM modes start from a state where ColdTier is
// fully populated.

#include "viper/viper.hpp"
#include "viper/hiom/checkpoint.hpp"
#include "viper/hiom/cold_tier.hpp"
#include "viper/hiom/hiom.hpp"
#include "viper/hiom/hot_tier.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr const char* kRootDir = "/pmem0/hiom/recovery_bench";
constexpr const char* kPoolDir = "/pmem0/hiom/recovery_bench/pool";
constexpr const char* kColdPoolFile
    = "/pmem0/hiom/recovery_bench/cold/cold.bin";
constexpr const char* kColdPoolDir = "/pmem0/hiom/recovery_bench/cold";
constexpr const char* kCheckpointFile
    = "/pmem0/hiom/recovery_bench/chkpt/chkpt.bin";
constexpr const char* kCheckpointDir = "/pmem0/hiom/recovery_bench/chkpt";

// Generous pool sizing so 100M entries (1.6 GiB raw + bookkeeping)
// fit. 8 GiB covers up to ~400M (uint64_t, uint64_t) pairs.
constexpr std::size_t kPoolSize = 8ULL << 30;

// Per-N pool sizing keeps the mmap cost proportional to the working
// set so the recovery measurement isn't dominated by 8 GiB of cold
// page-table setup. Pool size must be a multiple of 1 GiB
// (ViperConfig::dax_alignment default).
std::size_t pool_size_for(std::size_t N) {
    constexpr std::size_t kGiB = 1ULL << 30;
    // ~16 B raw per entry, plus bookkeeping. 3× margin keeps headroom.
    const std::size_t needed = N * 16 * 3;
    // Round up to GiB.
    const std::size_t gibs = (needed + kGiB - 1) / kGiB;
    return std::max<std::size_t>(gibs, 1) * kGiB;
}

// Returns floor of log2 such that (1ULL << r) >= n.
std::size_t pow2_ceil_log2(std::size_t n) {
    std::size_t r = 0;
    std::size_t v = 1;
    while (v < n) { v <<= 1; ++r; }
    return r;
}

// Size HotTier so capacity (≈ 16 slots per bucket) is at least 2× N.
// This keeps SIEVE eviction quiet during the prefill so per-put cost
// stays at the M0 baseline (~50 ns) instead of paying inline-flush
// back-pressure when buckets fill with PINNED slots. The recovery
// benchmark cares about correctness of the prefill (so all N reach
// ColdTier) and wall-clock prefill time (cosmetic) but not its
// per-op cost. Over-sized hot is fine: ~24 B per bucket × 16M
// buckets = ~384 MB DRAM at N=100M. Acceptable.
std::size_t hot_buckets_for(std::size_t N) {
    // capacity = buckets * 16; want capacity ≥ 2*N -> buckets ≥ N/8
    const std::size_t target = std::max<std::size_t>(N / 8, 1ULL << 10);
    return 1ULL << pow2_ceil_log2(target);
}

using ViperT = viper::Viper<std::uint64_t, std::uint64_t>;
using HiOMT  = viper::hiom::HiOM<std::uint64_t, std::uint64_t>;

void cleanup_dir(const char* dir, const char* expected_prefix) {
    if (std::string(dir).find(expected_prefix) != 0) {
        std::cerr << "Refusing to clean unfamiliar path: " << dir
                  << std::endl;
        std::exit(2);
    }
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
}

void cleanup_all() {
    // Pool dir is created by Viper::create itself; just remove it.
    if (std::string(kPoolDir).find("/pmem0/hiom/recovery_bench/") != 0) {
        std::cerr << "Refusing to clean unfamiliar pool: " << kPoolDir
                  << std::endl;
        std::exit(2);
    }
    std::filesystem::remove_all(kPoolDir);
    std::filesystem::create_directories(kRootDir);
    cleanup_dir(kColdPoolDir, "/pmem0/hiom/recovery_bench/cold");
    cleanup_dir(kCheckpointDir, "/pmem0/hiom/recovery_bench/chkpt");
}

// Prefill N entries via raw Viper + direct ColdTier upsert. We
// deliberately bypass the HiOM commit-buffer / flusher / HotTier
// machinery — those are designed for steady-state throughput at
// concurrent client scale; here we just need a correct, fast prefill
// that populates all three persistent layers (Viper VPages,
// Viper's CCEH, ColdTier) and then writes a sealed Checkpoint
// record. The choice of prefill path doesn't bias the recovery
// measurement: the open() time we measure is purely a function of
// what's on PM, not how it got there.
//
// At close, the PM state is:
//   - Viper VPages: all N (key, value) records.
//   - Viper CCEH: all N (key, offset) entries (used by the baseline
//     open() — it rebuilds CCEH on every open).
//   - ColdTier: all N (fp64, offset) entries (used by HiOM open()).
//   - Checkpoint: a sealed record with vpage_frontier = the post-
//     prefill cursor; M6 tail scan covers [frontier-1, current).
void prefill(std::size_t N, std::uint64_t seed) {
    auto viper = ViperT::create(kPoolDir, pool_size_for(N));
    // Size the ColdTier to N via the shared helper (targets ~0.5 load,
    // rounds main buckets to a power of two — bucket_id_of masks with
    // main_buckets-1, so non-pow2 sizing would leave most of the table
    // unaddressable and blow up chain length).
    const auto sz = viper::hiom::ColdTier::sizing_for(N);
    auto cold  = viper::hiom::ColdTier::create(
        kColdPoolFile, sz.main_buckets, sz.overflow_slots);
    auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);

    auto vclient = viper->get_client();

    std::mt19937_64 rng(seed);
    const auto pf_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < N; ++i) {
        const std::uint64_t key = i + 1;
        vclient.put(key, rng());
        const auto off = vclient.hiom_peek_offset(key);
        if (!cold->upsert(viper::hiom::key_fingerprint64(key),
                          viper::hiom::key_id_of(key), off)) {
            std::fprintf(stderr,
                "\n[HiOM FATAL] ColdTier overflow at key %zu/%zu during "
                "prefill — index sizing too small\n", i, N);
            std::abort();
        }
        if ((i & ((1 << 20) - 1)) == 0 && i > 0) {
            std::printf("    prefilled %zu / %zu\r", i, N);
            std::fflush(stdout);
        }
    }
    const auto pf_end = std::chrono::steady_clock::now();
    const double pf_ms = std::chrono::duration<double, std::milli>(
                            pf_end - pf_start).count();
    const std::size_t cold_size = cold->approx_size();
    std::printf("    prefill: %.1f ms (%.1f M ops/s)  cold=%zu/%zu\n",
                pf_ms, N / (pf_ms / 1000.0) / 1e6,
                cold_size, N);
    if (cold_size != N) {
        std::cerr << "  FAIL: ColdTier holds " << cold_size
                  << " entries; expected " << N << std::endl;
        std::exit(1);
    }

    // Seal a final checkpoint so HiOM open's tail scan has a
    // recent frontier to work from. Without this, read_valid()
    // would return nullopt and the tail scan would walk the whole
    // pool — making the M6.5 measurement degenerate.
    viper::hiom::CheckpointRecord rec{};
    rec.seq = 1;
    rec.flushed_count = N;
    rec.vpage_frontier = viper->hiom_vpage_frontier();
    rec.cold_size = N;
    chkpt->write(rec);
}

// Time Viper::open() with default ViperConfig — full CCEH rebuild
// via recover_database(). Returns ms.
double time_baseline_open(std::uint8_t recovery_threads) {
    viper::ViperConfig cfg;
    cfg.num_recovery_threads = recovery_threads;
    // skip_recovery defaults false.
    const auto t0 = std::chrono::steady_clock::now();
    auto viper = ViperT::open(kPoolDir, cfg);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// Time HiOM open path: Viper::open(skip_recovery=true) + ColdTier
// + Checkpoint + HiOM ctor with tail_scan=true. Returns ms.
// Also runs a sanity read pass to confirm ColdTier is authoritative.
double time_hiom_open(std::size_t N, std::uint8_t recovery_threads) {
    const auto t0 = std::chrono::steady_clock::now();
    viper::ViperConfig vcfg;
    vcfg.skip_recovery = true;
    auto viper = ViperT::open(kPoolDir, vcfg);
    auto cold  = viper::hiom::ColdTier::open(kColdPoolFile);
    auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
    HiOMT::RecoveryConfig rcfg;
    rcfg.tail_scan = true;
    rcfg.recovery_threads = recovery_threads;
    HiOMT hiom(*viper, hot_buckets_for(N), cold.get(),
               HiOMT::FlusherConfig{}, chkpt.get(),
               HiOMT::CheckpointConfig{}, rcfg);
    const auto t1 = std::chrono::steady_clock::now();
    const double open_ms
        = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Sanity pass: 100 random key reads must all hit. If they don't,
    // the speedup measurement is meaningless (we'd be measuring open
    // time of a corrupt index).
    auto client = hiom.get_client();
    std::mt19937_64 rng(0xfeed);
    std::size_t hits = 0;
    constexpr std::size_t kSanity = 100;
    for (std::size_t i = 0; i < kSanity; ++i) {
        const std::uint64_t k
            = (rng() % N) + 1;  // keys are i+1 for i in [0, N)
        std::uint64_t got = 0;
        if (client.get(k, &got)) ++hits;
    }
    if (hits != kSanity) {
        std::cerr << "  FAIL: HiOM open sanity reads " << hits
                  << " / " << kSanity << std::endl;
        std::exit(1);
    }
    std::printf("    sanity reads %zu/%zu\n", hits, kSanity);
    return open_ms;
}

void run_one(std::size_t N, std::uint8_t threads) {
    std::printf("\n=== N=%zu  recovery_threads=%u ===\n",
                N, static_cast<unsigned>(threads));
    cleanup_all();
    prefill(N, /*seed=*/0xc4ec0700);

    const double baseline_ms = time_baseline_open(threads);
    std::printf("    baseline_ms = %.1f\n", baseline_ms);

    const double hiom_ms = time_hiom_open(N, threads);
    std::printf("    hiom_ms     = %.1f\n", hiom_ms);

    const double speedup = baseline_ms / hiom_ms;
    std::printf("    speedup     = %.1fx\n", speedup);
}

// ---- M6.5 open-only diagnostic (reuses existing PM state) -------------
// Does NOT cleanup or prefill. Measures the three open variants against
// whatever state a prior --full run left on PM (assumed N=100M), with
// per-phase timing, to isolate which costs (2 GB CCEH alloc / ColdTier
// mmap / over-sized HotTier alloc) consume the tail-scan savings.
// Repeated opens are read-only w.r.t. PM data (the only mutator,
// cleanup_all, is never called here), so the 50-min prefill state is safe.

using clk = std::chrono::steady_clock;
double ms_between(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

double g_baseline_ms = 0.0;

// One HiOM open variant with phase breakdown. Returns total ms.
double measure_hiom_open(const char* tag, std::size_t N,
                         std::uint8_t threads, std::size_t cceh_cap,
                         std::size_t hot_buckets) {
    const auto t0 = clk::now();
    viper::ViperConfig vcfg;
    vcfg.skip_recovery = true;
    vcfg.cceh_init_cap = cceh_cap;
    auto viper = ViperT::open(kPoolDir, vcfg);
    const auto t1 = clk::now();
    auto cold = viper::hiom::ColdTier::open(kColdPoolFile);
    const auto t2 = clk::now();
    auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
    const auto t3 = clk::now();
    HiOMT::RecoveryConfig rcfg;
    rcfg.tail_scan = true;
    rcfg.recovery_threads = threads;
    HiOMT hiom(*viper, hot_buckets, cold.get(),
               HiOMT::FlusherConfig{}, chkpt.get(),
               HiOMT::CheckpointConfig{}, rcfg);
    const auto t4 = clk::now();
    const double total = ms_between(t0, t4);

    std::printf("[%s] cceh_init_cap=%zu  hot_buckets=%zu\n",
                tag, cceh_cap, hot_buckets);
    std::printf("    viper_open = %8.1f ms  (CCEH alloc + skip_recovery)\n",
                ms_between(t0, t1));
    std::printf("    cold_open  = %8.1f ms  (mmap cold.bin)\n",
                ms_between(t1, t2));
    std::printf("    chkpt_open = %8.1f ms\n", ms_between(t2, t3));
    std::printf("    hiom_ctor  = %8.1f ms  (HotTier alloc + tail scan)\n",
                ms_between(t3, t4));
    std::printf("    total      = %8.1f ms\n", total);

    // Sanity: 100 random keys must all hit via HiOM (ColdTier-backed).
    auto client = hiom.get_client();
    std::mt19937_64 rng(0xfeed);
    std::size_t hits = 0;
    for (std::size_t i = 0; i < 100; ++i) {
        const std::uint64_t k = (rng() % N) + 1;
        std::uint64_t got = 0;
        if (client.get(k, &got)) ++hits;
    }
    std::printf("    sanity     = %zu/100\n", hits);
    if (g_baseline_ms > 0.0) {
        std::printf("    speedup vs baseline = %.2fx\n",
                    g_baseline_ms / total);
    }
    if (hits != 100) {
        std::cerr << "  WARN: sanity < 100 for [" << tag
                  << "]; its speedup is meaningless\n";
    }
    return total;
}

void run_open_only(std::uint8_t threads) {
    // PM state from a prior --full run is assumed to hold N=100M.
    constexpr std::size_t N = 100'000'000;
    constexpr std::size_t kFairHotBuckets = 1ULL << 21;  // 2M buckets ~256 MB

    std::printf("\n=== OPEN-ONLY DIAGNOSTIC (reusing existing PM state) ===\n");
    std::printf("Assuming N=%zu. NO cleanup/prefill performed.\n", N);
    std::printf("Pool=%s\n\n", kPoolDir);

    // Baseline: full recover_database.
    {
        viper::ViperConfig cfg;
        cfg.num_recovery_threads = threads;
        const auto t0 = clk::now();
        auto viper = ViperT::open(kPoolDir, cfg);
        const auto t1 = clk::now();
        g_baseline_ms = ms_between(t0, t1);
        std::printf("[baseline] recover_database (default config)\n");
        std::printf("    total      = %8.1f ms\n\n", g_baseline_ms);
    }

    // HiOM as the --full benchmark runs it today.
    measure_hiom_open("hiom-current", N, threads,
                      /*cceh_cap=*/131072, hot_buckets_for(N));
    std::printf("\n");
    // HiOM as a real deployment would configure it.
    measure_hiom_open("hiom-fair", N, threads,
                      /*cceh_cap=*/1, kFairHotBuckets);
    std::printf("\n");

    // Reference: standalone empty-HotTier alloc cost (alloc + init only).
    {
        const auto a0 = clk::now();
        { viper::hiom::HotTier ht(hot_buckets_for(N)); }
        const auto a1 = clk::now();
        { viper::hiom::HotTier ht(kFairHotBuckets); }
        const auto a2 = clk::now();
        std::printf("[ref] standalone HotTier alloc:\n");
        std::printf("    %zu buckets (current) = %8.1f ms\n",
                    hot_buckets_for(N), ms_between(a0, a1));
        std::printf("    %zu buckets (fair)    = %8.1f ms\n",
                    kFairHotBuckets, ms_between(a1, a2));
    }
}

// ---- C3 recovery-sensitivity sweep: recovery time vs tail size --------
// Verifies the O(tail) tail-scan mechanism and maps checkpoint cadence onto
// the curve. Fresh prefill (default N=10M) writes ALL N into ColdTier (so it
// is full regardless of tail), then for each target tail size we rewrite the
// checkpoint frontier and re-open + tail-scan.
//
// Two measurement conventions (P1, see design plan):
//   - threads=1  : wall-clock ∝ scan blocks -> pure O(tail) line through the
//                  origin (the headline mechanism plot, figure 1).
//   - threads=32 : deployment config; total open time for the cadence /
//                  crossover comparison, plus the t1/t32 parallel speedup.
//
// NOTE (P2, honesty): the tail-scan upserts hit ColdTier's idempotent
// "fp already present" branch (offset store + 1 persist), cheaper than a real
// first-insert (CAS + 2 persists), so the slope is mildly optimistic — the
// linear TREND is unaffected. The 100-key sanity is a liveness check
// (ColdTier is full from prefill), NOT a recovery-correctness proof; that is
// covered by the M4 crash-injection suite (18/18).

constexpr const char* kResultsDir = "/root/viper/results/recovery";

using VPageU = viper::internal::ViperPage<std::uint64_t, std::uint64_t>;
// Entries per 24 KiB block for <u64,u64>: slots/page * pages/block (~1518).
constexpr std::size_t kSlotsPerBlock
    = VPageU::num_slots_per_page * (viper::BLOCK_SIZE / sizeof(VPageU));

std::size_t tail_blocks_for(std::size_t tail_entries) {
    return (tail_entries + kSlotsPerBlock - 1) / kSlotsPerBlock;
}

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

struct PointResult {
    std::size_t tail_target{0};
    std::size_t tail_blocks{0};
    std::size_t threads{0};
    std::size_t replayed{0};
    double tail_scan_ms{0.0};
    double ctor_ms{0.0};
    double viper_open_ms{0.0};
    double cold_open_ms{0.0};
    double chkpt_open_ms{0.0};
    double total_ms{0.0};
    std::size_t sanity_hits{0};
};

// Open viper(skip_recovery) once to read the post-prefill write frontier
// (the next block a client would claim). Closed immediately.
viper::block_size_t read_current_frontier() {
    viper::ViperConfig vcfg;
    vcfg.skip_recovery = true;
    vcfg.cceh_init_cap = 1;
    auto viper = ViperT::open(kPoolDir, vcfg);
    return viper::KeyValueOffset{viper->hiom_vpage_frontier()}.block_number;
}

// Probe the highest VPage block that actually holds a live record. The write
// frontier (current_block) can sit ~10 blocks above the real data top (the
// last blocks are claimed-but-empty / half-full), so anchoring small tails to
// the frontier makes them replay ~0. Anchoring to data_top instead lets a
// cadence-sized tail (~3 blocks) replay real records.
viper::block_size_t probe_data_top(viper::block_size_t current) {
    viper::ViperConfig vcfg;
    vcfg.skip_recovery = true;
    vcfg.cceh_init_cap = 1;
    auto viper = ViperT::open(kPoolDir, vcfg);
    viper::block_size_t b = (current > 0) ? current - 1 : 0;
    for (std::size_t back = 0; back < 128 && b > 0; ++back, --b) {
        std::size_t cnt = 0;
        viper->hiom_visit_records(
            b, b + 1,
            [&cnt](const std::uint64_t&, const std::uint64_t&,
                   viper::KeyValueOffset) { ++cnt; });
        if (cnt > 0) return b;
    }
    return b;
}

// Rewrite the checkpoint so its vpage_frontier points at `block`. The HiOM
// ctor's tail scan then covers [block-1, current_block). seq stays monotonic
// so HiOM's reopen-counter invariant holds.
void write_checkpoint_at_block(viper::block_size_t block,
                               std::size_t cold_size, std::uint64_t seq) {
    auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
    viper::hiom::CheckpointRecord rec{};
    rec.seq = seq;
    rec.flushed_count = cold_size;
    // Three-arg ctor (block, page, slot); NOT the single-arg packed-offset
    // ctor, which would (wrongly) treat `block` as a raw packed offset.
    rec.vpage_frontier = viper::KeyValueOffset(block, 0, 0).offset;
    rec.cold_size = cold_size;
    chkpt->write(rec);
}

// One open+tail-scan measurement against whatever frontier the checkpoint
// currently holds. Idempotent w.r.t. PM (re-upsert into the full ColdTier),
// so it is safe to repeat for reps.
PointResult run_tail_point(std::size_t N, std::size_t tail_target,
                           std::size_t threads, std::size_t hot_buckets,
                           std::size_t cceh_cap) {
    PointResult r;
    r.tail_target = tail_target;
    r.tail_blocks = tail_blocks_for(tail_target);
    r.threads = threads;

    const auto t0 = clk::now();
    viper::ViperConfig vcfg;
    vcfg.skip_recovery = true;
    vcfg.cceh_init_cap = cceh_cap;
    auto viper = ViperT::open(kPoolDir, vcfg);
    const auto t1 = clk::now();
    auto cold = viper::hiom::ColdTier::open(kColdPoolFile);
    const auto t2 = clk::now();
    auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
    const auto t3 = clk::now();
    HiOMT::RecoveryConfig rcfg;
    rcfg.tail_scan = true;
    rcfg.recovery_threads = threads;
    HiOMT hiom(*viper, hot_buckets, cold.get(),
               HiOMT::FlusherConfig{}, chkpt.get(),
               HiOMT::CheckpointConfig{}, rcfg);
    const auto t4 = clk::now();

    r.viper_open_ms = ms_between(t0, t1);
    r.cold_open_ms = ms_between(t1, t2);
    r.chkpt_open_ms = ms_between(t2, t3);
    r.ctor_ms = ms_between(t3, t4);
    r.total_ms = ms_between(t0, t4);
    r.tail_scan_ms = hiom.recovery_tail_scan_ms();
    r.replayed
        = hiom.stats().recovery_replayed.load(std::memory_order_relaxed);

    auto client = hiom.get_client();
    std::mt19937_64 rng(0xfeed);
    std::size_t hits = 0;
    for (std::size_t i = 0; i < 100; ++i) {
        const std::uint64_t k = (rng() % N) + 1;
        std::uint64_t got = 0;
        if (client.get(k, &got)) ++hits;
    }
    r.sanity_hits = hits;
    return r;
}

void write_sweep_csv(const std::vector<PointResult>& rows) {
    std::filesystem::create_directories(kResultsDir);
    const std::string path = std::string(kResultsDir) + "/tail_sweep.csv";
    std::ofstream f(path);
    f << "tail_target,tail_blocks,threads,recovery_replayed,tail_scan_ms,"
         "ctor_ms,viper_open_ms,cold_open_ms,chkpt_open_ms,total_ms,"
         "sanity_hits\n";
    for (const auto& r : rows) {
        f << r.tail_target << ',' << r.tail_blocks << ',' << r.threads << ','
          << r.replayed << ',' << r.tail_scan_ms << ',' << r.ctor_ms << ','
          << r.viper_open_ms << ',' << r.cold_open_ms << ',' << r.chkpt_open_ms
          << ',' << r.total_ms << ',' << r.sanity_hits << '\n';
    }
    std::printf("wrote %s (%zu rows)\n", path.c_str(), rows.size());
}

void write_sweep_meta(std::size_t N, viper::block_size_t current_block,
                      double baseline_ms, const std::vector<std::size_t>& tails,
                      const std::vector<std::size_t>& thread_set, int reps,
                      std::size_t hot_buckets, std::size_t cceh_cap) {
    std::filesystem::create_directories(kResultsDir);
    const std::string path
        = std::string(kResultsDir) + "/tail_sweep_meta.json";
    std::ofstream f(path);
    f << "{\n";
    f << "  \"N\": " << N << ",\n";
    f << "  \"current_block\": " << static_cast<std::size_t>(current_block)
      << ",\n";
    f << "  \"slots_per_block\": " << kSlotsPerBlock << ",\n";
    f << "  \"hot_buckets\": " << hot_buckets << ",\n";
    f << "  \"cceh_init_cap\": " << cceh_cap << ",\n";
    f << "  \"cadence_entries\": " << HiOMT::CheckpointConfig{}.cadence_entries
      << ",\n";
    f << "  \"baseline_ms\": " << baseline_ms << ",\n";
    f << "  \"baseline_recovery_threads\": 32,\n";
    f << "  \"reps\": " << reps << ",\n";
    f << "  \"tail_targets\": [";
    for (std::size_t i = 0; i < tails.size(); ++i)
        f << (i ? "," : "") << tails[i];
    f << "],\n";
    f << "  \"threads_swept\": [";
    for (std::size_t i = 0; i < thread_set.size(); ++i)
        f << (i ? "," : "") << thread_set[i];
    f << "]\n}\n";
    std::printf("wrote %s\n", path.c_str());
}

void run_tail_sweep(std::size_t N, bool fresh) {
    if (fresh) {
        std::printf("\n=== C3 TAIL-SWEEP (fresh prefill N=%zu) ===\n", N);
        std::printf("slots/block=%zu  hot_buckets=2^21  cceh_cap=1\n",
                    kSlotsPerBlock);
        cleanup_all();
        prefill(N, /*seed=*/0xc4ec0700);
    } else {
        std::printf("\n=== C3 TAIL-SWEEP (reuse existing PM, assume N=%zu) "
                    "===\n", N);
        std::printf("NON-DESTRUCTIVE: no cleanup/prefill; the checkpoint "
                    "frontier is restored to current at the end.\n");
        std::printf("slots/block=%zu  hot_buckets=2^21  cceh_cap=1\n",
                    kSlotsPerBlock);
    }

    const viper::block_size_t current_block = read_current_frontier();
    std::printf("current_block (post-prefill) = %zu  (~%zu entries)\n",
                static_cast<std::size_t>(current_block),
                static_cast<std::size_t>(current_block) * kSlotsPerBlock);

    const double baseline_ms = time_baseline_open(32);
    std::printf("baseline (Viper O(N) rebuild, t=32) = %.1f ms\n", baseline_ms);

    const viper::block_size_t data_top = probe_data_top(current_block);
    std::printf("data_top (highest block with live records) = %zu  "
                "(frontier - %zu)\n\n",
                static_cast<std::size_t>(data_top),
                static_cast<std::size_t>(current_block - data_top));

    constexpr std::size_t kHotBuckets = 1ULL << 21;
    constexpr std::size_t kCcehCap = 1;
    const std::vector<std::size_t> tails
        = {0, 1000, 4096, 16384, 65536, 262144, 1048576};
    const std::vector<std::size_t> thread_set = {1, 32};
    constexpr int kReps = 3;

    std::vector<PointResult> all_rows;
    std::uint64_t seq = 2;  // prefill wrote seq=1

    for (std::size_t tail : tails) {
        const std::size_t tb = tail_blocks_for(tail);
        // Anchor the tail to the real data top, not the frontier (which has
        // ~10 empty trailing blocks). tail=0 stays at the frontier as a true
        // floor (~0 replay); tail>0 covers ~tb blocks of real records ending
        // at data_top, so even a cadence-sized tail replays real entries.
        viper::block_size_t target_block;
        if (tail == 0) {
            target_block = current_block;
        } else {
            const std::size_t top1 = static_cast<std::size_t>(data_top) + 1;
            target_block = (top1 > tb)
                ? static_cast<viper::block_size_t>(top1 - tb)
                : 0;
        }
        write_checkpoint_at_block(target_block, N, seq++);

        for (std::size_t threads : thread_set) {
            std::vector<double> ts_ms, tot_ms;
            std::size_t replayed = 0;
            for (int rep = 0; rep < kReps; ++rep) {
                PointResult r = run_tail_point(N, tail, threads, kHotBuckets,
                                               kCcehCap);
                all_rows.push_back(r);
                ts_ms.push_back(r.tail_scan_ms);
                tot_ms.push_back(r.total_ms);
                replayed = r.replayed;
                if (r.sanity_hits != 100) {
                    std::cerr << "  FAIL: sanity " << r.sanity_hits
                              << "/100 at tail=" << tail
                              << " threads=" << threads << std::endl;
                    std::exit(1);
                }
            }
            std::printf("tail=%-8zu blk=%-4zu t=%-2zu  replayed=%-9zu  "
                        "tail_scan=%8.2f ms  total=%8.2f ms  (median/%d)\n",
                        tail, tb, threads, replayed, median_of(ts_ms),
                        median_of(tot_ms), kReps);
        }
    }

    // Restore frontier to current so leftover PM has tail≈0 next time.
    write_checkpoint_at_block(current_block, N, seq++);

    write_sweep_csv(all_rows);
    write_sweep_meta(N, current_block, baseline_ms, tails, thread_set, kReps,
                     kHotBuckets, kCcehCap);
}

}  // namespace

int main(int argc, char** argv) {
    // Default sweep is dev-loop: small N for quick iteration. Pass --full to
    // run the 100M case (~30 min including prefill). --open-only reuses
    // existing PM. --tail-sweep-prefill <N> runs the C3 recovery-sensitivity
    // sweep: fresh prefill then a tail-size sweep at threads {1, 32}.
    bool full = false;
    bool open_only = false;
    bool tail_sweep_prefill = false;
    bool tail_sweep_reuse = false;
    std::size_t tail_sweep_N = 10'000'000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--full") full = true;
        else if (a == "--open-only") open_only = true;
        else if (a == "--tail-sweep-prefill") {
            tail_sweep_prefill = true;
            if (i + 1 < argc)
                tail_sweep_N = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (a == "--tail-sweep") {
            // Non-destructive: reuse whatever PM state exists (assumed the
            // 100M from a prior --full unless an N is given).
            tail_sweep_reuse = true;
            tail_sweep_N = 100'000'000;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                tail_sweep_N = std::strtoull(argv[++i], nullptr, 10);
        }
    }

    constexpr std::uint8_t kThreads = 32;

    std::printf("HiOM recovery benchmark (M6.5)\n");
    std::printf("Pool=%s  recovery_threads=%u\n",
                kPoolDir, kThreads);

    if (tail_sweep_prefill || tail_sweep_reuse) {
        run_tail_sweep(tail_sweep_N, /*fresh=*/tail_sweep_prefill);
        std::printf("\nDone.\n");
        return 0;
    }

    if (open_only) {
        run_open_only(kThreads);
        std::printf("\nDone.\n");
        return 0;
    }

    std::vector<std::size_t> sizes = {1'000'000, 10'000'000};
    if (full) sizes.push_back(100'000'000);

    if (!full) {
        std::printf("(dev-loop sweep; pass --full for 100M case)\n");
    }

    for (auto N : sizes) run_one(N, kThreads);

    std::printf("\nDone.\n");
    return 0;
}
