// hiom_write_profile_probe — is HiOM's write-INDEX path cheaper than Viper's
// CCEH? Same rdtsc methodology as write_profile_probe, but drives a full
// HiOM<u64,V200> (Viper cceh_init_cap=1 + ColdTier + Checkpoint + flushers),
// under the SAME conditions as the Viper run (pre-fault via VIPER_ALLOC_ABLATE,
// resize off) so the comparison isolates the index.
//
// Viper's index cost (CCEH map_.Insert) was 2674 cyc/op @ INSERT t24 (pre-fault).
// HiOM replaces that with:
//   index    (viper-internal "index" phase) = the OLD-offset RESOLVER
//            (HotTier/ColdTier lookup), since hiom_owns_index_ skips CCEH
//   hot_upsert  HotTier upsert_pinned (+ back-pressure retry)
//   commit_push moodycamel push + flusher wake (+ back-pressure drain)
//   mirror_misc encode + note_client_block
// HiOM index total = index + hot_upsert + commit_push + mirror_misc.
//
// Writes ONLY under /pmem0/hiom_wprof (prefix-guarded, CLAUDE.md rule).

#define VIPER_WRITE_PROFILE
#include "viper/viper.hpp"
#include "viper/hiom/checkpoint.hpp"
#include "viper/hiom/cold_tier.hpp"
#include "viper/hiom/hiom.hpp"
#include "viper/hiom/hot_tier.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <sched.h>

namespace {

constexpr const char* kRoot  = "/pmem0/hiom_wprof";
constexpr const char* kPool  = "/pmem0/hiom_wprof/pool";
constexpr const char* kCold  = "/pmem0/hiom_wprof/cold.bin";
constexpr const char* kChkpt = "/pmem0/hiom_wprof/chkpt.bin";

struct V200 { char d[200]; };
using ViperT = viper::Viper<std::uint64_t, V200>;
using HiOMT  = viper::hiom::HiOM<std::uint64_t, V200>;

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
    if (std::string(kRoot).find("/pmem0/hiom_wprof") != 0) std::exit(2);
    std::filesystem::remove_all(kRoot);
    std::filesystem::create_directories(kRoot);
}

std::size_t gib(std::size_t b) { constexpr std::size_t G=1ULL<<30; return std::max<std::size_t>(1,(b+G-1)/G)*G; }
std::size_t ceil_log2(std::size_t n){ std::size_t r=0,v=1; while(v<n){v<<=1;++r;} return r; }
std::size_t hot_buckets_for(std::size_t N){ return 1ULL << ceil_log2(std::max<std::size_t>(N/8, 1ULL<<10)); }

using VP = viper::wprof::Phases;
using HP = viper::hiom::wprof2::Phases;

void report(const char* mode, int nt, double wall, std::size_t total_ops,
            const std::vector<VP>& vp, const std::vector<HP>& hp) {
    VP v{}; HP h{};
    for (auto& p : vp){ v.n+=p.n; v.lock+=p.lock; v.slot+=p.slot; v.alloc+=p.alloc;
        v.rec+=p.rec; v.bitmap+=p.bitmap; v.index+=p.index; v.freeold+=p.freeold; v.tail+=p.tail; }
    for (auto& p : hp){ h.hot_upsert+=p.hot_upsert; h.commit_push+=p.commit_push; h.mirror_misc+=p.mirror_misc;
        h.push_fadd_enq+=p.push_fadd_enq; h.push_wake+=p.push_wake; h.push_sync_drain+=p.push_sync_drain;
        h.resolv_hot+=p.resolv_hot; h.resolv_cold+=p.resolv_cold; h.resolv_verify+=p.resolv_verify;
        h.wake_calls+=p.wake_calls; h.sync_drains+=p.sync_drains; h.cold_neg_lookups+=p.cold_neg_lookups; }
    const double n = v.n ? (double)v.n : 1.0;
    // push_sync_drain is real wall (rare path); push_fadd_enq/push_wake and
    // resolv_* are COMPONENTS of commit_push / index(resolv) respectively —
    // exclude them from `tracked` so percentages still sum to ~100.
    const double tracked = v.lock+v.slot+v.alloc+v.rec+v.bitmap+v.index+v.freeold+v.tail
                         + h.hot_upsert+h.commit_push+h.push_sync_drain+h.mirror_misc;
    auto row=[&](const char* nm, std::uint64_t c){ printf("    %-12s %10.1f cyc/op  %5.1f%%\n", nm, c/n, 100.0*c/tracked); };
    auto sub=[&](const char* nm, std::uint64_t c){ printf("      .. %-14s %10.1f cyc/op\n", nm, c/n); };
    printf("[HiOM %s t=%d] %.2f Mops/s  (wall %.0f ns/op, n=%llu)\n",
           mode, nt, total_ops/wall/1e6, wall*1e9/total_ops, (unsigned long long)v.n);
    row("lock",v.lock); row("slot",v.slot); row("alloc",v.alloc);
    row("rec",v.rec); row("bitmap",v.bitmap);
    row("index(resolv)",v.index);
    sub("hot(DRAM)",h.resolv_hot); sub("cold(PM rd)",h.resolv_cold); sub("verify(PM rd)",h.resolv_verify);
    row("hot_upsert",h.hot_upsert); row("commit_push",h.commit_push);
    sub("fadd+enq",h.push_fadd_enq); sub("wake",h.push_wake);
    row("sync_drain",h.push_sync_drain); row("mirror_misc",h.mirror_misc);
    row("freeold",v.freeold); row("tail",v.tail);
    printf("    counters: wake_calls=%llu sync_drains=%llu cold_neg=%llu (of n=%llu)\n",
           (unsigned long long)h.wake_calls, (unsigned long long)h.sync_drains,
           (unsigned long long)h.cold_neg_lookups, (unsigned long long)v.n);
    const double hiom_index = (v.index+h.hot_upsert+h.commit_push+h.push_sync_drain+h.mirror_misc)/n;
    printf("    >> HiOM index total (resolv+hot_upsert+commit_push+sync_drain+misc) = %.1f cyc/op  vs Viper CCEH 2674 @insert-t24\n", hiom_index);
    fflush(stdout);
}

// HIOM_FLUSHERS=1|2|4|8 (must divide CommitBuffer::kNumLanes=8; default 4).
// A/B lever for the t=8 read/write-interference hypothesis: fewer flushers
// -> less concurrent ColdTier PM write pressure under the foreground
// resolver's dependent PM reads.
HiOMT::FlusherConfig fcfg() {
    HiOMT::FlusherConfig f{};
    if (const char* e = std::getenv("HIOM_FLUSHERS"); e && *e) {
        f.num_flushers = std::strtoul(e, nullptr, 10);
        printf("  (HIOM_FLUSHERS=%zu)\n", f.num_flushers);
    }
    return f;
}

void run(const char* mode, int nt, std::size_t ops, std::size_t prefill) {
    guarded_reset();
    const bool upd = std::string(mode) == "UPDATE";
    const std::size_t distinct = upd ? prefill : (std::size_t)nt*ops;
    viper::ViperConfig cfg; cfg.cceh_init_cap = 1; cfg.resize_threshold = 1e9;
    auto viper = ViperT::create(kPool, gib((distinct + (std::size_t)nt*ops)*224*7/5), cfg);
    const auto sz = viper::hiom::ColdTier::sizing_for(distinct);
    auto cold  = viper::hiom::ColdTier::create(kCold, sz.main_buckets, sz.overflow_slots);
    auto chkpt = viper::hiom::Checkpoint::create(kChkpt);
    HiOMT hiom(*viper, hot_buckets_for(distinct), cold.get(), fcfg(), chkpt.get());

    if (upd) {  // untimed prefill of `prefill` distinct keys, then drain
        auto cl = hiom.get_client();
        V200 val; std::memset(val.d, 0x11, sizeof(val.d));
        for (std::size_t i = 0; i < prefill; ++i) cl.put(i + 1, val);
        hiom.flush_and_wait();
    }

    std::atomic<int> ready{0}; std::atomic<bool> go{false};
    std::vector<VP> vp(nt); std::vector<HP> hp(nt);
    std::vector<std::thread> ts;
    for (int t = 0; t < nt; ++t) {
        ts.emplace_back([&, t]() {
            pin(t);
            auto cl = hiom.get_client();
            V200 val; std::memset(val.d, 0x22 + t, sizeof(val.d));
            std::uint64_t rng = 0x9E3779B97F4A7C15ull ^ (std::uint64_t)(t+1);
            auto nx=[&]{ rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return rng; };
            const std::uint64_t base = (std::uint64_t)t*ops + 1;
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {}
            // PoC: insert-only run can skip the old-offset resolver (new keys).
            // Gated by env ASSUME_NEW so we can A/B the resolver-skip here, in
            // the prefault (VIPER_ALLOC_ABLATE) world where the resolver is the
            // dominant write cost. UPDATE never sets it (keys pre-exist).
            const bool an = !upd && std::getenv("ASSUME_NEW")
                            && std::getenv("ASSUME_NEW")[0] != '0';
            for (std::size_t i = 0; i < ops; ++i)
                cl.put(upd ? ((nx() % prefill) + 1) : (base + i), val, an);
            vp[t] = viper::wprof::tl;
            hp[t] = viper::hiom::wprof2::tl;
        });
    }
    while (ready.load() < nt) {}
    auto w0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : ts) th.join();
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now()-w0).count();
    report(mode, nt, wall, (std::size_t)nt*ops, vp, hp);
}

}  // namespace

int main() {
    std::size_t ops = 1'000'000, prefill = 2'000'000;
    std::string mode = "both";
    if (const char* e=getenv("OPS")) ops=strtoull(e,nullptr,10);
    if (const char* e=getenv("PREFILL")) prefill=strtoull(e,nullptr,10);
    if (const char* e=getenv("MODE")) mode=e;
    printf("# hiom_write_profile_probe  K8/V200  ops/thr=%zu  prefill=%zu\n", ops, prefill);
    printf("# HiOM index = resolver(viper 'index') + hot_upsert + commit_push + mirror_misc\n\n");
    std::vector<int> threads = {1, 8, 24};
    if (mode=="insert"||mode=="both") for (int t:threads) run("INSERT", t, ops, prefill);
    if (mode=="update"||mode=="both") for (int t:threads) run("UPDATE", t, ops, prefill);
    guarded_reset();
    return 0;
}
