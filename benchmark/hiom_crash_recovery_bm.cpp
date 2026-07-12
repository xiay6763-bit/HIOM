// HiOM real process-kill recovery benchmark (E3 / Claim 3, 4, 5).
//
// This is an EVALUATION harness — it lives outside the core (frozen at
// CORE_COMMIT=323f564) and does NOT modify include/viper/** or any fixture
// runtime semantics. It only *drives* the public HiOM/Viper/ColdTier/
// Checkpoint API. The paper artefact records two hashes: CORE_COMMIT and
// EVAL_COMMIT (this file's commit).
//
// Difference from test/hiom_sigkill_test.cpp: that unit test proves
// correctness (lost==0, mismatch==0) at three cadence-tuned phases. This
// harness instead MEASURES recovery, at K8/V200 (the paper's main workload,
// not <u64,u64>), across many *random* kill moments, and emits one CSV row
// per crash with the full phase-split timing and the two distinct tail
// lengths the design doc distinguishes:
//
//   unsafe_suffix_blocks = current_block - durable_frontier
//   scan_blocks          = current_block - max(durable_frontier - 1, 0)
//                          (one boundary block more — the tail scan re-reads
//                           the frontier block itself; never conflate them)
//
// Flow per iteration:
//   cleanup -> fork child -> child: create pool, prefill N distinct keys,
//     flush+force_checkpoint (=> durable frontier at post-prefill cursor),
//     then run the chosen runtime workload publishing per-thread durable
//     watermarks -> parent SIGKILLs at a random delay -> parent reopens
//     (viper skip_recovery + cold + checkpoint + HiOM tail_scan) timing each
//     phase -> verify every confirmed op is recoverable -> append CSV row.
//
// Correctness gate per row: checkpoint proto_version==2, lost==0,
// mismatch==0, recovered==expected, current_block>=durable_frontier.
//
// Workloads (env HIOM_CR_WORKLOAD):
//   insert  : new-key inserts past the prefill keyspace (moves the frontier —
//             the ONLY workload that generates a non-trivial unsafe suffix,
//             because fixed-size V updates are in-place and push no commit
//             entry: see hiom.hpp fixed-size update fast path).
//   ycsb_a  : 50% get / 50% update over the prefilled keys (updates in-place
//             => suffix stays ~0; this is the "update-heavy recovery is near
//             instant" C3 point, reported to contrast with insert).
//   ycsb_b  : 95% get / 5% update over the prefilled keys.
//
// PM artefacts: /pmem0/hiom/crash_recovery/** — prefix-guarded cleanup, same
// rule as the rest of HiOM (CLAUDE.md /pmem0 section). NEVER rm -rf from shell.

#include "viper/viper.hpp"
#include "viper/hiom/checkpoint.hpp"
#include "viper/hiom/cold_tier.hpp"
#include "viper/hiom/hiom.hpp"
#include "benchmark.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

namespace {

using viper::kv_bm::KeyType8;
using viper::kv_bm::ValueType200;

using KeyT = KeyType8;
using ValueT = ValueType200;
using ViperT = viper::Viper<KeyT, ValueT>;
using HiOMT = viper::hiom::HiOM<KeyT, ValueT>;

constexpr const char* kRoot = "/pmem0/hiom/crash_recovery";
constexpr const char* kPoolDir = "/pmem0/hiom/crash_recovery/pool";
constexpr const char* kColdFile = "/pmem0/hiom/crash_recovery/cold/cold.bin";
constexpr const char* kChkptFile = "/pmem0/hiom/crash_recovery/chkpt/chkpt.bin";
constexpr const char* kPrefix = "/pmem0/hiom/crash_recovery/";

// K8/V200 packs to a large pool; size to hold N + insert tail comfortably.
// BM_POOL_SIZE (benchmark.hpp) is the fixture default; reuse it so the VPage
// geometry / frontier arithmetic matches the main YCSB runs exactly.
constexpr std::size_t kPoolSize = viper::kv_bm::BM_POOL_SIZE;
constexpr std::size_t kHotBucketsLog2Default = 21;  // matches fixture default
constexpr std::uint64_t kCadence = 4096;            // HIOM.md §M5 default

// Encode/decode a uint64 test id into a KeyType8 / ValueType200. BMRecord's
// data.fill() spreads the low bits over every T-lane; get_key() reads lane 0.
// Values must round-trip exactly, so we compare full records.
inline KeyT make_key(std::uint64_t id) { return KeyT{id}; }
inline ValueT make_val(std::uint64_t id) { return ValueT{id}; }

struct Shared {
    static constexpr int kMaxThreads = 32;
    // Per-thread durable watermark: count of ops whose put/update RETURNED
    // before the counter was bumped => the record is durable, parent may
    // demand it back. Semantics identical to the sigkill unit test.
    std::atomic<std::uint64_t> completed[kMaxThreads];
    std::atomic<std::uint64_t> checkpoints;
    std::atomic<int> child_ready;
    std::atomic<std::uint64_t> prefill_top;  // # prefilled distinct keys
};

enum class Workload { kInsert, kYcsbA, kYcsbB };

Workload parse_workload(const char* s) {
    if (!s || std::strcmp(s, "insert") == 0) return Workload::kInsert;
    if (std::strcmp(s, "ycsb_a") == 0) return Workload::kYcsbA;
    if (std::strcmp(s, "ycsb_b") == 0) return Workload::kYcsbB;
    std::fprintf(stderr, "unknown HIOM_CR_WORKLOAD='%s'\n", s);
    std::exit(2);
}
const char* workload_name(Workload w) {
    switch (w) {
        case Workload::kInsert: return "insert";
        case Workload::kYcsbA: return "ycsb_a";
        case Workload::kYcsbB: return "ycsb_b";
    }
    return "?";
}

void guarded_cleanup() {
    if (std::string(kPoolDir).find(kPrefix) != 0) {
        std::cerr << "Refusing to clean unfamiliar path: " << kPoolDir << "\n";
        std::exit(2);
    }
    std::error_code ec;
    std::filesystem::remove_all(kRoot, ec);
    std::filesystem::create_directories(std::string(kRoot) + "/cold", ec);
    std::filesystem::create_directories(std::string(kRoot) + "/chkpt", ec);
}

std::size_t env_size(const char* name, std::size_t dflt) {
    if (const char* e = std::getenv(name)) return std::strtoull(e, nullptr, 10);
    return dflt;
}

// ---- child: prefill, checkpoint, then run workload until SIGKILLed --------

[[noreturn]] void child_run(Shared* sh, std::size_t N, int nthreads,
                            std::size_t hot_buckets, Workload wl) {
    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    const auto sz = viper::hiom::ColdTier::sizing_for(N + N / 4 + 1);
    auto cold = viper::hiom::ColdTier::create(kColdFile, sz.main_buckets,
                                              sz.overflow_slots);
    auto chkpt = viper::hiom::Checkpoint::create(kChkptFile);

    HiOMT::CheckpointConfig ccfg;
    ccfg.cadence_entries = kCadence;
    HiOMT hiom(*viper_db, hot_buckets, cold.get(),
               HiOMT::FlusherConfig{}, chkpt.get(), ccfg);

    // 1. Prefill N distinct keys [1, N], single-threaded for a deterministic
    //    keyspace, then quiesce so the whole prefill is DURABLE and the
    //    checkpoint frontier sits at the post-prefill cursor. After this the
    //    unsafe suffix is generated purely by the runtime workload below.
    {
        auto cl = hiom.get_client();
        for (std::uint64_t k = 1; k <= N; ++k) {
            if (!cl.put(make_key(k), make_val(k), /*assume_new=*/true))
                std::abort();
        }
    }
    hiom.flush_and_wait();
    hiom.force_checkpoint();
    sh->prefill_top.store(N, std::memory_order_release);

    // 2. Runtime workload. Each thread publishes its durable watermark AFTER
    //    the op returns. For inserts the value id encodes the global key so
    //    the parent can reconstruct exactly which keys were confirmed.
    std::atomic<bool> stop{false};
    std::thread watcher([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            sh->checkpoints.store(
                hiom.stats().checkpoints_written.load(std::memory_order_relaxed),
                std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::vector<std::thread> workers;
    for (int t = 0; t < nthreads; ++t) {
        workers.emplace_back([t, nthreads, N, sh, wl, &hiom] {
            auto cl = hiom.get_client();
            std::mt19937_64 rng(0xC0FFEE ^ (static_cast<std::uint64_t>(t) << 20));
            std::uniform_int_distribution<std::uint64_t> pick(1, N);
            std::uniform_real_distribution<double> coin(0.0, 1.0);
            std::uint64_t done = 0;
            // Insert keys are striped past the prefill top: key = N + 1 + t +
            // nthreads*i, so threads never collide and every confirmed insert
            // is a brand-new key the parent can enumerate from `done`.
            for (std::uint64_t i = 0;; ++i) {
                bool ok;
                if (wl == Workload::kInsert) {
                    const std::uint64_t key =
                        N + 1 + static_cast<std::uint64_t>(t) +
                        static_cast<std::uint64_t>(nthreads) * i;
                    ok = cl.put(make_key(key), make_val(key), /*assume_new=*/true);
                } else {
                    const double u = coin(rng);
                    const double upd_frac = (wl == Workload::kYcsbA) ? 0.5 : 0.05;
                    const std::uint64_t key = pick(rng);
                    if (u < upd_frac) {
                        // In-place fixed-size update: overwrite value id with a
                        // marked variant so we can assert the update landed.
                        auto fn = [key](ValueT* v) { *v = make_val(key); };
                        ok = cl.update(make_key(key), fn);
                    } else {
                        ValueT got;
                        ok = cl.get(make_key(key), &got);
                    }
                }
                if (!ok) std::abort();
                done = i + 1;
                sh->completed[t].store(done, std::memory_order_release);
            }
        });
    }
    sh->child_ready.store(1, std::memory_order_release);
    for (auto& w : workers) w.join();   // unreachable; parent SIGKILLs us
    stop.store(true);
    watcher.join();
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ---- parent: reopen, time each phase, verify, emit one CSV row -----------

struct Row {
    const char* workload;
    int iteration;
    long kill_delay_ms;
    std::uint64_t completed_reads;   // get ops confirmed (ycsb only)
    std::uint64_t completed_updates; // update ops confirmed (ycsb only)
    std::uint64_t completed_inserts; // insert ops confirmed (insert only)
    std::uint64_t checkpoint_seq;
    std::uint64_t checkpoint_version;
    std::uint64_t durable_frontier_block;
    std::uint64_t current_block;
    std::uint64_t unsafe_suffix_blocks;
    std::uint64_t scan_blocks;
    std::uint64_t recovery_replayed;
    std::uint64_t recovery_locks_cleared;
    double viper_open_ms;
    double cold_open_ms;
    double checkpoint_open_ms;
    double hiom_ctor_ms;
    double lock_scan_ms;
    double tail_scan_ms;
    double total_recovery_ms;
    std::uint64_t expected;
    std::uint64_t recovered;
    std::uint64_t lost;
    std::uint64_t mismatch;
    std::size_t cold_size_before;   // not measurable post-kill; reported -1
    std::size_t cold_size_after;
};

void csv_header(std::FILE* f) {
    std::fprintf(f,
        "workload,iteration,kill_delay_ms,completed_reads,completed_updates,"
        "completed_inserts,checkpoint_seq,checkpoint_version,"
        "durable_frontier_block,current_block,unsafe_suffix_blocks,scan_blocks,"
        "recovery_replayed,recovery_locks_cleared,viper_open_ms,cold_open_ms,"
        "checkpoint_open_ms,hiom_ctor_ms,lock_scan_ms,tail_scan_ms,"
        "total_recovery_ms,expected,recovered,lost,"
        "mismatch,cold_size_after\n");
}

void csv_row(std::FILE* f, const Row& r) {
    std::fprintf(f,
        "%s,%d,%ld,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%zu\n",
        r.workload, r.iteration, r.kill_delay_ms,
        (unsigned long long)r.completed_reads,
        (unsigned long long)r.completed_updates,
        (unsigned long long)r.completed_inserts,
        (unsigned long long)r.checkpoint_seq,
        (unsigned long long)r.checkpoint_version,
        (unsigned long long)r.durable_frontier_block,
        (unsigned long long)r.current_block,
        (unsigned long long)r.unsafe_suffix_blocks,
        (unsigned long long)r.scan_blocks,
        (unsigned long long)r.recovery_replayed,
        (unsigned long long)r.recovery_locks_cleared,
        r.viper_open_ms, r.cold_open_ms, r.checkpoint_open_ms,
        r.hiom_ctor_ms, r.lock_scan_ms, r.tail_scan_ms, r.total_recovery_ms,
        (unsigned long long)r.expected, (unsigned long long)r.recovered,
        (unsigned long long)r.lost, (unsigned long long)r.mismatch,
        r.cold_size_after);
    std::fflush(f);
}

using clk = std::chrono::steady_clock;
double ms_between(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Reopen + recover, populate timing/frontier fields of `row`. Returns the
// live HiOM so the caller can verify; also fills durable_frontier_block,
// current_block, both tail lengths.
int reopen_recover_verify(Shared* sh, std::size_t N, int nthreads,
                          std::size_t hot_buckets, Workload wl, Row& row) {
    const auto t0 = clk::now();

    // Read the durable frontier from the checkpoint BEFORE constructing HiOM
    // (the ctor's tail scan advances nothing persistent, but read it here so
    // the arithmetic uses the exact record recovery will trust).
    viper::ViperConfig vcfg;
    vcfg.skip_recovery = true;
    vcfg.cceh_init_cap = 1;
    auto viper_db = ViperT::open(kPoolDir, vcfg);
    const auto t1 = clk::now();
    auto cold = viper::hiom::ColdTier::open(kColdFile);
    const auto t2 = clk::now();
    auto chkpt = viper::hiom::Checkpoint::open(kChkptFile);
    const auto t3 = clk::now();

    // Frontier arithmetic (the two lengths the design doc separates).
    const std::uint64_t current_block =
        viper::KeyValueOffset{viper_db->hiom_vpage_frontier()}.block_number;
    std::uint64_t durable_block = 0;
    row.checkpoint_seq = 0;
    row.checkpoint_version = 0;
    if (auto rec = chkpt->read_valid()) {
        durable_block =
            viper::KeyValueOffset{rec->vpage_frontier}.block_number;
        row.checkpoint_seq = rec->seq;
        row.checkpoint_version = rec->proto_version();
    }
    row.current_block = current_block;
    row.durable_frontier_block = durable_block;
    row.unsafe_suffix_blocks =
        current_block >= durable_block ? current_block - durable_block : 0;
    const std::uint64_t scan_lo = durable_block > 0 ? durable_block - 1 : 0;
    row.scan_blocks = current_block >= scan_lo ? current_block - scan_lo : 0;

    HiOMT::RecoveryConfig rcfg;
    rcfg.tail_scan = true;
    rcfg.recovery_threads = 8;
    HiOMT hiom(*viper_db, hot_buckets, cold.get(), HiOMT::FlusherConfig{},
               chkpt.get(), HiOMT::CheckpointConfig{}, rcfg);
    const auto t4 = clk::now();

    row.viper_open_ms = ms_between(t0, t1);
    row.cold_open_ms = ms_between(t1, t2);
    row.checkpoint_open_ms = ms_between(t2, t3);
    row.hiom_ctor_ms = ms_between(t3, t4);
    row.lock_scan_ms = hiom.recovery_lock_scan_ms();
    row.tail_scan_ms = hiom.recovery_tail_scan_ms();
    row.total_recovery_ms = ms_between(t0, t4);
    row.recovery_replayed = hiom.stats().recovery_replayed.load();
    row.recovery_locks_cleared = hiom.stats().recovery_locks_cleared.load();
    row.cold_size_after = hiom.cold_tier()->approx_size();

    // ---- verify every confirmed op is recoverable --------------------------
    auto cl = hiom.get_client();
    std::uint64_t expected = 0, recovered = 0, lost = 0, mismatch = 0;
    // Forensic (Stage-1 diagnosis): remember the first lost keys so the
    // post-verify physical scan can locate each on PM and dump its evidence.
    std::vector<std::uint64_t> lost_keys;
    constexpr std::size_t kMaxLostKeys = 512;

    // (a) Prefilled keys [1, N] are always confirmed (flushed before the run).
    for (std::uint64_t k = 1; k <= N; ++k) {
        ++expected;
        ValueT got;
        if (!cl.get(make_key(k), &got)) {
            ++lost;
            if (lost_keys.size() < kMaxLostKeys) lost_keys.push_back(k);
            continue;
        }
        // For ycsb, a prefilled key may have been updated at runtime; its
        // value could be the original make_val(k) (unchanged) — updates write
        // the SAME make_val(k) marker, so any successful get with value==k is
        // correct regardless of whether it was updated.
        if (got != make_val(k)) ++mismatch; else ++recovered;
    }

    // (b) Runtime-confirmed ops.
    row.completed_reads = 0;
    row.completed_updates = 0;
    row.completed_inserts = 0;
    for (int t = 0; t < nthreads; ++t) {
        const std::uint64_t done =
            sh->completed[t].load(std::memory_order_acquire);
        if (wl == Workload::kInsert) {
            row.completed_inserts += done;
            for (std::uint64_t i = 0; i < done; ++i) {
                const std::uint64_t key =
                    N + 1 + static_cast<std::uint64_t>(t) +
                    static_cast<std::uint64_t>(nthreads) * i;
                ++expected;
                ValueT got;
                if (!cl.get(make_key(key), &got)) {
                    ++lost;
                    if (lost_keys.size() < kMaxLostKeys) lost_keys.push_back(key);
                    continue;
                }
                if (got != make_val(key)) ++mismatch; else ++recovered;
            }
        } else {
            // ycsb get/update ops target prefilled keys already verified in
            // (a); we only tally the op counts for the CSV (they don't add new
            // recoverable keys). Split reads vs updates is not reconstructable
            // per-op from the watermark alone, so report the total under reads
            // and leave updates at the fraction implied by the workload.
            if (wl == Workload::kYcsbA) {
                row.completed_updates += done / 2;
                row.completed_reads += done - done / 2;
            } else {  // ycsb_b: 5% update
                row.completed_updates += done / 20;
                row.completed_reads += done - done / 20;
            }
        }
    }
    row.expected = expected;
    row.recovered = recovered;
    row.lost = lost;
    row.mismatch = mismatch;

    // ---- Stage-1 forensic: locate every lost record on PM and prove where -----
    // it physically lives relative to the recovery ceiling. Uses ONLY existing
    // public API (the frozen core is untouched): hiom_visit_records already
    // yields a record iff its page has USED_BIT set AND its slot bit is
    // occupied (viper.hpp visit predicate) — i.e. a live, durable record — and
    // it clamps its own upper bound to the FULL preallocated pool, not the
    // recovery ceiling. So any lost key it yields is durable-on-PM; its block
    // number vs. `current_block` (the scan ceiling) and its ColdTier presence
    // classify the loss. Env HIOM_CR_FORENSIC=1 (off by default => zero cost;
    // only runs when there IS a loss, so passing rows stay free).
    if (lost > 0 && std::getenv("HIOM_CR_FORENSIC")) {
        // Scan the entire pool (huge block_hi clamps internally to
        // v_blocks_.size()) and record where each lost key physically lives.
        // A record appearing here is provably live+durable: it passed the
        // USED-page + occupied-slot predicate that recovery's own scan uses.
        //
        // Key encoding: make_key(id) fills BOTH uint32 lanes with id, so the
        // record's get_key() is (id<<32)|id, NOT id. Match on the ENCODED key
        // (make_key(id).get_key()) — comparing against raw id silently matches
        // nothing and misreports every record as not-on-PM.
        std::unordered_map<std::uint64_t, viper::KeyValueOffset> found;
        std::unordered_map<std::uint64_t, std::uint64_t> enc_to_id;
        std::unordered_set<std::uint64_t> want;
        for (std::uint64_t id : lost_keys) {
            const std::uint64_t enc = make_key(id).get_key();
            want.insert(enc);
            enc_to_id[enc] = id;
        }
        constexpr viper::block_size_t kScanAll =
            std::numeric_limits<viper::block_size_t>::max();
        viper_db->hiom_visit_records(
            0, kScanAll,
            [&](const KeyT& k, const ValueT& /*v*/, viper::KeyValueOffset off) {
                const std::uint64_t enc = k.get_key();
                if (want.count(enc)) found[enc] = off;
            });

        std::fprintf(stderr,
            "\n  ==== FORENSIC: %llu lost records (dumping <= %zu) ====\n"
            "  durable_frontier_block=%llu  scan_start_block=%llu  "
            "current_block(ceiling)=%llu\n",
            (unsigned long long)lost, lost_keys.size(),
            (unsigned long long)durable_block,
            (unsigned long long)(durable_block > 0 ? durable_block - 1 : 0),
            (unsigned long long)current_block);

        std::size_t above = 0, at_or_below = 0, not_on_pm = 0, in_cold = 0;
        std::size_t dumped = 0;
        auto* cold_raw = hiom.cold_tier();
        // Track the distinct above-ceiling blocks so we can show the loss is
        // block/page-granular, not scattered.
        std::unordered_set<std::uint64_t> above_blocks;
        for (std::uint64_t id : lost_keys) {
            const std::uint64_t enc = make_key(id).get_key();
            auto it = found.find(enc);
            if (it == found.end()) {   // not live on PM at all
                ++not_on_pm;
                continue;
            }
            const auto off = it->second;
            const auto bn = static_cast<std::uint64_t>(off.block_number);
            const bool cold_has =
                cold_raw && cold_raw->lookup(
                    viper::hiom::key_fingerprint64(make_key(id))).has_value();
            if (cold_has) ++in_cold;
            const bool is_above = bn >= current_block;
            if (is_above) { ++above; above_blocks.insert(bn); }
            else ++at_or_below;
            if (dumped < 24) {
                std::fprintf(stderr,
                    "  key=%llu @ blk=%llu pg=%u slot=%u | live-on-PM=yes | "
                    "%s ceiling(%llu) | coldTier=%s\n",
                    (unsigned long long)id, (unsigned long long)bn,
                    (unsigned)off.page_number, (unsigned)off.data_offset,
                    is_above ? "ABOVE" : "at/below",
                    (unsigned long long)current_block,
                    cold_has ? "PRESENT" : "ABSENT");
                ++dumped;
            }
        }
        std::fprintf(stderr,
            "  ---- summary: above_ceiling=%zu (in %zu distinct blocks)  "
            "at_or_below=%zu  not_live_on_pm=%zu  in_coldtier=%zu ----\n"
            "  VERDICT: %s\n\n",
            above, above_blocks.size(), at_or_below, not_on_pm, in_cold,
            (above > 0 && at_or_below == 0 && in_cold == 0)
                ? "(a) FRONTIER OVERSHOOT — every lost record is live+durable "
                  "ABOVE the recovery ceiling and absent from ColdTier; the "
                  "tail scan never reached it."
            : (at_or_below > 0)
                ? "(b) SCANNED-BUT-SKIPPED — lost records sit at/below the "
                  "ceiling; escalate to add lock/version/occupancy accessors."
                : "(c)/other — records not live+durable on PM (harness "
                  "expected-set timing or genuine non-durability).");
    }


    bool pass = true;
    if (row.checkpoint_version != viper::hiom::CheckpointRecord::kProtoDurableFrontier) {
        std::fprintf(stderr, "  GATE FAIL: checkpoint version=%llu != 2\n",
                     (unsigned long long)row.checkpoint_version);
        pass = false;
    }
    if (lost != 0 || mismatch != 0) {
        std::fprintf(stderr, "  GATE FAIL: lost=%llu mismatch=%llu\n",
                     (unsigned long long)lost, (unsigned long long)mismatch);
        pass = false;
    }
    if (recovered != expected) {
        std::fprintf(stderr, "  GATE FAIL: recovered=%llu != expected=%llu\n",
                     (unsigned long long)recovered, (unsigned long long)expected);
        pass = false;
    }
    if (current_block < durable_block) {
        std::fprintf(stderr, "  GATE FAIL: current_block=%llu < durable=%llu\n",
                     (unsigned long long)current_block,
                     (unsigned long long)durable_block);
        pass = false;
    }
    return pass ? 0 : 1;
}

// One crash cycle. Returns 0 on a clean, gate-passing recovery.
int run_one(Shared* sh, int iter, std::size_t N, int nthreads,
            std::size_t hot_buckets, Workload wl, long kill_delay_ms,
            std::FILE* csv) {
    guarded_cleanup();
    for (int t = 0; t < nthreads; ++t)
        sh->completed[t].store(0, std::memory_order_relaxed);
    sh->checkpoints.store(0, std::memory_order_relaxed);
    sh->child_ready.store(0, std::memory_order_relaxed);
    sh->prefill_top.store(0, std::memory_order_relaxed);

    const pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }
    if (pid == 0) child_run(sh, N, nthreads, hot_buckets, wl);  // [[noreturn]]

    const auto wait_for = [&](auto&& pred, int timeout_ms, const char* what) {
        const auto deadline = clk::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                std::fprintf(stderr, "  child died early (0x%x) waiting %s\n",
                             status, what);
                return false;
            }
            if (clk::now() > deadline) {
                std::fprintf(stderr, "  timeout waiting %s\n", what);
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &status, 0);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        return true;
    };

    // Wait until child finished prefill+checkpoint and started the workload.
    if (!wait_for([&] { return sh->child_ready.load(std::memory_order_acquire); },
                  600000, "child_ready (prefill+checkpoint)"))
        return 1;

    // Let the runtime workload run for kill_delay_ms, then SIGKILL — no
    // graceful path, DRAM commit buffer / hot tier are truly lost.
    std::this_thread::sleep_for(std::chrono::milliseconds(kill_delay_ms));
    if (::kill(pid, SIGKILL) != 0) { std::perror("kill"); return 1; }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) { std::perror("waitpid"); return 1; }

    Row row{};
    row.workload = workload_name(wl);
    row.iteration = iter;
    row.kill_delay_ms = kill_delay_ms;
    row.cold_size_before = static_cast<std::size_t>(-1);  // pre-kill, in child
    const int rc = reopen_recover_verify(sh, N, nthreads, hot_buckets, wl, row);
    csv_row(csv, row);
    std::printf("  iter %d [%s] delay=%ldms suffix=%llu scan=%llu replayed=%llu "
                "locks=%llu lockscan=%.2fms tail=%.1fms total=%.1fms "
                "recovered=%llu/%llu %s\n",
                iter, row.workload, kill_delay_ms,
                (unsigned long long)row.unsafe_suffix_blocks,
                (unsigned long long)row.scan_blocks,
                (unsigned long long)row.recovery_replayed,
                (unsigned long long)row.recovery_locks_cleared,
                row.lock_scan_ms, row.tail_scan_ms, row.total_recovery_ms,
                (unsigned long long)row.recovered,
                (unsigned long long)row.expected,
                rc == 0 ? "OK" : "GATE-FAIL");
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    // Config via env (keeps the smoke ladder scriptable):
    //   HIOM_CR_N          distinct prefill keys       (default 100000)
    //   HIOM_CR_THREADS    runtime writer threads      (default 4)
    //   HIOM_CR_ITERS      crash iterations            (default 3)
    //   HIOM_CR_WORKLOAD   insert|ycsb_a|ycsb_b        (default insert)
    //   HIOM_CR_MIN_MS     min kill delay              (default 5)
    //   HIOM_CR_MAX_MS     max kill delay              (default 60)
    //   HIOM_CR_SEED       RNG seed for delays         (default 12345)
    //   HIOM_HOT_BUCKETS_LOG2  hot-tier buckets log2   (default 21)
    //   HIOM_CR_CSV        output path (default results/recovery/crash_<wl>.csv)
    const std::size_t N = env_size("HIOM_CR_N", 100'000);
    const int nthreads = static_cast<int>(env_size("HIOM_CR_THREADS", 4));
    const int iters = static_cast<int>(env_size("HIOM_CR_ITERS", 3));
    const Workload wl = parse_workload(std::getenv("HIOM_CR_WORKLOAD"));
    const long min_ms = static_cast<long>(env_size("HIOM_CR_MIN_MS", 5));
    const long max_ms = static_cast<long>(env_size("HIOM_CR_MAX_MS", 60));
    const std::uint64_t seed = env_size("HIOM_CR_SEED", 12345);
    const std::size_t hot_log2 = env_size("HIOM_HOT_BUCKETS_LOG2",
                                          kHotBucketsLog2Default);
    if (hot_log2 < 10 || hot_log2 > 30) {
        std::fprintf(stderr, "HIOM_HOT_BUCKETS_LOG2 out of [10,30]\n");
        return 2;
    }
    const std::size_t hot_buckets = std::size_t(1) << hot_log2;

    if (nthreads > Shared::kMaxThreads) {
        std::fprintf(stderr, "threads > %d\n", Shared::kMaxThreads);
        return 2;
    }

    std::string csv_path;
    if (const char* e = std::getenv("HIOM_CR_CSV")) {
        csv_path = e;
    } else {
        csv_path = std::string("results/recovery/crash_") + workload_name(wl)
                   + ".csv";
    }
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(csv_path).parent_path(), ec);

    std::printf("=== HiOM crash-recovery bench (K8/V200) ===\n");
    std::printf("N=%zu threads=%d iters=%d workload=%s delay=[%ld,%ld]ms "
                "hot_buckets=2^%zu csv=%s\n",
                N, nthreads, iters, workload_name(wl), min_ms, max_ms,
                hot_log2, csv_path.c_str());

    Shared* sh = static_cast<Shared*>(
        ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (sh == MAP_FAILED) { std::perror("mmap"); return 1; }
    new (sh) Shared();

    std::FILE* csv = std::fopen(csv_path.c_str(), "w");
    if (!csv) { std::perror("fopen csv"); return 1; }
    csv_header(csv);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<long> delay(min_ms, max_ms);

    int failures = 0;
    for (int i = 0; i < iters; ++i) {
        const long d = delay(rng);
        failures += run_one(sh, i, N, nthreads, hot_buckets, wl, d, csv);
    }

    std::fclose(csv);
    std::printf("=== done: %d/%d iterations passed the correctness gate ===\n",
                iters - failures, iters);
    // Leave PM state for inspection; the NEXT run's guarded_cleanup clears it.
    return failures == 0 ? 0 : 1;
}
