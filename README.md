<h1 align="center">HiOM: Hierarchical Offset Map for Viper</h1>
<p align="center">A DRAM-efficient tiered offset map for persistent-memory key-value stores.</p>

HiOM is a research extension of **Viper** ([VLDB '21](https://hpi.de/fileadmin/user_upload/fachgebiete/rabl/publications/2021/viper_vldb21.pdf))
that replaces Viper's all-DRAM CCEH offset index with a **tiered offset map**:

- a small, **fixed-size DRAM hot tier** — 8-byte fingerprint+offset slots under SIEVE eviction;
- an **authoritative PMem cold tier** — a 32-region linear-hashing hash table;
- **group-commit** buffering plus torn-write-safe **A/B checkpoints**, giving **O(tail)** recovery instead of Viper's O(N) index rebuild.

It targets **DRAM-constrained, read-heavy, recovery-sensitive** deployments and
is framed as a **three-axis Pareto point** — index DRAM × read throughput ×
recovery time — not an across-the-board replacement for Viper.

> 📐 Full design, invariants, and evaluation: **[design/HIOM.md](design/HIOM.md)**.

## Results

10 M records, K8/V200, Intel Optane DCPMM (FS-DAX); four-system sweep
(HiOM / Viper / Dash / CCEH). Numbers are verified from source — see HIOM.md.

| axis | HiOM | vs. baselines |
|------|------|---------------|
| **Index DRAM** (C1) | **272 MB**, flat in N | Viper / DRAM-CCEH ~2052 MB → **−87%** |
| **Read throughput** (C2) | 100r zipf, t24 **46.8 Mops/s** | ≈ Viper (43.2), **~1.5× Dash / CCEH** (~31) |
| **Recovery** (C3) | ~87 ms cold open @100M | **~25× faster** than Viper's O(N) rebuild |
| **Write** (limitation) | YCSB-A write-heavy | 0.46–0.74× Viper, 0.39–0.47× Dash/CCEH — a documented cost, not hidden |

The DRAM × read trade-off is literal: under a tight DRAM budget the all-DRAM
index camp (Viper / CCEH) is infeasible and the PM-resident camp (Dash) is slow,
while HiOM stays **both feasible and fast** — 0.9996 hot-tier hit rate at 272 MB,
which is 1/8 of Viper's index DRAM. Figures:
`eval/charts/{footprint,iso_dram,thread_scaling_*,recovery_*,hot_*}.pdf`.

## Architecture

HiOM is header-only, in [include/viper/hiom/](include/viper/hiom/):

| file | role |
|------|------|
| `hiom.hpp` | orchestrator — per-thread commit buffer, background flushers, checkpoint cadence, tail-scan recovery |
| `hot_tier.hpp` | DRAM 8-byte fingerprint+offset slots under SIEVE eviction |
| `cold_tier.hpp` | authoritative PMem hash table (32-region linear hashing) |
| `commit_buffer.hpp` | lock-free group-commit lanes |
| `checkpoint.hpp` | A/B torn-write-safe checkpoint |
| `offset_codec.hpp` | compact block/page/slot offset encoding |

When attached, HiOM **owns the write-path index** and Viper's CCEH is shrunk to a
single segment. It builds on Viper's PMem storage engine — `ViperPageBlock`s with
`clwb + sfence` persistence ([include/viper/viper.hpp](include/viper/viper.hpp)).

## Build

PMDK is expected at `/usr` (`libpmem.so` in `/usr/local/lib`); the host CPU must
support CLWB (`-mclwb`). The library is header-only; CMake + the `benchmark/` tree
exist to reproduce the experiments.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DVIPER_BUILD_BENCHMARKS=ON -DVIPER_BUILD_PLAYGROUND=ON
cmake --build build -j
```

## Benchmarks & tests

- **Recovery** (standalone stopwatch): `hiom_recovery_bm` — modes `--full` /
  `--open-only` / `--tail-sweep[-prefill]` (recovery time + O(tail) sensitivity).
- **Throughput / YCSB**: `all_ops_bm`, `ycsb_bm` via `HiOMFixture`. YCSB needs
  pre-generated binary workloads — run `benchmark/generate_ycsb.sh` first.
- **Correctness**: `hiom_integration_test` (recovery + crash injection).
- **Plots**: `eval/*.py` → `eval/charts/` (`pip install -r eval/requirements.txt`).

```bash
cmake --build build -j --target hiom_recovery_bm all_ops_bm ycsb_bm
./build/benchmark/hiom_recovery_bm --full
```

PMem artefacts live under `/pmem0/hiom*`. The benchmark sizing and
machine-specific paths are in [benchmark/benchmark.hpp](benchmark/benchmark.hpp).

## Built on Viper

HiOM extends **Viper: An Efficient Hybrid PMem-DRAM Key-Value Store** — Lawrence
Benson, Hendrik Makait, Tilmann Rabl (VLDB '21;
[paper](https://hpi.de/fileadmin/user_upload/fachgebiete/rabl/publications/2021/viper_vldb21.pdf),
[original repository](https://github.com/hpides/viper)). Viper provides the
header-only PMem-DRAM storage engine; HiOM swaps its offset index for the tiered
hot/cold design above. Viper's own header-only embedding and the `playground.cpp`
example are unchanged — see the original repository for that usage path.

## Citing

This is a research prototype; if you use it, please cite the underlying Viper paper:

```bibtex
@article{benson_viper_2021,
  author    = {Lawrence Benson and Hendrik Makait and Tilmann Rabl},
  title     = {Viper: An Efficient Hybrid PMem-DRAM Key-Value Store},
  journal   = {Proceedings of the {VLDB} Endowment},
  volume    = {14},
  number    = {9},
  year      = {2021},
  pages     = {1544--1556},
  doi       = {10.14778/3461535.3461543}
}
```

## License

MIT — © 2021 HPI Data Engineering Systems (for the Viper base). See
[LICENSE](LICENSE), which also carries the dependency licenses: **CCEH** (free
non-commercial research / educational / evaluation use, © Sungkyunkwan
University) and **concurrentqueue** (BSD, © Cameron Desrochers).
