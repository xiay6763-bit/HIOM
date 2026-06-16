// cxl_latency_probe.cpp — random-access load latency of a memory backing.
//
// Serial pointer-chase over cachelines of a large buffer (>> LLC), so each
// load depends on the previous one and memory-level parallelism can't hide
// the latency. Used to quantify how well a remote-NUMA node stands in as a
// first-order latency proxy for CXL-attached memory, vs local DRAM and the
// Optane PM cold-tier backing.
//
// Backing is chosen by the caller:
//   - anonymous mmap  -> DRAM; which NUMA node it lands on is set by the outer
//                        `numactl --membind=<n>`
//   - file path arg   -> mmap that file (e.g. an FS-DAX file on /pmem0 for PM)
// Pin the measuring core with `numactl --cpunodebind=0` so we always measure
// "a node-0 core reaching <backing>".
//
// Build:  g++ -O2 -o /tmp/cxl_lat benchmark/cxl_latency_probe.cpp
// Run:    numactl --cpunodebind=0 --membind=0 /tmp/cxl_lat                 # node0 DRAM
//         numactl --cpunodebind=0 --membind=1 /tmp/cxl_lat                 # node1 remote DRAM (CXL proxy)
//         numactl --cpunodebind=0           /tmp/cxl_lat /pmem0/hiom_lat_probe.bin  # Optane PM

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char** argv) {
    const size_t kMB = 512;                 // >> LLC, so loads hit memory
    const size_t bytes = kMB << 20;
    const size_t kIters = 30'000'000;       // ~10-30 s at 300-1000 ns/load
    const char* path = (argc > 1) ? argv[1] : nullptr;

    int fd = -1;
    void* base;
    if (path) {
        fd = ::open(path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) { perror("open"); return 1; }
        if (::ftruncate(fd, bytes) != 0) { perror("ftruncate"); return 1; }
        base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    } else {
        base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    std::memset(base, 0, bytes);            // fault pages in (honours membind for anon)

    // Build a single random Hamiltonian cycle over cachelines.
    const size_t stride = 64 / sizeof(uintptr_t);
    const size_t n = bytes / 64;
    std::vector<size_t> perm(n);
    for (size_t i = 0; i < n; ++i) perm[i] = i;
    std::mt19937_64 rng(0x9E3779B97F4A7C15ULL);
    std::shuffle(perm.begin(), perm.end(), rng);

    uintptr_t* cells = static_cast<uintptr_t*>(base);
    for (size_t i = 0; i < n; ++i) {
        cells[perm[i] * stride] = reinterpret_cast<uintptr_t>(&cells[perm[(i + 1) % n] * stride]);
    }

    // Serial dependent chase.
    volatile uintptr_t* p = &cells[perm[0] * stride];
    for (size_t i = 0; i < n; ++i) p = reinterpret_cast<volatile uintptr_t*>(*p);  // warmup

    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t i = 0; i < kIters; ++i) p = reinterpret_cast<volatile uintptr_t*>(*p);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    printf("%-28s %.2f ns/load  (%zu MB, %zu iters)\n",
           path ? path : "anon-DRAM", ns / kIters, kMB, kIters);
    if (reinterpret_cast<uintptr_t>(p) == 0x1) fputs("x", stderr);  // keep chase live

    ::munmap(base, bytes);
    if (fd >= 0) ::close(fd);
    return 0;
}
