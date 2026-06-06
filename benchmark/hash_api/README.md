# `benchmark/hash_api/` — HiOM wrapper for the external PM-hash-index baseline comparison

This directory holds HiOM's integration into the **external** persistent-memory
hash-index benchmark used for the cross-baseline comparison in
[design/HIOM.md](../../design/HIOM.md) §7 (experiments **E2** read-throughput /
scalability and **E4** write/scalability). It is *not* part of the in-tree
Google-Benchmark suite (`all_ops_bm` / `ycsb_bm`, see `benchmark/fixtures/`);
that suite stays the source of truth for **E1** (white-box index DRAM,
`fixture_dram_bytes()`) and **E3** (recovery, `hiom_recovery_bm`), which this
harness does not measure.

## Why `hash_api`, not `pibench`

The original §7 plan said "integrate via PiBench". A hands-on check (2026-06-06)
corrected that:

- `sfu-dis/pibench` is the **range-index** benchmark (`tree_api.hpp`) — wrong
  tool for a hash index. Discarded.
- The hash-index equivalent we actually use is the **HNUSystemsLab/Halo**
  repo's **`hash_api.h` + `benchmark.cpp`** driver. Our wrapper implements that
  `hash_api.h` interface — hence this directory's name.
- "PiBench" survives only as a *workload-family label* inside that harness
  (`PiBench1..8` = synthetic uniform/zipf, alongside `ycsb*`).

## What the harness gives us (one driver, many baselines)

The Halo repo bundles, behind one `hash_api.h` selected by a `-DXXXT` define:
**HALO, CCEH, DASH, CLEVEL, SOFT, CLHT, PCLHT, and Viper** — almost the entire
§7.2 baseline set (Plush is separate, its own bindings). We add **HIOM**.

Apples-to-apples key/value here is **`uint64_t`/`uint64_t`** (the harness NONVAR
8 B path), identical to the bundled `VIPERT`. The paper's K8/V200 numbers live in
the separate `ycsb_bm` `HiOMFixture` track, not here.

## Contents

- `halo_hash_api.patch` — `git diff` against upstream Halo
  (`github.com/HNUSystemsLab/Halo` @ `9c87bfb`), reproducing all of our changes:
  - `hash_api.h`: the `#ifdef HIOMT` wrapper block (HiOM = Viper[`cceh_init_cap=1`]
    + ColdTier + Checkpoint + HiOM; client-based API mirroring `VIPERT`;
    prefix-guarded `/pmem0/hiom_eval` cleanup in the ctor), **plus host-specific
    lines** (see below).
  - `benchmark.cpp`: the two `#ifdef VIPERT` → `#if defined(VIPERT) || defined(HIOMT)`
    edits, so HIOM reuses the per-thread-client load/run loops.
  - `Makefile`: `HIOM_F` flags + the `HIOM` target.

## Reproduce

```bash
# 1. Clone the harness OUTSIDE the viper tree (keeps viper's git clean).
cd /root/hiom-baselines            # any scratch dir outside the repo
git clone --recurse-submodules https://github.com/HNUSystemsLab/Halo.git
cd Halo && git checkout 9c87bfb

# 2. Apply our changes.
git apply /root/viper/benchmark/hash_api/halo_hash_api.patch

# 3. Build PCM once, then targets (default `gcc` is 7.5 here; use g++-11).
make CXX=g++-11 HIOM            # also: VIPER CLEVEL HALO  (easy tier)
```

`HIOM_F` adds `-I/root/viper/include` (HiOM is header-only),
`-I/root/viper/build/_deps/concurrentqueue-src` (moodycamel, HiOM's only extra
dep), and `-mclwb`.

## Build tiers (verified on this host)

- **Easy — stock `libpmem`/`libpmemobj`:** `HALO`, `VIPER`, `CLEVEL`, **`HIOM`**.
  All build + run.
- **Needs the bundled custom PMDK fork:** `CCEH`, `DASH` — run `make CUSTOM_PMDK`
  first (builds `third/pmdk/src`, the `MAP_FIXED_NOREPLACE` fork). Not yet done.
- **Needs `libvmem`:** `SOFT`, `PCLHT` — `libvmem` is not installed on this host
  (deprecated, dropped from recent PMDK). Install or build before these link.

## Run

```bash
numactl -N 0 ./HIOM <workload> <threads>     # e.g. ./HIOM PiBench9 1
```

`<workload>` matching `*PiBench*` reads `PiBench/<workload>.load|.run`; matching
`*ycsb*` reads YCSB files. Workload files are plain text:
`INSERT <key> <value_len>` (load) and `INSERT/UPDATE <key> <len>` /
`READ <key>` / `REMOVE <key>` (run). Generate real ones with the bundled
`PiBench/auto_gene.sh` (synthetic) or `YCSB/` scripts. A throwaway smoke can be
hand-rolled, e.g. `awk 'BEGIN{for(i=1;i<=N;i++)print "INSERT",i,8}'`.

## Host-specific lines in the patch (adjust per machine)

- Pool path `index_pool_name = "/pmem0/hiom_eval/"` (was `/mnt/pmem/hash/`). HiOM
  artefacts: `/pmem0/hiom_eval/HIOM_{viper,cold.bin,chkpt.bin}`. **`/pmem0` is a
  shared mount** — the wrapper only ever `remove_all`s under the guarded
  `/pmem0/hiom_eval` prefix (CLAUDE.md rule); never blanket-rm.
- `pool_size = 16 GiB` (was 128 GiB). `/pmem0` had only ~134 G free; a 128 G pool
  per system on a shared mount is unsafe. Bump per-experiment as datasets grow.
- HotTier capacity defaults to `1<<21` buckets (33 M slots, ~256 MB DRAM);
  override with env `HIOM_HOT_BUCKETS_LOG2` in `[10,30]` (same contract as
  `HiOMFixture`) for the C2 capacity sweep.

## Smoke result (2026-06-06, 10 k keys, 1 thread — directional, NOT a measurement)

| target | load (write) | run (read) |
|--------|-------------:|-----------:|
| VIPER  | 1.71 Mops/s  | 6.74 Mops/s |
| HIOM   | 1.20 Mops/s (0.70×) | 8.92 Mops/s (1.32×) |

Read faster / write slower than Viper — directionally consistent with HiOM's
scoped story. The dataset is cache-resident and single-threaded, so these are
**not** representative; real E2/E4 need 1 M–100 M datasets and a thread sweep.

## Known TODO before real E2/E4

- **Read-hit verification:** the harness's run loop discards `find`'s return, so
  throughput alone doesn't prove reads hit. Add a found-count assertion (HiOM
  get-correctness itself is covered by `test/hiom_integration_test.cpp`).
- **Write fairness:** no flush between load and run yet, so HIOM load throughput
  excludes background-flusher drain. The wrapper exposes `flush()`
  (`hiom_->flush_and_wait()`); decide the fair accounting and wire it in.
