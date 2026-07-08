// write_profile_probe — where does Viper's per-op WRITE time actually go?
//
// Instruments the REAL viper::Viper<K,V>::Client::put path (via the
// VIPER_WRITE_PROFILE rdtscp timers compiled into viper.hpp) and reports the
// per-phase breakdown for K8/V200 (the eval workload), at t=1/8/24:
//
//   lock    page version-lock acquire (incl. contention spin)
//   slot    find free slot in page (_Find_first)
//   alloc   get a new page/block when the page is full (update_access_information)
//   rec     write the (key,value) record to PM + clwb+sfence   <- the value write
//   bitmap  reset free_slots bit + clwb+sfence                 <- metadata write
//   index   CCEH map_.Insert (the suspected multi-thread bottleneck)
//   freeold free_occupied_slot for the old version (UPDATE only; another PM write)
//   tail    unlock + info_sync
//
// Answers: of the ~7x gap between Viper's insert (2.15 Mops/s) and the raw
// PM-write ceiling (15.6 Mops/s, see results/wbw/), how much is CCEH (-> lever
// d, low-contention index) vs PM record/bitmap writes (-> lever c, drop the
// 2nd persist) vs lock contention?
//
// Writes ONLY under /pmem0/hiom_wprof (prefix-guarded cleanup, CLAUDE.md rule).

#define VIPER_WRITE_PROFILE
#include "viper/viper.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sched.h>

namespace {

constexpr const char* kRoot = "/pmem0/hiom_wprof";
constexpr const char* kPool = "/pmem0/hiom_wprof/pool";

struct V200 { char d[200]; };
using ViperT = viper::Viper<std::uint64_t, V200>;

// node0-first CPU order (benchmark/fixtures/common_fixture.hpp CPUS).
const int kCPUS[] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,
    52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,
    26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51};
void pin(int tid) {
    cpu_set_t s; CPU_ZERO(&s);
    CPU_SET(kCPUS[tid % (int)(sizeof(kCPUS)/sizeof(kCPUS[0]))], &s);
    sched_setaffinity(0, sizeof(s), &s);
}

void guarded_reset() {
    if (std::string(kRoot).find("/pmem0/hiom_wprof") != 0) { std::exit(2); }
    std::filesystem::remove_all(kRoot);
    std::filesystem::create_directories(kRoot);
}

std::size_t gib_round(std::size_t bytes) {
    constexpr std::size_t G = 1ULL << 30;
    return std::max<std::size_t>(1, (bytes + G - 1) / G) * G;
}

using viper::wprof::Phases;

void report(const char* mode, int nthreads, double wall_s,
            const std::vector<Phases>& per, std::size_t total_ops) {
    Phases s{};
    for (const auto& p : per) {
        s.n+=p.n; s.lock+=p.lock; s.slot+=p.slot; s.alloc+=p.alloc;
        s.rec+=p.rec; s.bitmap+=p.bitmap; s.index+=p.index;
        s.freeold+=p.freeold; s.tail+=p.tail;
    }
    const double tracked = (double)(s.lock+s.slot+s.alloc+s.rec+s.bitmap+
                                    s.index+s.freeold+s.tail);
    const double n = (double)s.n ? (double)s.n : 1.0;
    auto row = [&](const char* nm, std::uint64_t v){
        printf("    %-8s %10.1f cyc/op  %5.1f%%\n", nm, v/n, 100.0*v/tracked);
    };
    printf("[%s t=%d] %.2f Mops/s  (wall %.0f ns/op, n=%llu)\n",
           mode, nthreads, total_ops/wall_s/1e6, wall_s*1e9/total_ops,
           (unsigned long long)s.n);
    row("lock",   s.lock);
    row("slot",   s.slot);
    row("alloc",  s.alloc);
    row("rec",    s.rec);     // value write to PM
    row("bitmap", s.bitmap);  // metadata write to PM
    row("index",  s.index);   // CCEH insert
    row("freeold",s.freeold); // old-slot free (update)
    row("tail",   s.tail);
    printf("    %-8s %10.1f cyc/op  (PM writes: rec+bitmap+freeold = %.1f%%; index = %.1f%%)\n",
           "TOTAL", tracked/n,
           100.0*(s.rec+s.bitmap+s.freeold)/tracked, 100.0*s.index/tracked);
    fflush(stdout);
}

void run_insert(int nthreads, std::size_t ops) {
    guarded_reset();
    const std::size_t pool = gib_round((std::size_t)nthreads*ops*224*7/5);
    viper::ViperConfig cfg; cfg.resize_threshold = 1e9;  // pre-mapped; no resize
    auto viper = ViperT::create(kPool, pool, cfg);

    std::atomic<int> ready{0}; std::atomic<bool> go{false};
    std::vector<Phases> per(nthreads);
    std::vector<std::thread> ts;
    for (int t = 0; t < nthreads; ++t) {
        ts.emplace_back([&, t]() {
            pin(t);
            auto cl = viper->get_client();
            V200 val; std::memset(val.d, 0x5a, sizeof(val.d));
            const std::uint64_t base = (std::uint64_t)t*ops + 1;
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {}
            for (std::size_t i = 0; i < ops; ++i) cl.put(base + i, val);
            per[t] = viper::wprof::tl;  // snapshot this thread's accumulators
        });
    }
    while (ready.load() < nthreads) {}
    auto w0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : ts) th.join();
    double wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - w0).count();
    report("INSERT", nthreads, wall, per, (std::size_t)nthreads*ops);
}

void run_update(int nthreads, std::size_t ops, std::size_t prefill) {
    guarded_reset();
    const std::size_t pool =
        gib_round((prefill + (std::size_t)nthreads*ops)*224*7/5);
    viper::ViperConfig cfg; cfg.resize_threshold = 1e9;  // pre-mapped; no resize
    auto viper = ViperT::create(kPool, pool, cfg);
    {   // single-thread prefill of `prefill` distinct keys (untimed; its
        // wprof::tl lives on this thread and is ignored)
        auto cl = viper->get_client();
        V200 val; std::memset(val.d, 0x11, sizeof(val.d));
        for (std::size_t i = 0; i < prefill; ++i) cl.put(i + 1, val);
    }

    std::atomic<int> ready{0}; std::atomic<bool> go{false};
    std::vector<Phases> per(nthreads);
    std::vector<std::thread> ts;
    for (int t = 0; t < nthreads; ++t) {
        ts.emplace_back([&, t]() {
            pin(t);
            auto cl = viper->get_client();
            V200 val; std::memset(val.d, 0x22 + t, sizeof(val.d));
            std::uint64_t rng = 0x9E3779B97F4A7C15ull ^ (std::uint64_t)(t+1);
            auto nx = [&]{ rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return rng; };
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {}
            for (std::size_t i = 0; i < ops; ++i)
                cl.put((nx() % prefill) + 1, val);  // update existing key
            per[t] = viper::wprof::tl;
        });
    }
    while (ready.load() < nthreads) {}
    auto w0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : ts) th.join();
    double wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - w0).count();
    report("UPDATE", nthreads, wall, per, (std::size_t)nthreads*ops);
}

}  // namespace

int main() {
    std::size_t ops = 1'000'000, prefill = 2'000'000;
    std::string mode = "both";
    if (const char* e = getenv("OPS")) ops = strtoull(e, nullptr, 10);
    if (const char* e = getenv("PREFILL")) prefill = strtoull(e, nullptr, 10);
    if (const char* e = getenv("MODE")) mode = e;

    printf("# write_profile_probe  K8/V200  ops/thr=%zu  prefill=%zu (update)\n",
           ops, prefill);
    printf("# phases: lock|slot|alloc|rec(value PM)|bitmap(meta PM)|index(CCEH)|freeold(PM,update)|tail\n\n");

    std::vector<int> threads = {1, 8, 24};
    if (mode == "insert" || mode == "both")
        for (int t : threads) run_insert(t, ops);
    if (mode == "update" || mode == "both")
        for (int t : threads) run_update(t, ops, prefill);

    guarded_reset();  // leave /pmem0 clean
    return 0;
}
