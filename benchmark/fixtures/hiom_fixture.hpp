#pragma once

// HiOMFixture — Google Benchmark fixture wrapping viper::hiom::HiOM<K,V>
// to share the same harness (`all_ops_bm`, `ycsb_bm`) used by
// ViperFixture and the other paper baselines.
//
// What this owns and tears down per benchmark:
//   - viper::Viper<K,V>           PM-resident KV store at kHiomViperPoolDir
//   - viper::hiom::ColdTier       PM-resident hash index at kHiomColdFile
//   - viper::hiom::Checkpoint     PM-resident A/B checkpoint at kHiomChkptFile
//   - viper::hiom::HiOM<K,V>      DRAM hot tier + commit buffer + flusher
//
// Lifecycle parity with ViperFixture: InitMap creates everything from
// scratch (defensive cleanup of our own paths only — never blanket-rm
// under /pmem0; that mount is shared per CLAUDE.md). DeInitMap drops
// HiOM (whose dtor drains the commit buffer), then Viper, then removes
// our pool files.
//
// Sizing notes: HotTier capacity is set to 2× the configured prefill
// size by default, so every prefill key fits in DRAM and the
// fixture measures HiOM's steady-state path rather than its eviction
// path. Override via SetHotTierBuckets() before InitMap if you want
// to exercise SIEVE eviction explicitly.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

#include "../benchmark.hpp"
#include "common_fixture.hpp"
#include "viper/viper.hpp"
#include "viper/hiom/checkpoint.hpp"
#include "viper/hiom/cold_tier.hpp"
#include "viper/hiom/hiom.hpp"

namespace viper::kv_bm {

// All HiOM PM artefacts live under a single dedicated directory. Same
// "/pmem0/hiom*" prefix used by integration tests + hiom_recovery_bm,
// so /pmem0 cleanup rules in CLAUDE.md still apply (only this prefix
// is touched by our cleanup).
inline constexpr char kHiomBenchRoot[] = "/pmem0/hiom_bench";
inline constexpr char kHiomViperPoolDir[] = "/pmem0/hiom_bench/viper";
inline constexpr char kHiomColdFile[] = "/pmem0/hiom_bench/cold.bin";
inline constexpr char kHiomChkptFile[] = "/pmem0/hiom_bench/chkpt.bin";

// ColdTier + A/B-checkpoint backing directory. Defaults to the PM bench
// root. Override with HIOM_COLD_DIR to place the *secondary index* on a
// different medium — e.g. a node-1 remote-DRAM tmpfs (mounted mpol=bind:1)
// to emulate a volatile CXL-DDR cold tier and measure whether the write
// penalty shrinks vs Optane. Viper's VPage *data* pool always stays on PM
// (kHiomViperPoolDir); only the index backing moves, so the comparison
// isolates the secondary-index medium.
inline std::string hiom_cold_dir() {
    if (const char* e = std::getenv("HIOM_COLD_DIR"); e && *e) {
        return std::string{e};
    }
    return std::string{kHiomBenchRoot};
}

template <typename KeyT = KeyType16, typename ValueT = ValueType200>
class HiOMFixture : public BaseFixture {
  public:
    using KeyType = KeyT;
    using ViperT = viper::Viper<KeyT, ValueT>;
    using HiOMT = viper::hiom::HiOM<KeyT, ValueT>;

    void InitMap(const uint64_t num_prefill_inserts = 0,
                 const bool re_init = true) override;
    void DeInitMap() override;

    uint64_t insert(uint64_t start_idx, uint64_t end_idx) final;

    uint64_t setup_and_insert(uint64_t start_idx, uint64_t end_idx) final;
    uint64_t setup_and_find(uint64_t start_idx, uint64_t end_idx,
                            uint64_t num_finds) final;
    uint64_t setup_and_delete(uint64_t start_idx, uint64_t end_idx,
                              uint64_t num_deletes) final;
    uint64_t setup_and_update(uint64_t start_idx, uint64_t end_idx,
                              uint64_t num_updates) final;

    uint64_t run_ycsb(uint64_t start_idx, uint64_t end_idx,
                      const std::vector<ycsb::Record>& data,
                      LatencyHistograms hdrs) final;

    // Drains the HiOM commit buffer and joins the flusher's outstanding
    // work so the timed YCSB read phase doesn't contend with PMem write
    // traffic from the flusher. Called by ycsb_bm's ycsb_run after
    // prefill_ycsb finishes (init thread only).
    void flush_post_prefill() final {
        if (hiom_) hiom_->flush_and_wait();
        warm_hot_if_requested();
    }

    // Augments the base RSS snapshot with HiOM-specific telemetry from
    // hiom_->hot_tier() and hiom_->stats(). Called by ycsb_run on the
    // init thread after the timed loop ends. hot_tier() accessors are
    // O(1) atomic loads; stats() folds the per-Client read shards into
    // the aggregate (O(#client slots)) — safe here because the timed
    // loop has already joined every worker, so no Client is mid-get().
    MemSnapshot fixture_telemetry() override {
        MemSnapshot snap = capture_mem();
        if (hiom_) {
            auto& ht = hiom_->hot_tier();
            snap.hot_size = ht.size();
            snap.hot_capacity = ht.capacity();
            snap.hot_evictions = ht.eviction_count();
            snap.hot_dram_bytes = ht.dram_bytes();
            const auto& stats = hiom_->stats();
            snap.hot_hits = stats.hot_hits.load(std::memory_order_relaxed);
            snap.cold_hits = stats.cold_hits.load(std::memory_order_relaxed);
        }
        return snap;
    }

    // White-box index DRAM = HiOM tiers (HotTier dominant; ColdTier in PMem)
    // plus the residual single-segment CCEH (cceh_init_cap=1 ⇒ ~16 KB) that
    // Viper still constructs under HiOM. Reported as fixture_dram_mb.
    size_t fixture_dram_bytes() override {
        size_t b = hiom_ ? hiom_->dram_bytes() : 0;
        if (viper_) b += viper_->cceh_dram_bytes();
        return b;
    }

    HiOMT* getHiom() { return hiom_.get(); }
    ViperT* getViper() { return viper_.get(); }

    // Optional knobs — call before InitMap to override defaults.
    void SetHotTierBuckets(std::size_t b) { hot_buckets_pow2_ = b; }
    void SetCheckpointCadence(std::uint64_t e) { checkpoint_cadence_ = e; }

  protected:
    // Warm the HotTier from the authoritative ColdTier so the timed read
    // phase starts page-resident and populated, lifting the lazy-mmap
    // first-touch fault + cold→hot re-warm out of the measurement (see
    // HiOM::warm_hot_tier / hot_tier.hpp). Opt-in via HIOM_WARM_HOT so
    // existing eval configs — and the C2 from-empty capacity sweep, which
    // intentionally measures warming during the read phase — keep their
    // current default behaviour. Called post-prefill, pre-timed-loop, on
    // the init thread only.
    void warm_hot_if_requested() {
        if (!hiom_) return;
        const char* e = std::getenv("HIOM_WARM_HOT");
        if (!e || e[0] == '0' || e[0] == '\0') return;
        const std::size_t warmed = hiom_->warm_hot_tier();
        std::cerr << "[HiOM] warm_hot_tier installed " << warmed
                  << " offsets into HotTier" << std::endl;
    }

    // Defensive cleanup helper. Only touches our own paths under
    // /pmem0/hiom_bench/; mirrors the CLAUDE.md-mandated viper_fixture
    // pattern. A misconfigured prefix aborts before we touch anything.
    static void cleanup_hiom_artefacts() {
        const std::string root{kHiomBenchRoot};
        if (root.find("/pmem0/hiom_bench") != 0) {
            std::cerr << "Refusing to clean unfamiliar HiOM bench path: "
                      << root << std::endl;
            std::abort();
        }
        // remove_all is safe here because the prefix check guards us
        // against ever blanket-rm'ing other tenants' /pmem0 dirs.
        if (std::filesystem::exists(root)) {
            std::filesystem::remove_all(root);
        }
        std::filesystem::create_directories(root);
    }

    std::unique_ptr<ViperT> viper_;
    std::unique_ptr<viper::hiom::ColdTier> cold_;
    std::unique_ptr<viper::hiom::Checkpoint> chkpt_;
    std::unique_ptr<HiOMT> hiom_;
    bool initialized_ = false;

    // 2^21 buckets × 16 slots = 33 M HotTier slots, ~256 MB DRAM. The
    // ALL_OPS suite runs (prefill=1M, op=1M) = 2 M live keys, plenty of
    // headroom; YCSB prefills 10 M and the 5050/1090 mixes add up to
    // ~5 M inserts ⇒ ~15 M working set — 2× margin so reads stay
    // HotTier-resident instead of falling through to ColdTier PMem.
    // Override via SetHotTierBuckets() to exercise eviction explicitly.
    std::size_t hot_buckets_pow2_ = (1ULL << 21);
    std::uint64_t checkpoint_cadence_ = 4096;
};

template <typename KeyT, typename ValueT>
void HiOMFixture<KeyT, ValueT>::InitMap(uint64_t num_prefill_inserts,
                                        const bool re_init) {
    if (initialized_ && !re_init) {
        return;
    }
    cleanup_hiom_artefacts();

    // Optional capacity override for the C2 HotTier-capacity sweep.
    // HIOM_HOT_BUCKETS_LOG2 = log2(num_buckets); validated to [10,30] so a
    // typo can't silently pick an absurd capacity or UB the 1ULL<<log2 shift.
    if (const char* e = std::getenv("HIOM_HOT_BUCKETS_LOG2")) {
        char* end = nullptr;
        const unsigned long log2 = std::strtoul(e, &end, 10);
        if (end == e || *end != '\0' || log2 < 10 || log2 > 30) {
            std::cerr << "HIOM_HOT_BUCKETS_LOG2 must be an integer in [10,30], got: "
                      << e << std::endl;
            std::abort();
        }
        hot_buckets_pow2_ = 1ULL << log2;
    }

    // 1. Viper. We use a directory pool (mirrors integration tests), not
    //    the /dev/dax* pool that ViperFixture defaults to — keeps HiOM's
    //    artefacts tidy in one place and avoids fighting ViperFixture
    //    over the dax device when both fixtures are wired into the same
    //    binary.
    ViperConfig vcfg;
    // M6.6: HiOM owns the index via set_hiom_owns_index(true) below
    // (called by HiOM's constructor), so CCEH is never inserted into
    // on the put/update/remove paths. Shrink the eager pre-allocation
    // from ~2 GiB (131,072 segments × 16 KiB) to a single 16 KiB
    // segment. This is the key DRAM reduction that makes the
    // win-condition narrative ("HiOM uses ≥50% less DRAM than Viper")
    // actually true in the measurement. Only the SIEVE mirror_write
    // touch on update paths is weakened by an empty CCEH (mirror_write
    // peeks CCEH for the offset and skips if tombstone) — correctness
    // unaffected; YCSB-C is read-only so this side-effect is moot.
    vcfg.cceh_init_cap = 1;
    // Opt-in prefault, same switch as ViperFixture (VIPER_PREFAULT=1):
    // pre-touch fresh PM mappings at create/resize time so timed puts skip
    // the first-touch page fault (design/HIOM.md E4). Keep it OFF for
    // recovery experiments.
    if (const char* pf = std::getenv("VIPER_PREFAULT"); pf && *pf && *pf != '0') {
        vcfg.prefault_new_mappings = true;
        std::cerr << "[HiOM] VIPER_PREFAULT set -> prefaulting new PM mappings" << std::endl;
    }
    // Bounded create-time prefault: cap the touch to VIPER_PREFAULT_BYTES so
    // create() stays fast on the 64 GiB pool (touch only the working set).
    if (const char* pb = std::getenv("VIPER_PREFAULT_BYTES"); pb && *pb) {
        vcfg.prefault_bytes = std::strtoull(pb, nullptr, 10);
        std::cerr << "[HiOM] VIPER_PREFAULT_BYTES=" << vcfg.prefault_bytes << std::endl;
    }
    // Background prefault runway: async, off create(). Keep OFF for recovery
    // experiments (don't set the env there).
    if (const char* rw = std::getenv("VIPER_PREFAULT_RUNWAY"); rw && *rw && *rw != '0') {
        vcfg.background_prefault = true;
        if (const char* rb = std::getenv("VIPER_RUNWAY_BLOCKS"); rb && *rb)
            vcfg.prefault_runway_blocks = std::strtoul(rb, nullptr, 10);
        std::cerr << "[HiOM] VIPER_PREFAULT_RUNWAY set -> background prefault runway ("
                  << vcfg.prefault_runway_blocks << " blocks)" << std::endl;
    }
    viper_ = ViperT::create(kHiomViperPoolDir, BM_POOL_SIZE, vcfg);

    // 2. ColdTier (PM-resident hash index). Default sizing in
    //    ColdTier::create handles up to ~256K main + 256K overflow per
    //    region × 32 regions = 16M entries main capacity, plenty for
    //    the 1M default and the 100M aspirational sweep.
    //    HIOM_COLD_DIR can redirect this backing off PM (e.g. a node-1
    //    remote-DRAM tmpfs) to emulate a volatile CXL-DDR cold tier.
    const std::string cold_dir = hiom_cold_dir();
    std::filesystem::create_directories(cold_dir);
    if (std::getenv("HIOM_COLD_DIR")) {  // only chatter when redirected off PM
        std::cerr << "[HiOM] ColdTier+checkpoint backing = " << cold_dir << std::endl;
    }
    cold_ = viper::hiom::ColdTier::create(cold_dir + "/cold.bin");

    // 3. Checkpoint (A/B record, same backing medium as ColdTier).
    chkpt_ = viper::hiom::Checkpoint::create(cold_dir + "/chkpt.bin");

    // 4. HiOM. CheckpointConfig.cadence_entries gates per-N-flushed
    //    checkpoint writes; 4096 is the integration-test default and
    //    matches HIOM.md §M5. FlusherConfig defaults to 5 ms / 1024
    //    high-watermark.
    typename HiOMT::FlusherConfig fcfg;
    // HIOM_FLUSHERS (must divide CommitBuffer::kNumLanes=8; default 4):
    // the 2026-07-07 split-probe showed background flusher PM writes poison
    // the foreground resolver's PM reads — fewer flushers => far faster
    // write path (INSERT t8 4.2× / t24 2.1× at f1 vs f4 in the microprobe).
    // Wired here so the YCSB eval can confirm the microprobe result on the
    // real workload and check the recovery/flush-drain trade-off before we
    // change the struct default. See design/HIOM.md §Status(2026-07-07).
    if (const char* e = std::getenv("HIOM_FLUSHERS"); e && *e) {
        fcfg.num_flushers = std::strtoul(e, nullptr, 10);
        std::cerr << "[HiOM] HIOM_FLUSHERS=" << fcfg.num_flushers << std::endl;
    }
    typename HiOMT::CheckpointConfig ccfg;
    ccfg.cadence_entries = checkpoint_cadence_;
    typename HiOMT::RecoveryConfig rcfg;  // tail_scan=false (fresh DB)
    // Build HiOM with a LARGE HotTier for prefill so small-capacity sweep
    // points don't livelock during the 10M put storm (SIEVE evict vs PINNED
    // slots — same root cause as a_zipf-33M). The read phase then rebuilds
    // at the target capacity from an EMPTY HotTier, backed by the
    // authoritative ColdTier; this also yields a cleaner per-capacity steady
    // state (every point starts cold, not from prefill residue).
    const std::size_t target_buckets = hot_buckets_pow2_;
    const std::size_t prefill_buckets =
        target_buckets > (std::size_t{1} << 21) ? target_buckets
                                                 : (std::size_t{1} << 21);
    hiom_ = std::make_unique<HiOMT>(*viper_, prefill_buckets,
                                    cold_.get(), fcfg,
                                    chkpt_.get(), ccfg, rcfg);

    this->prefill(num_prefill_inserts);

    // Flush the prefill so the steady-state benchmark phase doesn't pay
    // for "background catch-up" — every prefill entry is in ColdTier
    // and the HotTier slots are kUnpinned by the time the timer starts.
    hiom_->flush_and_wait();

    if (target_buckets != prefill_buckets) {
        // Rebuild at the target HotTier capacity. viper_/cold_/chkpt_ persist
        // (ColdTier holds all N entries, authoritative); the new HotTier
        // starts empty and warms from ColdTier during the read phase.
        hiom_.reset();
        hiom_ = std::make_unique<HiOMT>(*viper_, target_buckets,
                                        cold_.get(), fcfg,
                                        chkpt_.get(), ccfg, rcfg);
    }
    // Optionally warm the (final) HotTier from ColdTier so an all_ops
    // timed read phase doesn't absorb the lazy-mmap first-touch faults.
    // Opt-in (HIOM_WARM_HOT); no-op for YCSB (prefill=0 here → empty
    // ColdTier), which warms in flush_post_prefill instead.
    warm_hot_if_requested();
    initialized_ = true;
}

template <typename KeyT, typename ValueT>
void HiOMFixture<KeyT, ValueT>::DeInitMap() {
    BaseFixture::DeInitMap();
    // Order matters: HiOM's dtor drains the commit buffer and joins
    // the flusher; only after that is it safe to drop Viper (the
    // flusher reads viper_).
    hiom_.reset();
    chkpt_.reset();
    cold_.reset();
    viper_.reset();
    initialized_ = false;

    // Don't bother re-creating the directory; the next InitMap
    // re-cleans before re-creating. Just remove our own tree.
    const std::string root{kHiomBenchRoot};
    if (root.find("/pmem0/hiom_bench") == 0
        && std::filesystem::exists(root)) {
        std::filesystem::remove_all(root);
    }
    // If ColdTier/checkpoint were redirected off the PM root (HIOM_COLD_DIR,
    // e.g. a remote-DRAM tmpfs), remove just the two files we created —
    // never the directory itself, which we don't own.
    const std::string cold_dir = hiom_cold_dir();
    if (cold_dir != std::string{kHiomBenchRoot}) {
        std::error_code ec;
        std::filesystem::remove(cold_dir + "/cold.bin", ec);
        std::filesystem::remove(cold_dir + "/chkpt.bin", ec);
    }
}

template <typename KeyT, typename ValueT>
uint64_t HiOMFixture<KeyT, ValueT>::insert(uint64_t start_idx,
                                            uint64_t end_idx) {
    auto cl = hiom_->get_client();
    // PoC opt-in (HIOM_ASSUME_NEW): the insert benchmark and prefill both issue
    // only brand-new, monotonically-increasing keys, so HiOM's put can skip the
    // old-offset resolver (a ColdTier PM negative lookup that is pure waste for
    // a new key — see HiOM write-path attribution, results/wbw). Read once here,
    // off the per-op loop. YCSB run / update / delete never reach insert(), so
    // the resolver-skip's unsafe case (key already exists) can't be triggered.
    const char* e = std::getenv("HIOM_ASSUME_NEW");
    const bool assume_new = e && e[0] != '0' && e[0] != '\0';
    uint64_t insert_counter = 0;
    for (uint64_t key = start_idx; key < end_idx; ++key) {
        const KeyT db_key{key};
        const ValueT value{key};
        insert_counter += cl.put(db_key, value, assume_new);
    }
    return insert_counter;
}

template <typename KeyT, typename ValueT>
uint64_t HiOMFixture<KeyT, ValueT>::setup_and_insert(uint64_t start_idx,
                                                      uint64_t end_idx) {
    return insert(start_idx, end_idx);
}

template <typename KeyT, typename ValueT>
uint64_t HiOMFixture<KeyT, ValueT>::setup_and_find(uint64_t start_idx,
                                                    uint64_t end_idx,
                                                    uint64_t num_finds) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);
    std::uniform_real_distribution<double> udist(0.0, 1.0);

    auto cl = hiom_->get_client();
    uint64_t found_counter = 0;

    // Opt-in skewed key distribution (HIOM_ZIPF=theta, e.g. 0.99). Default /
    // unset => uniform (original behaviour). Standard YCSB ZipfianGenerator:
    // rank 0 (== start_idx) is hottest, so the hot value set stays tiny and
    // LLC-resident. This is the clean way to test [[value-cache-negative]]'s
    // mechanism — whether value-read prefetch still helps once the hot values
    // no longer miss to PM. zeta() is O(range) but computed once per thread.
    const uint64_t range = end_idx > start_idx ? end_idx - start_idx : 1;
    struct ZipfGen {
        double theta, zeta_n, alpha, eta;
        uint64_t n;
        static double zeta(uint64_t k, double th) {
            double s = 0.0;
            for (uint64_t i = 1; i <= k; ++i) s += 1.0 / std::pow((double)i, th);
            return s;
        }
        ZipfGen(uint64_t n_, double th) : theta(th), n(n_) {
            const double z2 = zeta(2, th);
            zeta_n = zeta(n, th);
            alpha = 1.0 / (1.0 - th);
            eta = (1.0 - std::pow(2.0 / (double)n, 1.0 - th)) /
                  (1.0 - z2 / zeta_n);
        }
        uint64_t next(double u) const {
            const double uz = u * zeta_n;
            if (uz < 1.0) return 0;
            if (uz < 1.0 + std::pow(0.5, theta)) return 1;
            return (uint64_t)((double)n * std::pow(eta * u - eta + 1.0, alpha));
        }
    };
    static thread_local const double zipf_theta = [] {
        const char* e = std::getenv("HIOM_ZIPF");
        return (!e || !*e) ? 0.0 : std::strtod(e, nullptr);
    }();
    const bool use_zipf = zipf_theta > 0.0;
    const ZipfGen zipf{range, use_zipf ? zipf_theta : 0.99};
    auto next_key = [&]() -> uint64_t {
        return use_zipf ? start_idx + (zipf.next(udist(rnd_engine)) % range)
                        : (uint64_t)distrib(rnd_engine);
    };

    // Opt-in batched group-prefetching read path (HIOM_GET_BATCH=B, B in
    // [2, HiOMT::Client::kGetBatchMax]). Default / unset preserves the
    // original per-key cl.get() loop exactly, so every existing eval config
    // is unaffected. The batch path gathers B keys, then cl.get_batch()
    // runs the two-pass prefetch pipeline. See HiOM::Client::get_batch.
    static thread_local const uint64_t batch = [] {
        const char* e = std::getenv("HIOM_GET_BATCH");
        if (!e || !*e) return uint64_t{0};
        const uint64_t b = std::strtoull(e, nullptr, 10);
        constexpr uint64_t kMax = HiOMT::Client::kGetBatchMax;
        return b < 2 ? uint64_t{0} : (b > kMax ? kMax : b);
    }();

    if (batch >= 2) {
        std::vector<KeyT> keys(batch);
        std::vector<ValueT> vals(batch);
        std::vector<char> got(batch);
        uint64_t done = 0;
        while (done < num_finds) {
            const uint64_t b = std::min<uint64_t>(batch, num_finds - done);
            for (uint64_t j = 0; j < b; ++j)
                keys[j] = KeyT{next_key()};
            cl.get_batch(keys.data(), b, vals.data(),
                         reinterpret_cast<bool*>(got.data()));
            for (uint64_t j = 0; j < b; ++j)
                found_counter += got[j]
                    && (vals[j] == ValueT{static_cast<uint64_t>(
                                              keys[j].data[0])});
            done += b;
        }
        return found_counter;
    }

    ValueT value;
    for (uint64_t i = 0; i < num_finds; ++i) {
        const uint64_t key = next_key();
        const KeyT db_key{key};
        const bool found = cl.get(db_key, &value);
        found_counter += found && (value == ValueT{key});
    }
    return found_counter;
}

template <typename KeyT, typename ValueT>
uint64_t HiOMFixture<KeyT, ValueT>::setup_and_delete(uint64_t start_idx,
                                                      uint64_t end_idx,
                                                      uint64_t num_deletes) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    auto cl = hiom_->get_client();
    uint64_t delete_counter = 0;
    for (uint64_t i = 0; i < num_deletes; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const KeyT db_key{key};
        delete_counter += cl.remove(db_key);
    }
    return delete_counter;
}

template <typename KeyT, typename ValueT>
uint64_t HiOMFixture<KeyT, ValueT>::setup_and_update(uint64_t start_idx,
                                                      uint64_t end_idx,
                                                      uint64_t num_updates) {
    std::random_device rnd{};
    auto rnd_engine = std::default_random_engine(rnd());
    std::uniform_int_distribution<> distrib(start_idx, end_idx);

    auto cl = hiom_->get_client();
    uint64_t update_counter = 0;
    auto update_fn = [](ValueT* value) {
        value->update_value();
        viper::internal::pmem_persist(value, sizeof(uint64_t));
    };
    for (uint64_t i = 0; i < num_updates; ++i) {
        const uint64_t key = distrib(rnd_engine);
        const KeyT db_key{key};
        update_counter += cl.update(db_key, update_fn);
    }
    return update_counter;
}

template <typename KeyT, typename ValueT>
uint64_t HiOMFixture<KeyT, ValueT>::run_ycsb(
    uint64_t /*start_idx*/, uint64_t /*end_idx*/,
    const std::vector<ycsb::Record>& /*data*/, LatencyHistograms /*hdrs*/) {
    throw std::runtime_error{
        "YCSB not implemented for non-(KeyType8, ValueType200) HiOM."};
}

// YCSB workload uses KeyType8 / ValueType200 (mirrors ViperFixture).
template <>
inline uint64_t HiOMFixture<KeyType8, ValueType200>::run_ycsb(
    uint64_t start_idx, uint64_t end_idx,
    const std::vector<ycsb::Record>& data, LatencyHistograms hdrs) {
    auto cl = hiom_->get_client();
    ValueType200 value;
    const ValueType200 null_value{0ul};

    const bool log_latency = (hdrs.read != nullptr || hdrs.write != nullptr);
    uint64_t op_count = 0;
    std::chrono::high_resolution_clock::time_point start;

    // Opt-in batched group-prefetching read path for YCSB (HIOM_GET_BATCH=B).
    // Only in throughput mode: batching folds B reads' latency into one
    // sample, which would corrupt the per-op HDR histogram — so latency runs
    // always take the scalar path below. Consecutive GETs are gathered into a
    // batch resolved by cl.get_batch (same two-pass prefetch pipeline as
    // all_ops); any non-GET op flushes the pending batch first, preserving
    // program order across the read/write boundary. For a 100r (YCSB-C)
    // workload this degenerates to a pure prefetch pipeline. Semantics
    // (op_count) are identical to the scalar loop.
    static thread_local const uint64_t ycsb_batch = [] {
        const char* e = std::getenv("HIOM_GET_BATCH");
        if (!e || !*e) return uint64_t{0};
        const uint64_t b = std::strtoull(e, nullptr, 10);
        constexpr uint64_t kMax = HiOMT::Client::kGetBatchMax;
        return b < 2 ? uint64_t{0} : (b > kMax ? kMax : b);
    }();
    if (ycsb_batch >= 2 && !log_latency) {
        std::vector<KeyType8> keys(ycsb_batch);
        std::vector<ValueType200> vals(ycsb_batch);
        std::vector<char> got(ycsb_batch);
        uint64_t bn = 0;
        auto flush = [&]() {
            if (bn == 0) return;
            cl.get_batch(keys.data(), bn, vals.data(),
                         reinterpret_cast<bool*>(got.data()));
            for (uint64_t j = 0; j < bn; ++j)
                op_count += got[j] && (vals[j] != null_value);
            bn = 0;
        };
        for (uint64_t op_num = start_idx; op_num < end_idx; ++op_num) {
            const ycsb::Record& record = data[op_num];
            switch (record.op) {
                case ycsb::Record::Op::GET:
                    keys[bn++] = record.key;
                    if (bn == ycsb_batch) flush();
                    break;
                case ycsb::Record::Op::INSERT:
                    flush();
                    cl.put(record.key, record.value);
                    ++op_count;
                    break;
                case ycsb::Record::Op::UPDATE: {
                    flush();
                    auto update_fn = [&](ValueType200* v) {
                        v->data[0] = record.value.data[0];
                        viper::internal::pmem_persist(
                            v->data.data(), sizeof(uint64_t));
                    };
                    op_count += cl.update(record.key, update_fn);
                    break;
                }
                default:
                    throw std::runtime_error(
                        "Unknown YCSB op: "
                        + std::to_string(static_cast<int>(record.op)));
            }
        }
        flush();
        return op_count;
    }

    for (uint64_t op_num = start_idx; op_num < end_idx; ++op_num) {
        const ycsb::Record& record = data[op_num];

        if (log_latency) {
            start = std::chrono::high_resolution_clock::now();
        }

        hdr_histogram* op_hdr = nullptr;
        switch (record.op) {
            case ycsb::Record::Op::INSERT: {
                cl.put(record.key, record.value);
                ++op_count;
                op_hdr = hdrs.write;
                break;
            }
            case ycsb::Record::Op::GET: {
                const bool found = cl.get(record.key, &value);
                op_count += found && (value != null_value);
                op_hdr = hdrs.read;
                break;
            }
            case ycsb::Record::Op::UPDATE: {
                auto update_fn = [&](ValueType200* v) {
                    v->data[0] = record.value.data[0];
                    viper::internal::pmem_persist(
                        v->data.data(), sizeof(uint64_t));
                };
                op_count += cl.update(record.key, update_fn);
                op_hdr = hdrs.write;
                break;
            }
            default: {
                throw std::runtime_error(
                    "Unknown YCSB op: "
                    + std::to_string(static_cast<int>(record.op)));
            }
        }

        if (!log_latency || op_hdr == nullptr) continue;
        const auto end = std::chrono::high_resolution_clock::now();
        const auto duration
            = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        hdr_record_value(op_hdr, duration.count());
    }
    return op_count;
}

}  // namespace viper::kv_bm
