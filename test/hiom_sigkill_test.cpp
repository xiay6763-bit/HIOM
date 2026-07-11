// Real process-kill (SIGKILL) recovery test for HiOM (Claim 4D).
//
// The in-process simulate_crash_for_test() stops flushers + drops the
// commit buffer cooperatively; this test instead forks a child that
// actually writes to PM under /pmem0/hiom/sigkill_test/ and SIGKILLs it
// at a random point — no destructor, no flush, no graceful drain runs.
// The parent then reopens the PM pool (skip_recovery + tail-scan) and
// verifies that EVERY put the child had returned from is recoverable.
//
// Durability contract mirrors the integration crash-stress test: a
// child writer stores its per-thread `completed` watermark in shared
// memory only AFTER viper put() returns (⇒ the VPage record is durable),
// so the parent may demand every key in [0, completed) back. The single
// in-flight key at index `completed` may be present or absent — both are
// legal outcomes of a crash between the store and the counter update.
//
// Three crash timings are exercised (tuned via checkpoint cadence and a
// jittered delay keyed on the child-published checkpoint count):
//   (a) before the first checkpoint,
//   (b) after a checkpoint, with a commit-buffer backlog,
//   (c) during steady ColdTier bulk flushing.
//
// PM artefacts live under /pmem0/hiom/sigkill_test/ — same prefix-guarded
// cleanup rule as the rest of HiOM (see CLAUDE.md /pmem0 section).

#include "viper/viper.hpp"
#include "viper/hiom/checkpoint.hpp"
#include "viper/hiom/cold_tier.hpp"
#include "viper/hiom/hiom.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

namespace {

constexpr const char* kRoot = "/pmem0/hiom/sigkill_test";
constexpr const char* kPoolDir = "/pmem0/hiom/sigkill_test/pool";
constexpr const char* kColdPoolFile = "/pmem0/hiom/sigkill_test/cold/cold.bin";
constexpr const char* kCheckpointFile = "/pmem0/hiom/sigkill_test/chkpt/chkpt.bin";

using ViperT = viper::Viper<std::uint64_t, std::uint64_t>;
using HiOMT = viper::hiom::HiOM<std::uint64_t, std::uint64_t>;

constexpr std::size_t kPoolSize = 1ULL << 30;   // 1 GiB
constexpr int kNumThreads = 4;
constexpr int kPerThread = 400'000;             // enough to outrun the kill
constexpr std::uint64_t kCadence = 2048;        // checkpoint every 2048 flushed
constexpr std::size_t kHotBuckets = 1ULL << 12; // 4K × 16 = 64K slots

// Shared across fork via MAP_SHARED|MAP_ANONYMOUS. The child writes,
// the parent reads after the kill (pages persist in the parent).
struct Shared {
    std::atomic<std::uint64_t> completed[kNumThreads];
    std::atomic<std::uint64_t> checkpoints;  // child-published, for phasing
    std::atomic<int> child_ready;
};

std::uint64_t make_val(int t, int i) {
    return (static_cast<std::uint64_t>(t) << 32) | static_cast<std::uint64_t>(i);
}
std::uint64_t make_key(int t, int i) {
    const std::uint64_t base
        = static_cast<std::uint64_t>(t) * (kPerThread + 1) + 1;
    return base + static_cast<std::uint64_t>(i);
}

void cleanup() {
    // Prefix guard — only ever touch our own subtree (CLAUDE.md).
    if (std::string(kPoolDir).find("/pmem0/hiom/sigkill_test/") != 0) {
        std::cerr << "Refusing to clean unfamiliar path: " << kPoolDir << "\n";
        std::exit(2);
    }
    std::error_code ec;
    std::filesystem::remove_all(kRoot, ec);
    std::filesystem::create_directories(std::string(kRoot) + "/cold", ec);
    std::filesystem::create_directories(std::string(kRoot) + "/chkpt", ec);
}

// Child: create the pool, write forever (until killed), publishing the
// durable watermark + checkpoint count into shared memory. Never returns
// normally — the parent SIGKILLs it.
[[noreturn]] void child_run(Shared* sh) {
    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    const auto sz = viper::hiom::ColdTier::sizing_for(kNumThreads * kPerThread);
    auto cold = viper::hiom::ColdTier::create(kColdPoolFile,
                                              sz.main_buckets, sz.overflow_slots);
    auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);

    HiOMT::CheckpointConfig ccfg;
    ccfg.cadence_entries = kCadence;
    HiOMT hiom(*viper_db, kHotBuckets, cold.get(),
               HiOMT::FlusherConfig{}, chkpt.get(), ccfg);

    // A tiny watcher thread republishes the checkpoint count so the
    // parent can pick its crash phase without touching HiOM internals.
    std::atomic<bool> stop{false};
    std::thread watcher([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            sh->checkpoints.store(
                hiom.stats().checkpoints_written.load(std::memory_order_relaxed),
                std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::vector<std::thread> writers;
    for (int t = 0; t < kNumThreads; ++t) {
        writers.emplace_back([t, sh, &hiom] {
            auto cl = hiom.get_client();
            for (int i = 0; i < kPerThread; ++i) {
                if (!cl.put(make_key(t, i), make_val(t, i))) std::abort();
                // Durable-watermark AFTER put returns.
                sh->completed[t].store(static_cast<std::uint64_t>(i) + 1,
                                       std::memory_order_release);
            }
        });
    }
    sh->child_ready.store(1, std::memory_order_release);
    for (auto& w : writers) w.join();
    // If the child somehow finishes before the kill, keep the process
    // alive so the parent's SIGKILL still lands (and completed==full is a
    // valid state to recover).
    stop.store(true);
    watcher.join();
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// Parent: verify [0, completed) per thread are all recoverable.
int parent_verify(Shared* sh, int iter, const char* phase) {
    viper::ViperConfig vcfg;
    vcfg.skip_recovery = true;
    auto viper_db = ViperT::open(kPoolDir, vcfg);
    auto cold = viper::hiom::ColdTier::open(kColdPoolFile);
    auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
    HiOMT::RecoveryConfig rcfg;
    rcfg.tail_scan = true;
    rcfg.recovery_threads = 8;
    HiOMT hiom(*viper_db, kHotBuckets, cold.get(),
               HiOMT::FlusherConfig{}, chkpt.get(),
               HiOMT::CheckpointConfig{}, rcfg);
    const auto replayed = hiom.stats().recovery_replayed.load();

    auto cl = hiom.get_client();
    std::size_t expected = 0, recovered = 0, lost = 0, mismatch = 0;
    // Direct cold-tier probe count (diagnostic): does cold have the key
    // at all, independent of the VPage-verifying get() path?
    std::size_t cold_present = 0;
    for (int t = 0; t < kNumThreads; ++t) {
        const std::uint64_t completed
            = sh->completed[t].load(std::memory_order_acquire);
        for (std::uint64_t i = 0; i < completed; ++i) {
            ++expected;
            const std::uint64_t key = make_key(t, static_cast<int>(i));
            if (hiom.cold_tier()->lookup(viper::hiom::key_fingerprint64(key))
                    .has_value())
                ++cold_present;
            std::uint64_t got = 0;
            if (!cl.get(key, &got)) ++lost;
            else if (got != make_val(t, static_cast<int>(i))) ++mismatch;
            else ++recovered;
        }
    }
    std::printf(
        "  iter %d phase=%s: checkpoints=%llu replayed=%llu cold_size=%zu "
        "expected=%zu cold_present=%zu recovered=%zu lost=%zu mismatch=%zu\n",
        iter, phase,
        (unsigned long long)sh->checkpoints.load(),
        (unsigned long long)replayed,
        hiom.cold_tier()->approx_size(),
        expected, cold_present, recovered, lost, mismatch);
    if (lost != 0 || mismatch != 0) {
        std::fprintf(stderr, "  FAIL: lost=%zu mismatch=%zu\n", lost, mismatch);
        return 1;
    }
    return 0;
}

// One full cycle: cleanup → fork child → wait for phase → SIGKILL →
// waitpid → reopen + verify.
int run_one(Shared* sh, int iter, const char* phase,
            std::mt19937_64& rng) {
    cleanup();
    for (int t = 0; t < kNumThreads; ++t)
        sh->completed[t].store(0, std::memory_order_relaxed);
    sh->checkpoints.store(0, std::memory_order_relaxed);
    sh->child_ready.store(0, std::memory_order_relaxed);

    const pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }
    if (pid == 0) {
        child_run(sh);  // [[noreturn]]
    }

    // Bounded wait helper: polls `pred`; fails if the child dies early
    // (e.g. aborts during setup — without this the parent would spin
    // forever on a condition the dead child can never satisfy) or the
    // deadline passes.
    const auto wait_for = [&](auto&& pred, int timeout_ms,
                              const char* what) -> bool {
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            int status = 0;
            const pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                std::fprintf(stderr,
                    "  FAIL: child died early (status=0x%x) while waiting "
                    "for %s\n", status, what);
                return false;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                std::fprintf(stderr,
                    "  FAIL: timeout waiting for %s\n", what);
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &status, 0);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        return true;
    };

    // Parent: wait until the child is up and writing.
    if (!wait_for([&] {
            return sh->child_ready.load(std::memory_order_acquire) != 0;
        }, 60000, "child_ready")) return 1;

    // Pick the crash moment by phase.
    if (std::string(phase) == "pre-checkpoint") {
        // Kill after a little progress but (best-effort) before the first
        // checkpoint. cadence=2048 gives a comfortable window.
        if (!wait_for([&] {
                return sh->completed[0].load(std::memory_order_acquire) >= 200
                       || sh->checkpoints.load(std::memory_order_acquire) != 0;
            }, 60000, "pre-checkpoint progress")) return 1;
    } else if (std::string(phase) == "post-checkpoint-backlog") {
        if (!wait_for([&] {
                return sh->checkpoints.load(std::memory_order_acquire) >= 1;
            }, 60000, "first checkpoint")) return 1;
        std::uniform_int_distribution<int> d(1, 8);
        std::this_thread::sleep_for(std::chrono::milliseconds(d(rng)));
    } else {  // mid-flush
        if (!wait_for([&] {
                return sh->checkpoints.load(std::memory_order_acquire) >= 3;
            }, 60000, "third checkpoint")) return 1;
        std::uniform_int_distribution<int> d(2, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(d(rng)));
    }

    if (::kill(pid, SIGKILL) != 0) { std::perror("kill"); return 1; }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) { std::perror("waitpid"); return 1; }

    return parent_verify(sh, iter, phase);
}

// Old-pool (V0) safety: a checkpoint written by the pre-Claim-5 protocol
// stored an UNSAFE frontier (min of writer positions, not the durable
// ColdTier frontier). New recovery must NOT trust it — it must recover
// conservatively from block 0.
//
// For the test to be discriminating, ColdTier must genuinely be missing
// keys when the fake V0 frontier claims they're covered. An in-process
// simulate_crash can't arrange that: its stop+wake+join lets the flusher
// run one final unconditional drain (flusher_loop drains after every
// wake, including the stop wake), emptying the buffer. So use the real
// mechanism this test exists for: a fork()ed child writes with an inert
// flusher (huge interval + watermark => nothing drains while it runs)
// and is SIGKILLed — no graceful path, the DRAM buffer is truly lost.
// The parent then ASSERTS ColdTier is incomplete (validity gate), writes
// a hand-crafted V0 record with an absurdly HIGH frontier, and reopens:
// if recovery trusted the V0 frontier it would tail-scan almost nothing
// and the buffered keys would stay lost; ignoring it (correct) rescans
// from block 0 and recovers everything.
constexpr std::size_t kV0N = 8000;

[[noreturn]] void child_v0_run(Shared* sh) {
    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    const auto sz = viper::hiom::ColdTier::sizing_for(kV0N);
    auto cold = viper::hiom::ColdTier::create(kColdPoolFile,
                                              sz.main_buckets, sz.overflow_slots);
    auto chkpt = viper::hiom::Checkpoint::create(kCheckpointFile);
    HiOMT::FlusherConfig fcfg;
    fcfg.interval = std::chrono::milliseconds(600000);  // inert
    fcfg.high_watermark = static_cast<std::size_t>(-1) / 2;
    HiOMT::CheckpointConfig ccfg;
    ccfg.cadence_entries = 1u << 30;  // no auto-checkpoint
    HiOMT hiom(*viper_db, kHotBuckets, cold.get(), fcfg, chkpt.get(), ccfg);
    auto cl = hiom.get_client();
    for (std::size_t i = 0; i < kV0N; ++i) {
        if (!cl.put(make_key(0, static_cast<int>(i)),
                    make_val(0, static_cast<int>(i)))) std::abort();
        sh->completed[0].store(i + 1, std::memory_order_release);
    }
    // Park until the parent SIGKILLs us — never drain, never destruct.
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

int run_old_pool_v0(Shared* sh) {
    std::cout << "=== old-pool (V0 checkpoint) conservative recovery ===" << std::endl;
    cleanup();
    sh->completed[0].store(0, std::memory_order_relaxed);

    const pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }
    if (pid == 0) child_v0_run(sh);

    // Wait until every put has returned, then kill without mercy.
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(120);
    while (sh->completed[0].load(std::memory_order_acquire) < kV0N) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            std::fprintf(stderr, "  FAIL: V0 child died early (0x%x)\n", status);
            return 1;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "  FAIL: timeout waiting for V0 child\n");
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (::kill(pid, SIGKILL) != 0) { std::perror("kill"); return 1; }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) { std::perror("waitpid"); return 1; }

    // Validity gate: the killed child's buffer must have held entries
    // ColdTier never got, else this test can't distinguish trusted-vs-
    // ignored V0 frontiers.
    {
        auto cold = viper::hiom::ColdTier::open(kColdPoolFile);
        const std::size_t cold_at_crash = cold->approx_size();
        std::printf("  cold at crash: %zu / %zu\n", cold_at_crash, kV0N);
        if (cold_at_crash >= kV0N) {
            std::fprintf(stderr,
                "  FAIL (test invalid): ColdTier complete at crash — "
                "inert-flusher child leaked a drain path\n");
            return 1;
        }
        // Hand-craft a V0 record claiming an absurdly HIGH frontier. Old
        // code would trust it and tail-scan almost nothing.
        auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
        viper::hiom::CheckpointRecord rec{};
        rec.seq = 999999;
        rec.flushed_count = kV0N;
        rec.vpage_frontier = viper::KeyValueOffset{
            static_cast<viper::block_size_t>(1u << 20), 0, 0}.offset;
        rec.reserved[0] = 0;  // V0 — untrusted
        chkpt->write(rec);
    }
    // Reopen with tail-scan. V0 frontier must be ignored → recover from
    // block 0 → every key present.
    {
        viper::ViperConfig vcfg;
        vcfg.skip_recovery = true;
        auto viper_db = ViperT::open(kPoolDir, vcfg);
        auto cold = viper::hiom::ColdTier::open(kColdPoolFile);
        auto chkpt = viper::hiom::Checkpoint::open(kCheckpointFile);
        HiOMT::RecoveryConfig rcfg;
        rcfg.tail_scan = true;
        rcfg.recovery_threads = 8;
        HiOMT hiom(*viper_db, kHotBuckets, cold.get(),
                   HiOMT::FlusherConfig{}, chkpt.get(),
                   HiOMT::CheckpointConfig{}, rcfg);
        auto cl = hiom.get_client();
        std::size_t lost = 0, mismatch = 0;
        for (std::size_t i = 0; i < kV0N; ++i) {
            std::uint64_t got = 0;
            if (!cl.get(make_key(0, static_cast<int>(i)), &got)) ++lost;
            else if (got != make_val(0, static_cast<int>(i))) ++mismatch;
        }
        std::printf("  V0 frontier ignored: expected=%zu lost=%zu mismatch=%zu\n",
                    kV0N, lost, mismatch);
        if (lost != 0 || mismatch != 0) {
            std::fprintf(stderr, "  FAIL: V0 checkpoint frontier was trusted\n");
            return 1;
        }
    }
    std::cout << "  PASS" << std::endl;
    return 0;
}

}  // namespace

int main() {
    std::cout << "=== HiOM real SIGKILL recovery test ===" << std::endl;

    void* mem = ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { std::perror("mmap"); return 1; }
    auto* sh = new (mem) Shared();

    std::mt19937_64 rng(0x516b111ull);
    const char* phases[] = {"pre-checkpoint", "post-checkpoint-backlog",
                            "mid-flush"};
    int rc = 0;
    int iter = 0;
    // Two rounds per phase (6 kills total) to shake out timing-dependent
    // torn states.
    for (int round = 0; round < 2 && rc == 0; ++round) {
        for (const char* ph : phases) {
            rc |= run_one(sh, iter++, ph, rng);
            if (rc != 0) break;
        }
    }

    cleanup();
    if (rc == 0) rc |= run_old_pool_v0(sh);
    if (rc != 0) { std::cerr << "\nFAIL" << std::endl; return 1; }
    std::cout << "\nALL PASS" << std::endl;
    return 0;
}
