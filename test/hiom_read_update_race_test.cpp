// Deterministic regression for the read-vs-in-place-update visibility race.
//
// K8/V200 (fixed-size) updates are IN-PLACE: Viper's Client::update takes the
// VPage version lock, overwrites the value, and unlocks — the KVOffset never
// changes. HiOM's read path resolves key→offset via ColdTier and then calls
// hiom_read_at_offset, which returns false while the page is momentarily LOCKED
// or its version changed mid-read. The bug: resolve_slow / verify_and_read
// counted that transient false as a cold_fp_collision and FAILED the get,
// instead of retrying. Under YCSB-A (50% update) that surfaced as
// read_success_rate < 1.0 (~1 in millions) even though there is NO true fp64
// collision in the keyspace (proven offline by hiom_fp64_collision_scan).
//
// This test forces the race deterministically: N updater threads hammer
// cl.update(k) in place on a small hot keyspace while N reader threads
// cl.get(k) the SAME keys as fast as they can. Every value written is a valid
// value for its key (a monotonically increasing marker), so a correct read can
// never legitimately fail — the key is always present with a self-consistent
// value. Any get() that returns false is the race. The pre-fix code fails here
// within seconds; the fixed read path (bounded seqlock retry) must show ZERO
// read failures over millions of contended gets.
//
// PM artefacts under /pmem0/hiom/read_update_race — prefix-guarded cleanup.

#include "viper/viper.hpp"
#include "viper/hiom/checkpoint.hpp"
#include "viper/hiom/cold_tier.hpp"
#include "viper/hiom/hiom.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct BigVal {
    std::uint64_t id;
    unsigned char pad[200];
    bool operator==(const BigVal& o) const { return id == o.id; }
    bool operator!=(const BigVal& o) const { return id != o.id; }
};

using KeyT = std::uint64_t;
using ValueT = BigVal;
using ViperT = viper::Viper<KeyT, ValueT>;
using HiOMT = viper::hiom::HiOM<KeyT, ValueT>;

inline ValueT make_val(std::uint64_t id) {
    ValueT v{};
    v.id = id;
    return v;
}

constexpr const char* kRoot = "/pmem0/hiom/read_update_race";
constexpr const char* kPrefix = "/pmem0/hiom/read_update_race/";
constexpr const char* kPoolDir = "/pmem0/hiom/read_update_race/pool";
constexpr const char* kColdFile = "/pmem0/hiom/read_update_race/cold/cold.bin";
constexpr const char* kChkptFile = "/pmem0/hiom/read_update_race/chkpt/chkpt.bin";

void guarded_cleanup() {
    if (std::string(kPoolDir).find(kPrefix) != 0) {
        std::cerr << "Refusing to clean unfamiliar path\n";
        std::exit(2);
    }
    std::error_code ec;
    std::filesystem::remove_all(kRoot, ec);
    std::filesystem::create_directories(std::string(kRoot) + "/cold", ec);
    std::filesystem::create_directories(std::string(kRoot) + "/chkpt", ec);
}

}  // namespace

int main() {
    std::cout << "=== HiOM read-vs-update visibility race test ===" << std::endl;
    guarded_cleanup();

    // Small hot keyspace = maximal lock contention on the same VPages.
    const std::uint64_t K =
        (std::getenv("RU_KEYS") != nullptr)
            ? std::strtoull(std::getenv("RU_KEYS"), nullptr, 10)
            : 4096;
    const int n_upd =
        (std::getenv("RU_UPDATERS") != nullptr)
            ? std::atoi(std::getenv("RU_UPDATERS"))
            : 8;
    const int n_rd =
        (std::getenv("RU_READERS") != nullptr)
            ? std::atoi(std::getenv("RU_READERS"))
            : 8;
    const std::uint64_t reads_per_thread =
        (std::getenv("RU_READS") != nullptr)
            ? std::strtoull(std::getenv("RU_READS"), nullptr, 10)
            : 3'000'000;

    constexpr std::size_t kPoolSize = 4ull * 1024 * 1024 * 1024;  // 4 GiB
    auto viper_db = ViperT::create(kPoolDir, kPoolSize);
    const auto sz = viper::hiom::ColdTier::sizing_for(K + 1);
    auto cold = viper::hiom::ColdTier::create(kColdFile, sz.main_buckets,
                                              sz.overflow_slots);
    auto chkpt = viper::hiom::Checkpoint::create(kChkptFile);
    HiOMT hiom(*viper_db, std::size_t(1) << 20, cold.get(),
               HiOMT::FlusherConfig{}, chkpt.get(), HiOMT::CheckpointConfig{});

    // Seed the hot keyspace and flush so every key is durable in ColdTier.
    {
        auto cl = hiom.get_client();
        for (std::uint64_t k = 0; k < K; ++k) {
            if (!cl.put(k, make_val(k), /*assume_new=*/true)) {
                std::fprintf(stderr, "seed put(%llu) failed\n",
                             (unsigned long long)k);
                return 1;
            }
        }
    }
    hiom.flush_and_wait();

    std::cout << "Seeded " << K << " keys. Running " << n_upd
              << " in-place updaters + " << n_rd << " readers ("
              << reads_per_thread << " reads/thread)..." << std::endl;

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> read_failures{0};
    std::atomic<std::uint64_t> reads_done{0};

    // Updaters: forever overwrite each key in place with a fresh marker. Every
    // value make_val(k + round*K) is a valid, self-consistent value for key k
    // (its embedded id is derived from k), so a read can verify the key match
    // regardless of which round's value it observes.
    std::vector<std::thread> updaters;
    for (int t = 0; t < n_upd; ++t) {
        updaters.emplace_back([t, n_upd, K, &hiom, &stop] {
            auto cl = hiom.get_client();
            std::uint64_t round = 1;
            while (!stop.load(std::memory_order_relaxed)) {
                for (std::uint64_t k = t; k < K;
                     k += static_cast<std::uint64_t>(n_upd)) {
                    const std::uint64_t marker = k + round * K;
                    auto fn = [marker](ValueT* v) { v->id = marker; };
                    cl.update(k, fn);
                }
                ++round;
            }
        });
    }

    // Readers: get() every key repeatedly. A correct get must ALWAYS succeed —
    // the key is present and every concurrent update leaves a self-consistent
    // value. A false return is the transient-read-treated-as-failure bug.
    std::vector<std::thread> readers;
    for (int t = 0; t < n_rd; ++t) {
        readers.emplace_back([t, n_rd, K, reads_per_thread, &hiom,
                              &read_failures, &reads_done] {
            auto cl = hiom.get_client();
            std::uint64_t local_fail = 0, local_done = 0;
            for (std::uint64_t i = 0; i < reads_per_thread; ++i) {
                const std::uint64_t k = (i * 2654435761ull + t) % K;
                ValueT got;
                if (!cl.get(k, &got)) {
                    ++local_fail;
                }
                ++local_done;
            }
            read_failures.fetch_add(local_fail, std::memory_order_relaxed);
            reads_done.fetch_add(local_done, std::memory_order_relaxed);
        });
    }

    for (auto& r : readers) r.join();
    stop.store(true, std::memory_order_relaxed);
    for (auto& u : updaters) u.join();

    const std::uint64_t fails = read_failures.load();
    const std::uint64_t done = reads_done.load();
    std::cout << "Reads: " << done << "  failures: " << fails << std::endl;

    if (fails != 0) {
        std::fprintf(stderr,
            "FAIL: %llu of %llu concurrent gets returned false while the key "
            "was present — read path failed instead of retrying across a "
            "concurrent in-place update's lock window.\n",
            (unsigned long long)fails, (unsigned long long)done);
        return 1;
    }
    std::cout << "PASS: " << done << " contended gets, 0 failures — read path "
                 "rides through concurrent in-place updates." << std::endl;
    return 0;
}
