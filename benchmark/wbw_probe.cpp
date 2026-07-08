// wbw_probe — PM write-head throughput probe for the HiOM write-combining
// feasibility question. Measures, on /pmem0 (FSDAX, shared), how much raw
// PM-write throughput is left on the table by Viper's per-op-fence write
// model vs a group-commit (batched-fence) / write-combining model, and how
// much same-key update coalescing would save.
//
// Models (record = REC bytes, regular store + clwb unless noted):
//   A  random  + fence-per-op            (scattered small writes, worst case)
//   B  seq     + fence-per-op            (== Viper's write model, the baseline)
//   C  seq     + fence-per-BATCH         (group commit: same bytes, fewer fences)
//   D  seq NT  + fence-per-BATCH         (group commit w/ non-temporal stores:
//                                         the realistic write-combining ceiling)
//   E  coalesce R:1 (DRAM buffer, NT, batched fence)  (write-back on skew:
//                                         only 1/R logical writes reach PM)
//
// Verdict logic (printed at the end):
//   C/B, D/B   = fence-amortisation / write-combining headroom (same volume)
//   E/B        = total write-path speedup if updates coalesce R:1 (zipf case)
// If D/B ~1.5-2x => write-throughput win is physically available -> rewrite pays.
// If D/B ~1.1x   => bandwidth-saturated -> the shared FSDAX host can't give a
//                   clean write win; that's hardware, not the design.
//
// NOTE: writes ONLY to a single viper-owned file (kPath). Never rm -rf; the
// caller removes that one explicit file afterwards (CLAUDE.md /pmem0 rule).

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <immintrin.h>

static const char* kPath = "/pmem0/hiom_wbw_probe.bin";

// node0-first CPU order (from benchmark/fixtures/common_fixture.hpp CPUS):
// node0 physical 0-25, node0 SMT 52-77, node1 26-51. t<=24 stays on 0-23.
static const int kCPUS[] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,
    52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,
    26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51};

static inline void pin(int tid) {
    cpu_set_t s; CPU_ZERO(&s);
    CPU_SET(kCPUS[tid % (int)(sizeof(kCPUS)/sizeof(kCPUS[0]))], &s);
    sched_setaffinity(0, sizeof(s), &s);
}

// flush primitives matching viper::internal (clwb loop + sfence).
static inline void clwb_range(void* addr, size_t len) {
    char* p = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(addr) & ~uintptr_t(63));
    char* end = reinterpret_cast<char*>(addr) + len;
    for (; p < end; p += 64) _mm_clwb(p);
}
static inline void sfence() { _mm_sfence(); }

// regular store + clwb (no fence). Mirrors viper's put: stores then clwb.
static inline void put_clwb(char* dst, const char* src, size_t n) {
    std::memcpy(dst, src, n);
    clwb_range(dst, n);
}
// non-temporal store (no fence). n is a multiple of 16.
static inline void put_nt(char* dst, const char* src, size_t n) {
    for (size_t o = 0; o < n; o += 16) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + o));
        _mm_stream_si128(reinterpret_cast<__m128i*>(dst + o), v);
    }
}

enum class Model { A_rand_fenceop, B_seq_fenceop, C_seq_fencebatch,
                   D_seq_nt_batch, E_coalesce };

struct Cfg {
    size_t rec = 256;            // record bytes (K8+V200 ~ 4 cachelines)
    size_t ops_per_thread = 1'000'000;
    size_t region_bytes = 128ull << 20;  // per-thread PM slice
    size_t batch = 16;           // records per fence in C/D/E
    size_t coalesce_R = 8;       // E: logical:physical write ratio
};

// runs one model on `nthreads`, returns aggregate LOGICAL Mops/s.
static double run_model(Model m, int nthreads, char* base, const Cfg& c) {
    const size_t slots = c.region_bytes / c.rec;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    std::vector<double> dur(nthreads, 0.0);

    for (int t = 0; t < nthreads; ++t) {
        ts.emplace_back([&, t]() {
            pin(t);
            char* region = base + (size_t)t * c.region_bytes;
            // per-thread source + DRAM staging buffer (for E)
            std::vector<char> src(c.rec), stage(c.rec);
            std::memset(src.data(), 0xA5 + t, c.rec);
            uint64_t rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)(t + 1);
            auto next_rng = [&]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };

            // warmup: fault PTEs + warm Optane for this region (untimed)
            for (size_t i = 0; i < slots; ++i) put_nt(region + i * c.rec, src.data(), c.rec);
            sfence();

            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {}
            auto t0 = std::chrono::steady_clock::now();

            const size_t N = c.ops_per_thread;
            size_t since = 0;          // records since last fence
            switch (m) {
            case Model::A_rand_fenceop:
                for (size_t i = 0; i < N; ++i) {
                    char* dst = region + (next_rng() % slots) * c.rec;
                    put_clwb(dst, src.data(), c.rec); sfence();
                }
                break;
            case Model::B_seq_fenceop:
                for (size_t i = 0; i < N; ++i) {
                    char* dst = region + (i % slots) * c.rec;
                    put_clwb(dst, src.data(), c.rec); sfence();
                }
                break;
            case Model::C_seq_fencebatch:
                for (size_t i = 0; i < N; ++i) {
                    char* dst = region + (i % slots) * c.rec;
                    put_clwb(dst, src.data(), c.rec);
                    if (++since == c.batch) { sfence(); since = 0; }
                }
                sfence();
                break;
            case Model::D_seq_nt_batch:
                for (size_t i = 0; i < N; ++i) {
                    char* dst = region + (i % slots) * c.rec;
                    put_nt(dst, src.data(), c.rec);
                    if (++since == c.batch) { sfence(); since = 0; }
                }
                sfence();
                break;
            case Model::E_coalesce: {
                size_t w = 0;  // physical writes
                for (size_t i = 0; i < N; ++i) {
                    // every logical op pays a DRAM staging-buffer write...
                    std::memcpy(stage.data(), src.data(), c.rec);
                    // ...only 1 in R reaches PM (the coalesced flush)
                    if (i % c.coalesce_R == 0) {
                        char* dst = region + ((w++) % slots) * c.rec;
                        put_nt(dst, stage.data(), c.rec);
                        if (++since == c.batch) { sfence(); since = 0; }
                    }
                }
                sfence();
                break;
            }
            }
            auto t1 = std::chrono::steady_clock::now();
            dur[t] = std::chrono::duration<double>(t1 - t0).count();
        });
    }
    while (ready.load() < nthreads) {}
    auto w0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : ts) th.join();
    auto w1 = std::chrono::steady_clock::now();
    double wall = std::chrono::duration<double>(w1 - w0).count();
    double total_ops = (double)c.ops_per_thread * nthreads;  // LOGICAL ops
    return (total_ops / wall) / 1e6;  // Mops/s
}

static const char* name(Model m) {
    switch (m) {
    case Model::A_rand_fenceop:   return "A rand+fence/op  ";
    case Model::B_seq_fenceop:    return "B seq +fence/op  ";
    case Model::C_seq_fencebatch: return "C seq +fence/batch";
    case Model::D_seq_nt_batch:   return "D seq NT+fence/bat";
    case Model::E_coalesce:       return "E coalesce R:1   ";
    }
    return "?";
}

int main(int argc, char** argv) {
    Cfg c;
    std::vector<int> threads = {1, 8, 24};
    int reps = 3;
    if (const char* e = getenv("OPS"))      c.ops_per_thread = strtoull(e, nullptr, 10);
    if (const char* e = getenv("REC"))      c.rec = strtoull(e, nullptr, 10);
    if (const char* e = getenv("BATCH"))    c.batch = strtoull(e, nullptr, 10);
    if (const char* e = getenv("R"))        c.coalesce_R = strtoull(e, nullptr, 10);
    if (const char* e = getenv("REPS"))     reps = atoi(e);
    (void)argc; (void)argv;

    const int maxthreads = threads.back();
    const size_t file_size = (size_t)maxthreads * c.region_bytes;

    int fd = open(kPath, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    if (ftruncate(fd, file_size) != 0) { perror("ftruncate"); return 1; }
    char* base = (char*)mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }

    printf("# wbw_probe  rec=%zuB  ops/thr=%zu  region=%zuMiB/thr  batch=%zu  R=%zu  file=%.1fGiB\n",
           c.rec, c.ops_per_thread, c.region_bytes >> 20, c.batch, c.coalesce_R,
           file_size / 1073741824.0);
    printf("# throughput = aggregate LOGICAL Mops/s (median of %d reps); GB/s shown is PHYSICAL PM bytes\n", reps);
    printf("%-18s %6s %12s %10s\n", "model", "thr", "Mops/s", "phys GB/s");

    Model models[] = {Model::A_rand_fenceop, Model::B_seq_fenceop,
                      Model::C_seq_fencebatch, Model::D_seq_nt_batch,
                      Model::E_coalesce};
    // store medians: results[model][threadidx]
    double med[5][8] = {{0}};
    for (size_t mi = 0; mi < 5; ++mi) {
        for (size_t ti = 0; ti < threads.size(); ++ti) {
            int nt = threads[ti];
            std::vector<double> v;
            for (int r = 0; r < reps; ++r) v.push_back(run_model(models[mi], nt, base, c));
            std::sort(v.begin(), v.end());
            double mops = v[v.size() / 2];
            med[mi][ti] = mops;
            // physical bytes/op: E writes 1/R records, others 1.
            double phys_per_op = (models[mi] == Model::E_coalesce)
                                     ? (double)c.rec / c.coalesce_R : (double)c.rec;
            double gbps = mops * 1e6 * phys_per_op / 1e9;
            printf("%-18s %6d %12.2f %10.2f\n", name(models[mi]), nt, mops, gbps);
            fflush(stdout);
        }
    }

    // verdict
    printf("\n# === headroom vs Viper model (B = seq + fence/op) ===\n");
    printf("%-6s %10s %10s %10s %10s\n", "thr", "C/B", "D/B", "E/B", "A/B");
    for (size_t ti = 0; ti < threads.size(); ++ti) {
        double B = med[1][ti];
        printf("%-6d %10.2f %10.2f %10.2f %10.2f\n", threads[ti],
               med[2][ti] / B, med[3][ti] / B, med[4][ti] / B, med[0][ti] / B);
    }
    printf("\n# read: D/B (group-commit, same volume) >=1.5x => write win available\n");
    printf("# read: E/B (coalesce R:1) is the extra zipf-update win on top of that\n");

    munmap(base, file_size);
    close(fd);
    return 0;
}
