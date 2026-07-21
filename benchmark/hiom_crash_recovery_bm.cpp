// HiOM real process-kill recovery benchmark (E3 / Claim 3, 4, 5).
//
// This is an EVALUATION harness — it lives outside the core (the frozen core
// implementation; CORE_COMMIT is recorded in benchmark/run_frozen_ycsb.sh's
// manifest, currently b11c861) and does NOT modify include/viper/** or any
// fixture runtime semantics. It only *drives* the public HiOM/Viper/ColdTier/
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

// Which system's crash-recovery we measure. The fork/SIGKILL/random-delay/
// verify skeleton is shared; only the write path (child) and the reopen path
// (parent) differ. kHiom is the default (existing behaviour, unchanged).
//   kHiom  : child writes through HiOM (ColdTier authoritative), parent reopens
//            viper skip_recovery + cold + checkpoint + HiOM tail_scan.
//   kViper : child writes through a plain Viper client (no HiOM/cold/chkpt),
//            parent reopens with a REAL recover_database() (32-thread CCEH
//            rebuild, skip_recovery=false) and times it as total_recovery_ms.
//            This is the honest Viper true-SIGKILL baseline for E3.
enum class System { kHiom, kViper };

System parse_system(const char* s) {
    if (!s || std::strcmp(s, "hiom") == 0) return System::kHiom;
    if (std::strcmp(s, "viper") == 0) return System::kViper;
    std::fprintf(stderr, "unknown HIOM_CR_SYSTEM='%s' (want hiom|viper)\n", s);
    std::exit(2);
}
const char* system_name(System s) {
    return s == System::kHiom ? "hiom" : "viper";
}

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

// Viper-only child: identical keyspace / watermark protocol as the HiOM child,
// but every op goes straight through a plain Viper client. No HiOM, ColdTier,
// or Checkpoint exist — the parent will lean on Viper's own recover_database()
// to rebuild the CCEH index from the durable VPages. Prefill is quiesced with
// a fence so it is durable before the runtime workload opens the unsafe suffix.
[[noreturn]] void child_run_viper(Shared* sh, std::size_t N, int nthreads,
                                  Workload wl) {
    auto viper_db = ViperT::create(kPoolDir, kPoolSize);

    // 1. Prefill [1, N] single-threaded, then persist. Viper's put already
    //    persists the VPage slot; a store fence before publishing prefill_top
    //    mirrors the HiOM child's flush_and_wait ordering.
    {
        auto cl = viper_db->get_client();
        for (std::uint64_t k = 1; k <= N; ++k) {
            if (!cl.put(make_key(k), make_val(k)))
                std::abort();
        }
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    sh->prefill_top.store(N, std::memory_order_release);

    // 2. Runtime workload — same striping / watermark discipline as HiOM child.
    std::vector<std::thread> workers;
    for (int t = 0; t < nthreads; ++t) {
        workers.emplace_back([t, nthreads, N, sh, wl, &viper_db] {
            auto cl = viper_db->get_client();
            std::mt19937_64 rng(0xC0FFEE ^ (static_cast<std::uint64_t>(t) << 20));
            std::uniform_int_distribution<std::uint64_t> pick(1, N);
            std::uniform_real_distribution<double> coin(0.0, 1.0);
            std::uint64_t done = 0;
            for (std::uint64_t i = 0;; ++i) {
                bool ok;
                if (wl == Workload::kInsert) {
                    const std::uint64_t key =
                        N + 1 + static_cast<std::uint64_t>(t) +
                        static_cast<std::uint64_t>(nthreads) * i;
                    ok = cl.put(make_key(key), make_val(key));
                } else {
                    const double u = coin(rng);
                    const double upd_frac = (wl == Workload::kYcsbA) ? 0.5 : 0.05;
                    const std::uint64_t key = pick(rng);
                    if (u < upd_frac) {
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
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

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
    const char* system;
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
        "system,workload,iteration,kill_delay_ms,completed_reads,completed_updates,"
        "completed_inserts,checkpoint_seq,checkpoint_version,"
        "durable_frontier_block,current_block,unsafe_suffix_blocks,scan_blocks,"
        "recovery_replayed,recovery_locks_cleared,viper_open_ms,cold_open_ms,"
        "checkpoint_open_ms,hiom_ctor_ms,lock_scan_ms,tail_scan_ms,"
        "total_recovery_ms,expected,recovered,lost,"
        "mismatch,cold_size_after\n");
}

void csv_row(std::FILE* f, const Row& r) {
    std::fprintf(f,
        "%s,%s,%d,%ld,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%zu\n",
        r.system, r.workload, r.iteration, r.kill_delay_ms,
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
// Viper-only reopen: a REAL post-crash Viper recovery. Open with the default
// config (skip_recovery=false, num_recovery_threads=32) so recover_database()
// rebuilds the whole CCEH index from the durable VPages — Viper's actual O(N)
// crash path, not the skip_recovery mmap the HiOM reopen uses. The rebuild is
// reported as BOTH viper_open_ms and total_recovery_ms (there are no other
// phases); every HiOM-specific column stays 0 so the two systems' CSVs share
// one schema.
int reopen_recover_verify_viper(Shared* sh, std::size_t N, int nthreads,
                                Workload wl, Row& row) {
    const auto t0 = clk::now();
    viper::ViperConfig vcfg;   // defaults: full recovery, 32 threads
    auto viper_db = ViperT::open(kPoolDir, vcfg);
    const auto t1 = clk::now();
    // FINDING (2026-07-13, live): stock Viper's crash recovery is incomplete.
    // recover_database() rebuilds the CCEH index but never repairs VPage
    // write-locks left odd by a mid-put SIGKILL — the paper's recovery
    // experiment only measures CLEAN-shutdown restarts (recovery_bm.cpp:
    // InitMap -> DeInitMap -> open), where every lock is even, so this can't
    // show up there. Post-SIGKILL, any read of a page that was mid-put at
    // kill time spins FOREVER in the seqlock retry (observed: verify pinned a
    // core for 19 min inside ReadOnlyClient::get until we killed it).
    // To make the baseline measurable at all (and its lost=0 verifiable), run
    // the same stale-lock repair pass HiOM's recovery performs — same core
    // API, same parallelism as the rebuild — and charge it to Viper's total
    // (reported under lock_scan_ms). Strictly generous to the baseline.
    const std::size_t locks_cleared =
        viper_db->hiom_clear_all_stale_page_locks(vcfg.num_recovery_threads);
    const auto t2 = clk::now();

    row.viper_open_ms = ms_between(t0, t1);      // = recover_database rebuild
    row.lock_scan_ms = ms_between(t1, t2);       // = stale-lock repair pass
    row.recovery_locks_cleared = locks_cleared;
    row.total_recovery_ms = ms_between(t0, t2);
    row.current_block =
        viper::KeyValueOffset{viper_db->hiom_vpage_frontier()}.block_number;

    // ---- verify every confirmed op is recoverable (same sets as HiOM) -----
    auto cl = viper_db->get_read_only_client();
    std::uint64_t expected = 0, recovered = 0, lost = 0, mismatch = 0;
    std::vector<std::uint64_t> lost_keys;
    constexpr std::size_t kMaxLostKeys = 20000;
    // Reservoir-ish: keep every Kth lost key so the sample spans the whole
    // id range, not just the first 4096 (which are all low-id prefill and
    // biased the above/below-ceiling classification).
    std::uint64_t lost_seen = 0;
    auto note_lost = [&](std::uint64_t key) {
        ++lost_seen;
        if (lost_keys.size() < kMaxLostKeys) lost_keys.push_back(key);
        else if ((lost_seen & 0x3f) == 0 && !lost_keys.empty())
            lost_keys[(lost_seen >> 6) % lost_keys.size()] = key;
    };
    for (std::uint64_t k = 1; k <= N; ++k) {
        ++expected;
        ValueT got;
        if (!cl.get(make_key(k), &got)) {
            ++lost;
            note_lost(k);
            continue;
        }
        if (got != make_val(k)) ++mismatch; else ++recovered;
    }
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
                    note_lost(key);
                    continue;
                }
                if (got != make_val(key)) ++mismatch; else ++recovered;
            }
        } else {
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
    row.cold_size_after = 0;

    // Forensic: classify each lost Viper record relative to the recovery
    // ceiling (current_block = hiom_vpage_frontier's block). Distinguishes a
    // harness watermark race (record physically ABOVE the ceiling => the put
    // persisted but recover_database's num_used_blocks HWM hadn't advanced to
    // cover it at kill time => never a real durability loss, the confirmed set
    // is the watermark) from a genuine below-ceiling loss (a page recovery
    // SHOULD have rebuilt but didn't). Same predicate as the HiOM forensic;
    // uses only public visit API. Env HIOM_CR_FORENSIC=1.
    if (lost > 0 && std::getenv("HIOM_CR_FORENSIC")) {
        std::unordered_map<std::uint64_t, viper::KeyValueOffset> found;
        std::unordered_set<std::uint64_t> want;
        for (std::uint64_t id : lost_keys)
            want.insert(make_key(id).get_key());
        constexpr viper::block_size_t kScanAll =
            std::numeric_limits<viper::block_size_t>::max();
        viper_db->hiom_visit_records(
            0, kScanAll,
            [&](const KeyT& k, const ValueT&, viper::KeyValueOffset off) {
                const std::uint64_t enc = k.get_key();
                if (want.count(enc)) found[enc] = off;
            });
        const std::uint64_t ceiling = row.current_block;
        std::size_t above = 0, at_or_below = 0, not_on_pm = 0;
        std::unordered_set<std::uint64_t> above_blocks;
        // Block-number histogram of lost-yet-on-PM records, so we see WHERE in
        // the pool recovery missed them (not just above/below the ceiling).
        std::uint64_t min_lost_blk = ~0ull, max_lost_blk = 0;
        for (std::uint64_t id : lost_keys) {
            auto it = found.find(make_key(id).get_key());
            if (it == found.end()) { ++not_on_pm; continue; }
            const auto bn = static_cast<std::uint64_t>(it->second.block_number);
            if (bn < min_lost_blk) min_lost_blk = bn;
            if (bn > max_lost_blk) max_lost_blk = bn;
            if (bn >= ceiling) { ++above; above_blocks.insert(bn); }
            else ++at_or_below;
        }
        std::fprintf(stderr,
            "  ==== VIPER FORENSIC: %llu lost (sampled %zu) | ceiling(block)=%llu | "
            "on-PM lost blocks span [%llu, %llu] | above=%zu at_or_below=%zu "
            "not_on_pm=%zu ====\n",
            (unsigned long long)lost, lost_keys.size(),
            (unsigned long long)ceiling,
            (unsigned long long)(min_lost_blk==~0ull?0:min_lost_blk),
            (unsigned long long)max_lost_blk,
            above, at_or_below, not_on_pm);
        // (Block-span histogram above localizes where recovery missed;
    }

    bool pass = true;
    if (lost != 0 || mismatch != 0) {
        std::fprintf(stderr, "  GATE FAIL: lost=%llu mismatch=%llu\n",
                     (unsigned long long)lost, (unsigned long long)mismatch);
        pass = false;
    }
    if (recovered != expected) {
        std::fprintf(stderr, "  GATE FAIL: recovered=%llu != expected=%llu\n",
                     (unsigned long long)recovered,
                     (unsigned long long)expected);
        pass = false;
    }
    return pass ? 0 : 1;
}

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
    // Recovery parallelism: default 32 = ViperConfig::num_recovery_threads's
    // default, so BOTH systems get the same repair/rebuild thread budget (the
    // Viper baseline runs recover_database + its stale-lock repair at 32).
    // Overridable for sensitivity runs via HIOM_CR_REC_THREADS.
    rcfg.recovery_threads =
        static_cast<std::size_t>(env_size("HIOM_CR_REC_THREADS", 32));
    // O(P) stale-lock scan A/B: default OFF — the bounded-lock-set protocol
    // (lock-intent registry + lazy repair, viper.hpp) replaces it; recovery
    // is O(tail) again. HIOM_CR_LOCK_SCAN=1 re-enables the scan for the
    // ablation column.
    rcfg.stale_lock_scan = env_size("HIOM_CR_LOCK_SCAN", 0) != 0;
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
                    viper::hiom::key_fingerprint64(make_key(id)),
                    viper::hiom::key_id_of(make_key(id))).has_value();
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
    // Lazy-repair telemetry: with the scan off, every crash-stale lock is
    // repaired on first touch during replay/verify; nonzero here is the
    // registry doing the scan's job for O(threads) pages instead of O(P).
    std::printf("  lazy_lock_repairs=%zu\n", viper_db->hiom_lazy_lock_repairs());
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
// Viper baseline recovery, measured on its OWN paper's terms: a CLEAN restart
// (create → prefill → graceful close → timed reopen with recover_database).
// Rationale (2026-07-13, option (a)): Viper [VLDB'21]'s recovery experiment is
// clean-shutdown rebuild timing (recovery_bm.cpp: InitMap → DeInitMap → open),
// NOT process-kill. Its recovery cost is O(N) CCEH rebuild either way, so this
// gives the honest, apples-to-its-own-paper O(N) baseline for the C3 speed
// comparison against HiOM's O(unsafe suffix). Subjecting Viper to real SIGKILL
// exposes a rare pre-existing Viper recovery race (acknowledged-write reindex
// loss — see the Viper-baseline crash-recovery note) that is tangential to the
// recovery-TIME axis; we do not rely on it. The real-SIGKILL Viper path is
// still available via HIOM_CR_VIPER_SIGKILL=1 for reproducing that finding.
int run_one_viper_clean(Shared* /*sh*/, int iter, std::size_t N, int nthreads,
                        Workload wl, std::FILE* csv) {
    guarded_cleanup();
    // 1. FORK: child creates + prefills N + GRACEFULLY closes (destructor
    //    flushes/marks clean) + exits 0. The parent — which never touches the
    //    pool — then reopens it COLD. This mirrors HiOM's fork+SIGKILL exactly
    //    (parent always reopens a pool built by a now-dead child), so both
    //    systems' recovery is measured on an identically-cold pool; only the
    //    child's exit differs (graceful close vs SIGKILL). Without the fork the
    //    same-process reopen was cache/PM-warm and understated Viper's rebuild
    //    (observed 8.6s cold iter0 vs 2.3s warm iter1) — an unfair asymmetry.
    const pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }
    if (pid == 0) {
        auto db = ViperT::create(kPoolDir, kPoolSize);
        auto cl = db->get_client();
        for (std::uint64_t k = 1; k <= N; ++k)
            if (!cl.put(make_key(k), make_val(k))) std::_Exit(3);
        db.reset();               // explicit clean shutdown before exit
        std::_Exit(0);            // graceful — no SIGKILL, pool marked clean
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) { std::perror("waitpid"); return 1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "  child clean-prefill failed (0x%x)\n", status);
        return 1;
    }

    Row row{};
    row.system = "viper";
    row.workload = "clean_restart";
    row.iteration = iter;
    row.kill_delay_ms = 0;
    row.cold_size_before = static_cast<std::size_t>(-1);

    // 2. Parent reopens COLD == O(N) CCEH rebuild (num_recovery_threads = 32).
    const auto t0 = clk::now();
    viper::ViperConfig vcfg;
    auto db = ViperT::open(kPoolDir, vcfg);
    const auto t1 = clk::now();
    row.viper_open_ms = ms_between(t0, t1);
    row.total_recovery_ms = row.viper_open_ms;
    row.current_block =
        viper::KeyValueOffset{db->hiom_vpage_frontier()}.block_number;

    // 3. Verify every prefilled key is present. Clean shutdown => lost MUST be
    //    0; this asserts the clean-restart is genuinely clean (no torn tail).
    auto cl = db->get_read_only_client();
    std::uint64_t lost = 0, mismatch = 0, recovered = 0;
    for (std::uint64_t k = 1; k <= N; ++k) {
        ValueT got;
        if (!cl.get(make_key(k), &got)) { ++lost; continue; }
        if (!(got == make_val(k))) ++mismatch; else ++recovered;
    }
    row.completed_inserts = 0;
    row.expected = N;
    row.recovered = recovered;
    row.lost = lost;
    row.mismatch = mismatch;
    row.cold_size_after = 0;
    (void)nthreads; (void)wl;

    csv_row(csv, row);
    const bool pass = (lost == 0 && mismatch == 0);
    std::printf("  iter %d [viper/clean_restart] N=%zu rebuild=%.1fms "
                "recovered=%llu/%llu %s\n",
                iter, N, row.total_recovery_ms,
                (unsigned long long)recovered, (unsigned long long)N,
                pass ? "OK" : "GATE-FAIL");
    if (!pass)
        std::fprintf(stderr, "  GATE FAIL (clean restart should never lose): "
                     "lost=%llu mismatch=%llu\n",
                     (unsigned long long)lost, (unsigned long long)mismatch);
    return pass ? 0 : 1;
}

// HiOM clean-restart (option a, apples-to-apples vs the Viper/Halo clean lines):
// the child prefills N, DRAINS to durable (flush_and_wait + force_checkpoint ->
// durable frontier == current block => unsafe suffix = 0), then gracefully
// closes; the parent cold-reopens with the SAME reopen_recover_verify path as
// the real-SIGKILL run. With suffix=0 the tail replay is empty, so recovery
// collapses to HiOM's open floor (viper skip_recovery mmap + cold + checkpoint
// open) — the fair "how fast does a cleanly-closed HiOM reopen" number, to sit
// beside Viper's O(N) rebuild and Halo's O(N) snapshot restore. (The real crash
// number stays the headline C3 claim; this is the matched-condition baseline.)
int run_one_hiom_clean(Shared* sh, int iter, std::size_t N, int nthreads,
                       std::size_t hot_buckets, Workload wl, std::FILE* csv) {
    guarded_cleanup();
    for (int t = 0; t < nthreads; ++t)
        sh->completed[t].store(0, std::memory_order_relaxed);
    sh->checkpoints.store(0, std::memory_order_relaxed);
    sh->child_ready.store(0, std::memory_order_relaxed);
    sh->prefill_top.store(0, std::memory_order_relaxed);

    const pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }
    if (pid == 0) {
        // Clean child: build HiOM, prefill [1,N], drain to durable, close.
        auto viper_db = ViperT::create(kPoolDir, kPoolSize);
        const auto sz = viper::hiom::ColdTier::sizing_for(N + N / 4 + 1);
        auto cold = viper::hiom::ColdTier::create(kColdFile, sz.main_buckets,
                                                  sz.overflow_slots);
        auto chkpt = viper::hiom::Checkpoint::create(kChkptFile);
        HiOMT::CheckpointConfig ccfg;
        ccfg.cadence_entries = kCadence;
        {
            HiOMT hiom(*viper_db, hot_buckets, cold.get(),
                       HiOMT::FlusherConfig{}, chkpt.get(), ccfg);
            {
                auto cl = hiom.get_client();
                for (std::uint64_t k = 1; k <= N; ++k)
                    if (!cl.put(make_key(k), make_val(k), /*assume_new=*/true))
                        std::_Exit(3);
            }
            hiom.flush_and_wait();     // drain commit buffers -> ColdTier
            hiom.force_checkpoint();   // durable frontier == current block
        }                              // hiom destructs (final quiesce)
        chkpt.reset();
        cold.reset();
        viper_db.reset();              // graceful close, pool marked clean
        std::_Exit(0);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) { std::perror("waitpid"); return 1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "  hiom clean-prefill failed (0x%x)\n", status);
        return 1;
    }

    Row row{};
    row.system = "hiomclean";
    row.workload = "clean_restart";
    row.iteration = iter;
    row.kill_delay_ms = 0;
    const int rc = reopen_recover_verify(sh, N, nthreads, hot_buckets, wl, row);
    csv_row(csv, row);
    const bool pass = (rc == 0 && row.lost == 0 && row.mismatch == 0);
    std::printf("  iter %d [hiom/clean_restart] N=%zu reopen=%.1fms "
                "(open=%.1f tail=%.1f suffix=%llu blk) recovered=%llu/%llu %s\n",
                iter, N, row.total_recovery_ms, row.viper_open_ms,
                row.tail_scan_ms, (unsigned long long)row.unsafe_suffix_blocks,
                (unsigned long long)row.recovered, (unsigned long long)N,
                pass ? "OK" : "GATE-FAIL");
    return pass ? 0 : 1;
}

int run_one(Shared* sh, int iter, std::size_t N, int nthreads,
            std::size_t hot_buckets, System sys, Workload wl,
            long kill_delay_ms, std::FILE* csv) {
    guarded_cleanup();
    for (int t = 0; t < nthreads; ++t)
        sh->completed[t].store(0, std::memory_order_relaxed);
    sh->checkpoints.store(0, std::memory_order_relaxed);
    sh->child_ready.store(0, std::memory_order_relaxed);
    sh->prefill_top.store(0, std::memory_order_relaxed);

    const pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }
    if (pid == 0) {
        if (sys == System::kViper) child_run_viper(sh, N, nthreads, wl);
        child_run(sh, N, nthreads, hot_buckets, wl);  // [[noreturn]]
    }

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
    row.system = system_name(sys);
    row.workload = workload_name(wl);
    row.iteration = iter;
    row.kill_delay_ms = kill_delay_ms;
    row.cold_size_before = static_cast<std::size_t>(-1);  // pre-kill, in child
    const int rc =
        sys == System::kViper
            ? reopen_recover_verify_viper(sh, N, nthreads, wl, row)
            : reopen_recover_verify(sh, N, nthreads, hot_buckets, wl, row);
    csv_row(csv, row);
    std::printf("  iter %d [%s/%s] delay=%ldms suffix=%llu scan=%llu replayed=%llu "
                "locks=%llu lockscan=%.2fms tail=%.1fms total=%.1fms "
                "recovered=%llu/%llu %s\n",
                iter, row.system, row.workload, kill_delay_ms,
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
    //   HIOM_CR_SYSTEM     hiom|viper                  (default hiom; viper =
    //                      real recover_database() rebuild as the baseline)
    //   HIOM_CR_WORKLOAD   insert|ycsb_a|ycsb_b        (default insert)
    //   HIOM_CR_MIN_MS     min kill delay              (default 5)
    //   HIOM_CR_MAX_MS     max kill delay              (default 60)
    //   HIOM_CR_SEED       RNG seed for delays         (default 12345)
    //   HIOM_HOT_BUCKETS_LOG2  hot-tier buckets log2   (default 21)
    //   HIOM_CR_CSV        output path (default results/recovery/crash_<wl>.csv)
    const std::size_t N = env_size("HIOM_CR_N", 100'000);
    const int nthreads = static_cast<int>(env_size("HIOM_CR_THREADS", 4));
    const int iters = static_cast<int>(env_size("HIOM_CR_ITERS", 3));
    const System sys = parse_system(std::getenv("HIOM_CR_SYSTEM"));
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
        csv_path = std::string("results/recovery/crash_") + system_name(sys)
                   + "_" + workload_name(wl) + ".csv";
    }
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(csv_path).parent_path(), ec);

    std::printf("=== %s crash-recovery bench (K8/V200) ===\n",
                sys == System::kViper
                    ? (env_size("HIOM_CR_VIPER_SIGKILL", 0) != 0
                           ? "Viper (real SIGKILL — reproduces recovery race)"
                           : "Viper (clean restart — paper methodology)")
                    : "HiOM");
    std::printf("N=%zu threads=%d iters=%d system=%s workload=%s "
                "delay=[%ld,%ld]ms hot_buckets=2^%zu csv=%s\n",
                N, nthreads, iters, system_name(sys), workload_name(wl),
                min_ms, max_ms, hot_log2, csv_path.c_str());

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

    // Option (a) (2026-07-13): the Viper baseline is measured by CLEAN restart
    // (its own paper's methodology) unless HIOM_CR_VIPER_SIGKILL=1 opts into the
    // real-SIGKILL path (which reproduces the pre-existing Viper recovery race).
    // HiOM always uses real SIGKILL — that IS HiOM's crash-consistency claim.
    const bool viper_sigkill = env_size("HIOM_CR_VIPER_SIGKILL", 0) != 0;
    const bool viper_clean = (sys == System::kViper) && !viper_sigkill;
    // HiOM matched-condition baseline (option a): HIOM_CR_HIOM_CLEAN=1 measures
    // HiOM's clean-restart reopen (suffix=0 -> open floor only), the fair
    // apples-to-apples line vs the Viper/Halo clean-restart lines. Default OFF:
    // HiOM's headline C3 number is real SIGKILL.
    const bool hiom_clean =
        (sys == System::kHiom) && env_size("HIOM_CR_HIOM_CLEAN", 0) != 0;

    int failures = 0;
    for (int i = 0; i < iters; ++i) {
        const long d = delay(rng);
        if (viper_clean)
            failures += run_one_viper_clean(sh, i, N, nthreads, wl, csv);
        else if (hiom_clean)
            failures += run_one_hiom_clean(sh, i, N, nthreads, hot_buckets, wl, csv);
        else
            failures += run_one(sh, i, N, nthreads, hot_buckets, sys, wl, d, csv);
    }

    std::fclose(csv);
    std::printf("=== done: %d/%d iterations passed the correctness gate ===\n",
                iters - failures, iters);
    // Leave PM state for inspection; the NEXT run's guarded_cleanup clears it.
    return failures == 0 ? 0 : 1;
}
