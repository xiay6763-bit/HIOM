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
                      hdr_histogram* hdr) final;

    HiOMT* getHiom() { return hiom_.get(); }
    ViperT* getViper() { return viper_.get(); }

    // Optional knobs — call before InitMap to override defaults.
    void SetHotTierBuckets(std::size_t b) { hot_buckets_pow2_ = b; }
    void SetCheckpointCadence(std::uint64_t e) { checkpoint_cadence_ = e; }

  protected:
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

    // 2^18 buckets × 16 slots = 4M HotTier slots, ~36 MB DRAM. The
    // ALL_OPS suite typically runs (prefill=1M, op=1M) = 2M live keys,
    // so 2× that fits without SIEVE eviction churn. Override via
    // SetHotTierBuckets() to exercise the eviction path explicitly.
    std::size_t hot_buckets_pow2_ = (1ULL << 18);
    std::uint64_t checkpoint_cadence_ = 4096;
};

template <typename KeyT, typename ValueT>
void HiOMFixture<KeyT, ValueT>::InitMap(uint64_t num_prefill_inserts,
                                        const bool re_init) {
    if (initialized_ && !re_init) {
        return;
    }
    cleanup_hiom_artefacts();

    // 1. Viper. We use a directory pool (mirrors integration tests), not
    //    the /dev/dax* pool that ViperFixture defaults to — keeps HiOM's
    //    artefacts tidy in one place and avoids fighting ViperFixture
    //    over the dax device when both fixtures are wired into the same
    //    binary.
    ViperConfig vcfg;
    viper_ = ViperT::create(kHiomViperPoolDir, BM_POOL_SIZE, vcfg);

    // 2. ColdTier (PM-resident hash index). Default sizing in
    //    ColdTier::create handles up to ~256K main + 256K overflow per
    //    region × 32 regions = 16M entries main capacity, plenty for
    //    the 1M default and the 100M aspirational sweep.
    cold_ = viper::hiom::ColdTier::create(kHiomColdFile);

    // 3. Checkpoint (A/B PM record).
    chkpt_ = viper::hiom::Checkpoint::create(kHiomChkptFile);

    // 4. HiOM. CheckpointConfig.cadence_entries gates per-N-flushed
    //    checkpoint writes; 4096 is the integration-test default and
    //    matches HIOM.md §M5. FlusherConfig defaults to 5 ms / 1024
    //    high-watermark.
    typename HiOMT::FlusherConfig fcfg;
    typename HiOMT::CheckpointConfig ccfg;
    ccfg.cadence_entries = checkpoint_cadence_;
    typename HiOMT::RecoveryConfig rcfg;  // tail_scan=false (fresh DB)
    hiom_ = std::make_unique<HiOMT>(*viper_, hot_buckets_pow2_,
                                    cold_.get(), fcfg,
                                    chkpt_.get(), ccfg, rcfg);

    this->prefill(num_prefill_inserts);

    // Flush the prefill so the steady-state benchmark phase doesn't pay
    // for "background catch-up" — every prefill entry is in ColdTier
    // and the HotTier slots are kUnpinned by the time the timer starts.
    hiom_->flush_and_wait();
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
}

template <typename KeyT, typename ValueT>
uint64_t HiOMFixture<KeyT, ValueT>::insert(uint64_t start_idx,
                                            uint64_t end_idx) {
    auto cl = hiom_->get_client();
    uint64_t insert_counter = 0;
    for (uint64_t key = start_idx; key < end_idx; ++key) {
        const KeyT db_key{key};
        const ValueT value{key};
        insert_counter += cl.put(db_key, value);
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

    auto cl = hiom_->get_client();
    uint64_t found_counter = 0;
    ValueT value;
    for (uint64_t i = 0; i < num_finds; ++i) {
        const uint64_t key = distrib(rnd_engine);
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
    const std::vector<ycsb::Record>& /*data*/, hdr_histogram* /*hdr*/) {
    throw std::runtime_error{
        "YCSB not implemented for non-(KeyType8, ValueType200) HiOM."};
}

// YCSB workload uses KeyType8 / ValueType200 (mirrors ViperFixture).
template <>
inline uint64_t HiOMFixture<KeyType8, ValueType200>::run_ycsb(
    uint64_t start_idx, uint64_t end_idx,
    const std::vector<ycsb::Record>& data, hdr_histogram* hdr) {
    auto cl = hiom_->get_client();
    ValueType200 value;
    const ValueType200 null_value{0ul};

    uint64_t op_count = 0;
    std::chrono::high_resolution_clock::time_point start;
    for (uint64_t op_num = start_idx; op_num < end_idx; ++op_num) {
        const ycsb::Record& record = data[op_num];

        if (hdr != nullptr) {
            start = std::chrono::high_resolution_clock::now();
        }

        switch (record.op) {
            case ycsb::Record::Op::INSERT: {
                cl.put(record.key, record.value);
                ++op_count;
                break;
            }
            case ycsb::Record::Op::GET: {
                const bool found = cl.get(record.key, &value);
                op_count += found && (value != null_value);
                break;
            }
            case ycsb::Record::Op::UPDATE: {
                auto update_fn = [&](ValueType200* v) {
                    v->data[0] = record.value.data[0];
                    viper::internal::pmem_persist(
                        v->data.data(), sizeof(uint64_t));
                };
                op_count += cl.update(record.key, update_fn);
                break;
            }
            default: {
                throw std::runtime_error(
                    "Unknown YCSB op: "
                    + std::to_string(static_cast<int>(record.op)));
            }
        }

        if (hdr == nullptr) continue;
        const auto end = std::chrono::high_resolution_clock::now();
        const auto duration
            = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        hdr_record_value(hdr, duration.count());
    }
    return op_count;
}

}  // namespace viper::kv_bm
