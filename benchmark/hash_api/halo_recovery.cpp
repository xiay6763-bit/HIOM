// halo_recovery.cpp — clean-restart recovery-time harness for Halo (K8/V200).
//
// Emits the SAME per-sample CSV schema as hiom_crash_recovery_bm's vsn output
// (results/recovery/vsn/<system>_<N>_rep<r>.csv) so eval/recovery_vs_n_crash_plot.py
// can draw Halo as a third recovery line next to HiOM (O(unsafe suffix)) and
// Viper (O(N) rebuild).
//
// METHODOLOGY (2026-07-15, matches run_one_viper_clean in
// hiom_crash_recovery_bm.cpp — the Viper baseline's own line):
//   * The child forks, prefills N K8/V200 records through Halo, then GRACEFULLY
//     closes: ~Halo() -> MemoryManagerPool::shutdown() snapshots every CLHT
//     bucket to PM (memcpy + pmem_deep_persist) and sets ROOT->clean = 1.
//   * The parent — which never touched the pool — then COLD-reopens it. Halo's
//     ctor takes the recovery branch (SNAPSHOT && exists(PM_PATH)); because
//     ROOT->clean == 1, the redo-log replay is SKIPPED and recovery is exactly
//     startup(): restore the O(N) CLHT snapshot from PM. We time that ctor as
//     total_recovery_ms.
//   * Then verify every prefilled key is present with a matching value. Gate:
//     lost == 0 && mismatch == 0 (a clean snapshot must never lose a record).
//
// This is the rebuild class' BEST case (clean snapshot, no torn tail), so it is
// conservative w.r.t. HiOM's O(unsafe-suffix) advantage: a real crash could
// only make Halo's redo-log replay slower, never faster.
//
// Value size is 200 B (K8/V200), via Halo's VARVALUE path Halo<size_t,
// std::string>. Each value's first 8 bytes carry the key so verify is exact.
//
// Env:
//   HALO_REC_N        distinct prefill keys        (default 1000000)
//   HALO_REC_REP      rep index -> CSV iteration    (default 0)
//   HALO_REC_THREADS  prefill + verify threads      (default 16)
//   HALO_REC_CSV      output CSV path               (default results/recovery/vsn/halo_default.csv)
//   HALO_REC_PMPATH   Halo pool dir, guarded prefix (default /pmem0/halo_recovery/)
//
// Build/run: see benchmark/run_halo_recovery.sh (needs the external Halo clone;
// benchmark/hash_api/README.md).

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "Halo.hpp"

using namespace HALO;
using clk = std::chrono::steady_clock;

static constexpr std::size_t VLEN = 200;  // K8/V200 value length

static std::size_t env_size(const char* k, std::size_t d) {
  const char* e = std::getenv(k);
  return e ? std::strtoull(e, nullptr, 10) : d;
}
static std::string env_str(const char* k, const char* d) {
  const char* e = std::getenv(k);
  return e ? std::string(e) : std::string(d);
}
static double ms_between(clk::time_point a, clk::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

// A 200 B value whose first 8 bytes encode the key (rest zero) — lets verify
// check value integrity, not just presence.
static void fill_value(std::uint64_t key, char* buf) {
  std::memset(buf, 0, VLEN);
  std::memcpy(buf, &key, sizeof(key));
}

// Only ever remove_all under the guarded /pmem0/halo prefix (CLAUDE.md: /pmem0
// is a shared mount).
static void guarded_cleanup(const std::string& p) {
  if (p.rfind("/pmem0/halo", 0) != 0) {
    std::fprintf(stderr, "refusing cleanup outside /pmem0/halo: %s\n", p.c_str());
    std::exit(2);
  }
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
}

int main() {
  const std::size_t N = env_size("HALO_REC_N", 1'000'000);
  const int rep = static_cast<int>(env_size("HALO_REC_REP", 0));
  const int T = std::max<int>(1, static_cast<int>(env_size("HALO_REC_THREADS", 16)));
  const std::string csv_path =
      env_str("HALO_REC_CSV", "results/recovery/vsn/halo_default.csv");
  std::string pm = env_str("HALO_REC_PMPATH", "/pmem0/halo_recovery/");
  if (pm.empty() || pm.back() != '/') pm.push_back('/');

  std::printf("=== Halo clean-restart recovery bench (K8/V200) ===\n");
  std::printf("N=%zu rep=%d threads=%d pm=%s csv=%s\n", N, rep, T, pm.c_str(),
              csv_path.c_str());

  PM_PATH = pm;
  guarded_cleanup(pm);

  // ---- Phase 1: child prefills N through Halo, then clean-closes. ----
  const pid_t pid = fork();
  if (pid < 0) {
    std::perror("fork");
    return 1;
  }
  if (pid == 0) {
    PM_PATH = pm;  // inherited, set again for clarity
    auto* h = new Halo<std::size_t, std::string>(N);
    std::vector<std::thread> ts;
    const std::size_t part = (N + T - 1) / T;
    for (int t = 0; t < T; ++t) {
      ts.emplace_back([&, t]() {
        char val[VLEN];
        int r[1024];
        const std::size_t s = static_cast<std::size_t>(t) * part + 1;
        const std::size_t e = std::min<std::size_t>(N, static_cast<std::size_t>(t + 1) * part);
        for (std::size_t k = s; k <= e; ++k) {
          fill_value(k, val);
          Pair_t<std::size_t, std::string> p(k, val, VLEN);
          h->Insert(p, &r[k & 1023]);  // async batched (guards thread-local mmanager)
        }
        h->wait_all();  // flush this thread's thread_local write buffer -> durable
      });
    }
    for (auto& th : ts) th.join();
    delete h;      // ~Halo() -> shutdown(): snapshot CLHT + ROOT->clean = 1
    std::_Exit(0); // graceful; no atexit / double-flush
  }

  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    std::perror("waitpid");
    return 1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::fprintf(stderr, "child clean-prefill failed (status 0x%x)\n", status);
    return 1;
  }

  // ---- Phase 2: parent cold reopen == startup() O(N) snapshot restore. ----
  const auto t0 = clk::now();
  auto* h = new Halo<std::size_t, std::string>(N);
  const auto t1 = clk::now();
  const double total_ms = ms_between(t0, t1);

  // ---- Verify (NOT timed): every key present + value first-8 == key. ----
  std::atomic<std::size_t> lost{0}, mismatch{0}, recovered{0};
  {
    std::vector<std::thread> ts;
    const std::size_t part = (N + T - 1) / T;
    for (int t = 0; t < T; ++t) {
      ts.emplace_back([&, t]() {
        const std::size_t s = static_cast<std::size_t>(t) * part + 1;
        const std::size_t e = std::min<std::size_t>(N, static_cast<std::size_t>(t + 1) * part);
        std::size_t ll = 0, mm = 0, rr = 0;
        for (std::size_t k = s; k <= e; ++k) {
          Pair_t<std::size_t, std::string> p;
          p.set_key(k);
          if (!h->Get(p)) {
            ++ll;
            continue;
          }
          std::uint64_t got = 0;
          const std::string& v = p.str_value();
          if (v.size() >= sizeof(got)) std::memcpy(&got, v.data(), sizeof(got));
          if (got == k) ++rr; else ++mm;
        }
        lost += ll;
        mismatch += mm;
        recovered += rr;
      });
    }
    for (auto& th : ts) th.join();
  }

  // ---- Emit the 27-column vsn CSV row (Halo-irrelevant columns = 0). ----
  std::error_code ec;
  std::filesystem::create_directories(
      std::filesystem::path(csv_path).parent_path(), ec);
  std::FILE* csv = std::fopen(csv_path.c_str(), "w");
  if (!csv) {
    std::perror("fopen csv");
    return 1;
  }
  std::fprintf(csv,
               "system,workload,iteration,kill_delay_ms,completed_reads,"
               "completed_updates,completed_inserts,checkpoint_seq,"
               "checkpoint_version,durable_frontier_block,current_block,"
               "unsafe_suffix_blocks,scan_blocks,recovery_replayed,"
               "recovery_locks_cleared,viper_open_ms,cold_open_ms,"
               "checkpoint_open_ms,hiom_ctor_ms,lock_scan_ms,tail_scan_ms,"
               "total_recovery_ms,expected,recovered,lost,mismatch,"
               "cold_size_after\n");
  // viper_open_ms mirrors total_recovery_ms (this IS Halo's whole reopen), the
  // rest of the HiOM/Viper-specific stage columns are 0.
  std::fprintf(csv,
               "halo,clean_restart,%d,0,0,0,%zu,0,0,0,0,0,0,0,0,"
               "%.3f,0.000,0.000,0.000,0.000,0.000,%.3f,%zu,%zu,%zu,%zu,%zu\n",
               rep, N, total_ms, total_ms, N, recovered.load(), lost.load(),
               mismatch.load(), N);
  std::fclose(csv);

  const bool pass = (lost.load() == 0 && mismatch.load() == 0);
  std::printf(
      "halo N=%zu rep=%d rebuild=%.1fms recovered=%zu/%zu lost=%zu mismatch=%zu "
      "%s\n",
      N, rep, total_ms, recovered.load(), N, lost.load(), mismatch.load(),
      pass ? "OK" : "GATE-FAIL");
  if (!pass)
    std::fprintf(stderr,
                 "GATE FAIL (clean restart must not lose): lost=%zu mismatch=%zu\n",
                 lost.load(), mismatch.load());

  delete h;
  return pass ? 0 : 1;
}
