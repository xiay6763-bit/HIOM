#pragma once

// DRAM measurement helpers for benchmark fixtures.
//
// Captures resident-set-size and attributes pool-backed (PMem mmap)
// pages separately, so we can report "DRAM-only" usage for the win
// condition scaling experiment. Used by ycsb_bm to snapshot before
// and after prefill on the init thread.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

namespace viper::kv_bm {

struct MemSnapshot {
    std::uint64_t rss_kb = 0;        // process-wide VmRSS
    std::uint64_t pool_rss_kb = 0;   // sum of Rss for /pmem0/* mappings
    // HiOM-only telemetry; ViperFixture leaves these zero.
    std::uint64_t hot_size = 0;
    std::uint64_t hot_capacity = 0;
    std::uint64_t hot_evictions = 0;
    std::uint64_t hot_hits = 0;
    std::uint64_t cold_hits = 0;
};

inline std::uint64_t parse_vmrss_kb() {
    std::ifstream status{"/proc/self/status"};
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0) {
            std::uint64_t kb = 0;
            std::sscanf(line.c_str(), "VmRSS: %lu kB", &kb);
            return kb;
        }
    }
    return 0;
}

// Walks /proc/self/smaps summing the Rss field of every mapping whose
// backing path starts with pmem_prefix. Returns total kB.
//
// /proc/self/smaps interleaves a maps-style header line per region
// (hex address followed by perms, offset, device, inode, path) with
// detail lines like "Rss:    N kB". We toggle a `matching` flag on
// each header to scope subsequent Rss accumulation.
inline std::uint64_t sum_pool_rss_kb(std::string_view pmem_prefix) {
    std::ifstream smaps{"/proc/self/smaps"};
    if (!smaps.is_open()) return 0;
    std::string line;
    bool matching = false;
    std::uint64_t total = 0;
    while (std::getline(smaps, line)) {
        // Region header lines start with a hex digit (the address range).
        if (!line.empty() && std::isxdigit(static_cast<unsigned char>(line[0]))) {
            const auto path_pos = line.find_first_of('/');
            matching = (path_pos != std::string::npos) &&
                       (line.compare(path_pos, pmem_prefix.size(), pmem_prefix) == 0);
        } else if (matching && line.compare(0, 4, "Rss:") == 0) {
            std::uint64_t kb = 0;
            std::sscanf(line.c_str(), "Rss: %lu kB", &kb);
            total += kb;
        }
    }
    return total;
}

inline MemSnapshot capture_mem(std::string_view pmem_prefix = "/pmem0/") {
    MemSnapshot snap;
    snap.rss_kb = parse_vmrss_kb();
    snap.pool_rss_kb = sum_pool_rss_kb(pmem_prefix);
    return snap;
}

// Pushes counters into Google Benchmark state. Called from ycsb_run on
// the init thread after the timed loop. "dram_loaded_mb" subtracts the
// pool RSS so the value represents DRAM-only allocations (CCEH segments,
// HiOM HotTier, commit buffer, per-thread state).
inline void report_mem(benchmark::State& state,
                       const MemSnapshot& baseline,
                       const MemSnapshot& loaded) {
    constexpr double kb_to_mb = 1.0 / 1024.0;
    state.counters["rss_baseline_mb"] = baseline.rss_kb * kb_to_mb;
    state.counters["rss_loaded_mb"]   = loaded.rss_kb   * kb_to_mb;
    state.counters["pool_rss_loaded_mb"] = loaded.pool_rss_kb * kb_to_mb;
    const std::int64_t dram_loaded =
        static_cast<std::int64_t>(loaded.rss_kb) -
        static_cast<std::int64_t>(loaded.pool_rss_kb);
    state.counters["dram_loaded_mb"] = (dram_loaded > 0 ? dram_loaded : 0) * kb_to_mb;
    state.counters["hot_tier_size"]  = static_cast<double>(loaded.hot_size);
    state.counters["hot_capacity"]   = static_cast<double>(loaded.hot_capacity);
    state.counters["hot_evictions"]  = static_cast<double>(loaded.hot_evictions);
    const std::uint64_t total_hits = loaded.hot_hits + loaded.cold_hits;
    if (total_hits > 0) {
        state.counters["hot_hit_rate"] =
            static_cast<double>(loaded.hot_hits) / static_cast<double>(total_hits);
    }
}

}  // namespace viper::kv_bm
