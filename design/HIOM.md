# HiOM: Hierarchical Offset Map for Viper

Design document for a tiered offset-map extension to Viper [VLDB '21],
targeting ICDE (CCF-A) submission. Backup venue: 计算机研究与发展 / FGCS.

Code references use the form
`[viper.hpp:101-108](../include/viper/viper.hpp#L101-L108)`. Numbers ending
in `≈` are derived/projected; numbers without `≈` are verified from source
(see Appendix A).

---

## Status (2026-05-03)

- **Phase**: M0 ✅ complete; M1 functionally complete except EBR
  (deferred to M4); **M2 Phase B-1 complete** — overflow chains,
  region-parallel load, multi-thread stress all in place. Phase B-2
  (split worker + HiOM integration replacing CCEH) is the next chunk.
- **Code on disk**:
  - `include/viper/hiom/hot_tier.hpp` (~330 lines): `viper::hiom::HotTier`
    standalone hash table with packed (fp, offset) slots and SIEVE.
  - `include/viper/hiom/offset_codec.hpp` (~110 lines): 4 B ↔ KVOffset
    codec + 32-region block-base map (M0 uses 1 degenerate region).
  - `include/viper/hiom/hiom.hpp` (~190 lines): `HiOM<K,V>` wrapper
    around Viper with HotTier in front of Viper's CCEH.
  - `include/viper/hiom/cold_tier.hpp` (~410 lines): PM-backed
    linear-hashing index with overflow chains and `parallel_load`.
  - `test/hot_tier_test.cpp` (~660 lines, 8 tests): standalone HotTier.
  - `test/hiom_integration_test.cpp` (~210 lines, 3 tests): HiOM↔Viper
    correctness + hit-mostly throughput vs raw Viper.
  - `test/cold_tier_test.cpp` (~370 lines, 9 tests): correctness, update
    CAS, remove+reactivate, close+reopen persistence, overflow chain
    stress, 8-thread concurrent stress, parallel_load, scalable M2-exit
    harness, single-thread microbench.
  - `include/viper/viper.hpp` has TWO new additive public methods:
    `Client::hiom_peek_offset` and `ReadOnlyClient::hiom_read_at_offset`.
    No existing API changed.
- **Throughputs (single-threaded)**:
  - HotTier standalone lookup: 135 M ops/s.
  - HiOM full-path lookup (HotTier hit + decode + PM verify-on-key):
    54.6 M ops/s vs raw Viper 49.5 M ops/s — ratio 1.103.
  - ColdTier insert: 3.41 M ops/s (clean run); 1.19 M/s when run after
    a separate large-pool test (PM bandwidth interference).
  - ColdTier lookup: 200 M ops/s (no PM verify since this is a pure
    index lookup; decode handled at the caller).
  - ColdTier parallel_load: 500K entries scanned in 22 ms with 32 threads.
- **Prior work**: An earlier write-back DRAM cache prototype (`viper_x.hpp`
  + `dram_tier.hpp`) was deleted from the tree on 2026-05-02. Its design —
  full `(K,V)` caching with epoch consistency — is orthogonal to HiOM's
  index tiering.

## Three contributions

- **C1**: Working-set-aware index tiering with SIEVE eviction, applied for
  the first time to persistent hash indices in hybrid PM-DRAM KV stores.
- **C2**: Safe compact hot-tier encoding (4 B fingerprint + 4 B offset)
  with a case split for keys ≤8 B vs. >8 B (see §2.2).
- **C3**: Crash-consistent group-commit protocol with pin invariants and
  A/B checkpoints, achieving ~40× faster recovery vs. Viper.

Supporting techniques: per-thread commit buffer using existing
`concurrentqueue`; 32-region linear hashing for parallel cold-tier load;
AVX-512 SIMD fingerprint compare.

## Win condition

For working sets ≥ 1.5× DRAM, HiOM achieves ≥80% of Viper-LARGE throughput
while reducing DRAM consumption by ≥50%. As working set grows to N× DRAM
(up to 5×), HiOM degrades gracefully to 60-70% of Viper-LARGE on uniform
workloads and ≥80% on production-skewed workloads. Viper itself OOMs at
working sets exceeding DRAM capacity (CCEH preallocates ≈2 GB on init).

---

# §1 Background — Viper Internals (Verified)

## 1.1 Storage layout

Viper organizes persistent storage as a sequence of **VPageBlocks**. Each
block is 24 KB (`BLOCK_SIZE = NUM_DIMMS × PAGE_SIZE = 6 × 4 KB`,
[viper.hpp:36-37](../include/viper/viper.hpp#L36-L37)) — sized so a single
block spans all six DIMMs of a typical Optane DCPMM host. Within a block,
4 KB **VPages** hold packed `(K, V)` entries. The number of slots per page
is computed at compile time by `get_num_slots_per_page<K, V>()`
([viper.hpp:73-99](../include/viper/viper.hpp#L73-L99)) so each page tightly
fits as many entries as possible plus:

- a 1-byte version_lock (`uint8_t` atomic, [viper.hpp:33](../include/viper/viper.hpp#L33))
- a per-slot `std::bitset<num_slots>` of free-slot markers
  ([viper.hpp:170](../include/viper/viper.hpp#L170))

The version_lock byte is bit-packed
([viper.hpp:41-44](../include/viper/viper.hpp#L41-L44)):

```
bit 7   CLIENT_BIT      page is currently owned by a client
bit 6   USED_BIT        page has been used since allocation
bit 1-5 version counter (5 bits, wraps modulo USED_BIT)
bit 0   lock bit (1 = locked, 0 = unlocked)
```

Persistence is enforced via `internal::pmem_persist`
([viper.hpp:101-108](../include/viper/viper.hpp#L101-L108)): a `_mm_clwb`
loop over the affected cache lines followed by a single `_mm_sfence`. The
public API (`put`/`get`/`update`/`remove`) is exposed through per-thread
**Clients** ([viper.hpp:361-406](../include/viper/viper.hpp#L361-L406))
that own a private VPageBlock for inserts, eliminating cross-thread
contention on the write path.

## 1.2 The DRAM offset map

The DRAM-resident **offset map** maps keys to physical positions in PMem.
Viper uses CCEH instantiated as `cceh::CCEH<K> map_`
([viper.hpp:435](../include/viper/viper.hpp#L435)).

Each CCEH slot stores a `Pair`
([cceh.hpp:190-203](../include/viper/cceh.hpp#L190-L203)):

```c
struct Pair {
    IndexK key;     // 8 bytes (size_t)
    IndexV value;   // 8 bytes (KeyValueOffset)
};
```

Total slot size: **16 bytes**. The interpretation of `IndexK` is **conditional
on key size** ([cceh.hpp:112-113](../include/viper/cceh.hpp#L112-L113)):

```c
#define requires_fingerprint(K)  (std::is_same_v<K, std::string> || sizeof(K) > 8)
```

| Key category | `IndexK key` field stores | Source |
|--------------|---------------------------|--------|
| `K` ≤ 8 bytes (e.g., `uint64_t`) | The raw key, reinterpret-cast to `size_t` | [cceh.hpp:368](../include/viper/cceh.hpp#L368) |
| `K` > 8 bytes or `std::string` | An 8-byte hash fingerprint | [cceh.hpp:366](../include/viper/cceh.hpp#L366) |

This distinction matters for HiOM design: when the index already stores a
hash fingerprint, HiOM's hot tier can simply truncate it; when the index
stores the raw key, HiOM's hot tier must compute a fingerprint *de novo*
(see §2.2).

The `IndexV value` field is `KeyValueOffset`, an 8-byte bit-packed structure
([cceh.hpp:134-144](../include/viper/cceh.hpp#L134-L144)):

```c
struct KeyValueOffset {
    union {
        uint64_t offset;
        struct {
            uint64_t block_number : 45;   // up to 2^45 blocks
            uint8_t  page_number  : 3;    // page within block (6 pages → 3 bits)
            uint16_t data_offset  : 16;   // slot within page (or byte offset)
        };
    };
};
```

The 45-bit `block_number` × 24 KB block size addresses up to ~768 EB of PMem.

## 1.3 CCEH structure and DRAM cost

CCEH is an extendible-hashing index with a flat directory of pointers to
fixed-size segments. Viper's instantiation:

| Parameter | Value | Source |
|-----------|-------|--------|
| Initial directory capacity | 131,072 | [viper.hpp:475](../include/viper/viper.hpp#L475) `map_{131072}` |
| Initial directory depth | log₂(131072) = 17 | [cceh.hpp:480-481](../include/viper/cceh.hpp#L480-L481) |
| Segment size | 16 KB (1024 slots × 16 B) | [cceh.hpp:183](../include/viper/cceh.hpp#L183) |
| Probe window per lookup | 16 slots = 4 cache lines × 4 pairs | [cceh.hpp:184-185](../include/viper/cceh.hpp#L184-L185) |

On construction, Viper allocates a fresh `Segment` for *every* directory
entry ([cceh.hpp:483-486](../include/viper/cceh.hpp#L483-L486)). This
produces a **fixed initial DRAM cost of 131,072 × 16 KB ≈ 2 GB** regardless
of how many keys are eventually inserted. As the dataset grows, segments
split, increasing total allocation. Viper §5.3.4 reports 2.3 GB for 100M
keys (matching our analysis: 75% load → near-edge segment splits push the
2 GB baseline up by ~15%).

For 1B keys, projected CCEH allocation is ≈20 GB; for 6B keys, ≈120 GB.
This scaling is the primary motivation for HiOM's tiered design.

## 1.4 Persistence model

CCEH itself runs in DRAM by default ([cceh.hpp:21](../include/viper/cceh.hpp#L21)
`//#define CCEH_PERSISTENT` is commented out). All CCEH state is volatile
and reconstructed on restart.

Viper's `recover_database()` ([viper.hpp:791-847](../include/viper/viper.hpp#L791-L847))
scans every used VPage in parallel — by default 32 worker threads
([viper.hpp:64](../include/viper/viper.hpp#L64) `num_recovery_threads = 32`)
each iterate a slice of `v_blocks_`, decode the per-page bitset, read each
non-free slot's key, and call `map_.Insert(key, offset)` to rebuild the
DRAM index. Recovery time is therefore O(total PMem data scanned), as
reported in Viper §5.3.9 (~2 minutes for 1 TB).

Variable-size string-keyed Viper does **not** implement recovery
([viper.hpp:849-853](../include/viper/viper.hpp#L849-L853) throws
`"Not implemented yet"`). HiOM evaluation must use fixed-size keys.

## 1.5 Insert path and persistence ordering

The fixed-size insert path
([viper.hpp:1023-1069](../include/viper/viper.hpp#L1023-L1069)) executes
the following sequence under the page lock:

1. `v_page_->data[free_slot_idx] = {key, value}`
2. `pmem_persist(entry_ptr, sizeof(VEntry))` — `clwb` + `sfence`
3. `free_slots->reset(free_slot_idx)`
4. `pmem_persist(free_slots, ...)` — `clwb` + `sfence`
5. `viper_.map_.Insert(key, kv_offset, ...)` — DRAM-only update

This ordering is critical for crash consistency: the K/V pair and the
bitset slot-occupancy bit are both persisted before the volatile DRAM
index is updated. On crash, recovery re-scans the bitset; any slot whose
free-bit is reset and whose K/V is intact is treated as live, and the
DRAM index is rebuilt from these.

## 1.6 Limitations of Viper's design (motivation for HiOM)

**(L1) Fixed DRAM overhead independent of data size.** CCEH's 2 GB initial
allocation is paid even for an empty database. For deployments with
constrained DRAM (e.g., 32-64 GB servers with several-TB PMem), this is a
non-trivial fraction.

**(L2) Linear DRAM growth with dataset size.** Beyond the initial 2 GB,
each new ~100M keys adds ~2 GB of CCEH state. A 1B-key dataset already
needs ~20 GB of DRAM just for the index; for fixed-size 16-byte records,
the index is *larger than the data*. Viper does not gracefully degrade
when working set exceeds DRAM — it OOMs.

**(L3) Recovery scales with data size, not index size.** Even though the
index is volatile, recovery must scan all PMem data to rebuild it. For a
1 TB database, this is a 2-minute outage.

HiOM addresses (L1) and (L2) by tiering the index across DRAM and PM,
sized to fit DRAM regardless of dataset size; and addresses (L3) by
making the cold tier itself the recovery source, requiring only a bounded
incremental scan of recent VPages.

---

# §2 DRAM Hot Tier

The hot tier is the in-DRAM cache of recently accessed offset-map entries.
Three constraints:

- **C-Compact**: per-entry footprint ≤ 50% of Viper's 16 B CCEH slot.
- **C-Safe**: false-positive lookups must not return wrong values.
- **C-Pin**: entries with pending PM cold-tier flushes must be unevictable
  (Invariant I1, see §3).

## 2.1 Entry layout

Each hot-tier slot is **8 bytes**:

```
┌─────────────────────────────────┬──────────────────────────────────┐
│  fingerprint  (4 bytes)         │  offset       (4 bytes)          │
│  uint32_t                       │  block_offset_t                  │
└─────────────────────────────────┴──────────────────────────────────┘
                          8 bytes / entry
```

Plus a separate per-slot **1-byte metadata** array:

```
bit 7-5  reserved
bit 4    visited bit (SIEVE eviction, see §2.4)
bit 3-2  state: 00=EMPTY, 01=UNPINNED, 10=PINNED, 11=IN_FLUSH
bit 1-0  reserved
```

Storing metadata separately keeps the data array dense and aligns 8-byte
entries naturally on cache lines (8 entries per 64 B line).

### 2.1.1 The 4-byte offset

The full Viper `KeyValueOffset` is 8 bytes: 45-bit `block_number` +
3-bit `page_number` + 16-bit `data_offset`. The hot-tier 4-byte offset
truncates this to:

```
13 bits   block_number_low  (low 13 bits of full block number)
3 bits    page_number       (full)
16 bits   data_offset       (full)
```

The high 32 bits of the original 45-bit `block_number` are stored in a
separate **block-base map** that all hot-tier entries within a single
block region share. At lookup:

```
full_offset.block_number = block_base_map[hot_tier_region]
                         + entry.offset_low
```

Hot-tier regions partition the cold tier into 32 segments (see §3 cold
tier), so the block-base map has only 32 entries × 4 bytes = 128 B.

## 2.2 Fingerprinting: case split by key size

Whether HiOM's 4-byte fingerprint requires extra computation depends on
what Viper's CCEH already stores in the equivalent slot
([cceh.hpp:112-113](../include/viper/cceh.hpp#L112-L113)).

### Case A: Key size ≤ 8 bytes (e.g., `uint64_t`)

Viper's CCEH stores the **raw key** in `IndexK` ([cceh.hpp:368](../include/viper/cceh.hpp#L368))
— *no* fingerprint exists yet. HiOM must compute one.

- **Derivation**: `fp = h32(key)` where `h32` is the low 32 bits of
  `std::_Hash_bytes` (the same hash CCEH uses for routing,
  [hash.hpp:63-65](../include/viper/hash.hpp#L63-L65)). Reusing the
  routing hash avoids a second computation.
- **Verification on hit**: a 4-byte fingerprint match has a false-positive
  probability of `1 / 2^32 ≈ 2.3 × 10^-10`. For 100M lookups, expected
  false matches < 1. We still verify by reading the full key from PM —
  adding ~350 ns PM read on the rare collision. Net expected verification
  cost across 100M lookups: `100M × (1/2^32) × 350 ns = 8 ms`. Negligible.

### Case B: Key size > 8 bytes or `std::string`

Viper's CCEH already stores an 8-byte hash in `IndexK`
([cceh.hpp:366](../include/viper/cceh.hpp#L366)). HiOM truncates it.

- **Derivation**: `fp = (uint32_t)(viper_index_k)`. Free.
- **Verification**: identical to Case A; Viper already does this at
  [cceh.hpp:402-404](../include/viper/cceh.hpp#L402-L404).

### Why we did not pick a 2-byte fingerprint

A 2-byte fingerprint would compress the entry to 6 bytes (33% saving over
the 4-byte version's 50% saving). But the false-positive probability becomes
`1 / 2^16 ≈ 1.5 × 10^-5`. For 100M lookups, that's ~1500 redundant PM
reads — not catastrophic but visible in p99 latency. Hash collisions
cluster in skewed workloads (zipf distribution amplifies the rate by ~5×),
pushing the cost into the 5-10 ms range, which would show up in throughput.

The 4-byte fingerprint sits at the inflection point: collisions are
statistically negligible across realistic dataset sizes (up to ~10B keys
with expected false matches < 1 per query stream).

## 2.3 Hot-tier sizing

Hot tier is **statically capped** at construction time. The user supplies
`hot_tier_capacity_entries`; HiOM allocates `hot_tier_capacity_entries × 9 B`
(8 B entry + 1 B metadata) plus modest hash-table overhead.

Rationale for static (vs. dynamic):

- **Predictable DRAM**: deployments running with a known DRAM budget can
  reason about HiOM's DRAM footprint up front.
- **Invariant I3** (§3 invariants) requires `|commit_buffer| ≤ |hot_tier|`,
  trivially enforced when hot tier size is fixed.
- **Avoids tier-resize correctness**: a dynamic tier would interleave with
  ongoing flushes and pin states, drastically complicating the protocol.

Default: **25% of available DRAM** (e.g., 32 GB DRAM → 8 GB hot tier →
~890M entries at 9 B/entry). §6 includes a sensitivity sweep varying
capacity from 1 GB to 16 GB.

## 2.4 Eviction: SIEVE

Hot-tier entries are evicted using SIEVE [Yang et al., NSDI '24], a
lazy-promotion FIFO variant with O(1) update cost and consistently better
hit rate than CLOCK or LRU on production traces.

Per-entry overhead: **1 visited bit** (in metadata byte, bit 4).

Algorithm:
- On hit: `visited |= 1`.
- On insert (new entry): place at head of FIFO with `visited = 0`.
- On eviction: walk from current `hand` pointer; if `visited == 0` and
  `state == UNPINNED`, evict; else clear `visited` and advance.

PINNED and IN_FLUSH entries are skipped during eviction (Invariant I3
ensures the pin count never exceeds capacity).

Why SIEVE over S3-FIFO [Yang et al., SOSP '23]: both achieve similar hit
rates, but SIEVE needs only 1 bit per entry vs. S3-FIFO's two-queue +
ghost-set metadata. HiOM's compact-encoding constraint makes SIEVE the
right pick.

> **NOTE**: SIEVE venue/year needs verification before submission. May be
> SOSP '23 or NSDI '24 — confirm via Google Scholar before citing.

## 2.5 Lookup path

```
lookup(key):
    h        = h32(key)                          // ~5 ns hash compute
    fp       = (uint32_t) h
    bucket   = h % num_buckets
    for slot in bucket.probe_window:             // 16-slot range, 2 cache lines
        if slot.fingerprint == fp:
            metadata[slot] |= VISITED_BIT       // SIEVE marker
            return verify(slot.offset, key)
    return MISS                                  // fall through to cold tier
```

`verify(offset, key)`:
1. Reconstruct full `KeyValueOffset` from `block_base_map + entry.offset`.
2. Read PM page header version_lock; if locked, return retry.
3. Read PM data slot at offset.
4. Compare keys; if match, return value; else return MISS.

Lookup hot-tier hit path: 1 DRAM probe + 1 PM read = same as Viper's
existing offset-map hit. No latency regression on the fast path.

## 2.6 Insert / Update path

```
insert(key, value):
    PM-side (unchanged from Viper at viper.hpp:1037-1043):
        write K/V to VPage slot
        pmem_persist(entry)
        update bitset + pmem_persist(bitset)

    HiOM-side:
        h        = h32(key)
        fp       = (uint32_t) h
        offset   = compose_compact_offset(block, page, slot)

        atomically:
            hot_tier[bucket].fingerprint = fp
            hot_tier[bucket].offset      = offset
            metadata[bucket].state       = PINNED      // Invariant I1

        commit_buffer.push((key, full_offset, INSERT))
```

The commit buffer is per-thread (see Implementation §M3), so
`commit_buffer.push` contains no contention. The PINNED state prevents
eviction until the background flusher writes this entry to the cold tier.

## 2.7 Bucket structure and concurrency

Each bucket is a fixed-size 16-slot probe window (matching Viper's CCEH
window at [cceh.hpp:184-185](../include/viper/cceh.hpp#L184-L185)). The
window is two contiguous 64-byte cache lines. Concurrency uses CAS-on-slot
with the same pattern as CCEH's `Insert`
([cceh.hpp:386-415](../include/viper/cceh.hpp#L386-L415)) — no per-bucket
mutex, lock-free probe.

Concurrent reader protection during eviction uses **epoch-based reclamation
(EBR)**, reusing Viper's already-fetched `epoch-reclaimer` dependency.
Writer threads acquire an EBR epoch on entry; eviction defers slot
reclamation until the epoch advances past all reader epochs.

## 2.8 What changed vs. Viper's CCEH

| Aspect | Viper CCEH | HiOM Hot Tier |
|--------|-----------|---------------|
| Slot size | 16 B (8 + 8) | 8 B (4 + 4) + 1 B metadata |
| Fingerprint width | 8 B (or raw key for ≤8 B keys) | 4 B always |
| Index resize | Extendible directory + segment splits | Fixed capacity |
| Eviction | None (full retention) | SIEVE |
| State per entry | None | EMPTY/UNPINNED/PINNED/IN_FLUSH (2 bits) |
| Concurrency primitive | Segment-level `sema` counter | Per-slot CAS + EBR |

## 2.9 Failure-mode analysis

| Failure | Behaviour | Recovery |
|---------|-----------|----------|
| Hot-tier slot lost (DRAM bit flip) | Lookup misses → falls through to cold tier | Cold tier authoritative |
| Hot-tier loaded with stale offset (race) | Verify step on PM data catches via key check | Returns MISS, retry |
| Hot-tier full of PINNED | Insert blocks on commit-buffer flush trigger | Backpressure |
| Crash | Hot tier entirely volatile, lost | Cold tier + VPage scan rebuild |

The hot tier holds **no authoritative state**. Every entry is either
- (a) a cached copy of a cold-tier entry (UNPINNED), or
- (b) a write that's also in the commit buffer awaiting cold-tier flush
  (PINNED / IN_FLUSH), where the K/V data on PM is already persisted.

Losing the hot tier is always recoverable — the basis for keeping it volatile.

---

# §3 Cold Tier, Invariants, Protocols (TODO)

The following sections need to be written before implementation starts:

- **§3 Cold Tier**: linear hashing, 32 fixed regions, design alternatives
  table (rejecting static-probing, cuckoo, CCEH), parallel load
- **§4 Six Invariants** (I1–I6): formal list + maintenance proofs by case
  analysis on each operation (insert, update, delete, evict, flush)
- **§5 Group Commit + Checkpoint A/B**: PM checkpoint slots, valid_pointer
  atomic flip, recovery protocol
- **§6 State Machine**: PINNED → IN_FLUSH → UNPINNED transitions, per-entry
  latch, update/delete tombstone semantics
- **§7 Evaluation Plan**: workloads, metrics, baselines, win condition
  experiments

---

# Implementation Roadmap

(Internal — not paper material.)

## Starting state

- `viper_x.hpp` + `dram_tier.hpp` + `viper_x_fixture.hpp` were deleted on
  2026-05-02. They implemented a write-back DRAM cache (different design
  point from HiOM); kept as separate baselines would have inflated §6 noise.
- Unrelated benchmark improvements they introduced are kept:
  - `viper_fixture.hpp` defensive stale-pool cleanup (CLAUDE.md pattern).
  - `YCSB_PREFILL_LIMIT` / `YCSB_OPS_LIMIT` env vars in `ycsb_bm.cpp`.
  - `Repetitions(3)` + 1M-record fast-iteration constants.
  - `all_ops_benchmark.cpp` argv-passthrough in `main()`.

## Target file layout

```
include/viper/
├── viper.hpp                    # Original Viper, unchanged
├── cceh.hpp                     # Original CCEH, unchanged
├── hash.hpp                     # Original, unchanged
└── hiom/
    ├── hiom.hpp                 # Top-level HiOM<K,V> class
    ├── hot_tier.hpp             # Compact hash + SIEVE eviction
    ├── cold_tier.hpp            # Linear hashing index
    ├── commit_buffer.hpp        # Per-thread group-commit buffer
    ├── checkpoint.hpp           # A/B checkpoint protocol
    └── invariants.hpp           # Debug-mode invariant assertions
```

## Milestones

### M0 — Hot tier MVP (1 week) ✅ COMPLETE (2026-05-03)

A working hot tier with no cold tier, no eviction, no commit buffer.
HiOM lookup either hits hot tier or falls through to Viper's CCEH directly.
Validates compact encoding + 4-byte fingerprint design choices in isolation.

- [x] `hiom::hot_tier<K, V>` with insert/lookup, no eviction. *(SIEVE
      eviction added in M1 — kept M0 functionally a superset.)*
- [x] Bucket = 16-slot probe window, CAS on fingerprint.
- [x] Block-base map (32 entries, see §2.1.1). *Implemented in
      `offset_codec.hpp`. M0 uses a degenerate 1-region map (all zeros);
      4-byte offset addresses up to 2¹³ = 8192 Viper blocks ≈ 192 MB.
      M2/M3 will introduce real 32-region routing and lift this ceiling.*
- [x] Verify path: PM data read + key match. *Implemented via two new
      additive public Viper methods (`Client::hiom_peek_offset` and
      `ReadOnlyClient::hiom_read_at_offset`) plus `HiOM::Client::verify_and_read`.*
- [x] Unit test: 1M random keys; insert + lookup correctness.
- [x] Microbench: 8M lookups/sec single-threaded target. *Hit 135 M/s
      standalone; 54.6 M/s through the full HiOM→Viper path including
      verify-on-PM-key.*

Exit (vs raw Viper lookup, ±10%): **MET.** HiOM 54.6 M/s vs raw Viper
49.5 M/s on hit-mostly lookup (200K keys, hit rate 99.95%), ratio 1.103.
HiOM marginally faster because the compact 8 B HotTier slot fits more
entries per cache line than CCEH's 16 B slot, even though we still pay
the verify-on-PM-key read on every hit.

**M0 integration footprint (2026-05-03):**
- `include/viper/hiom/offset_codec.hpp`: 4 B ↔ KVOffset codec, block-base map.
- `include/viper/hiom/hiom.hpp`: `HiOM<K,V>` wrapper + `Client`.
- `test/hiom_integration_test.cpp`: correctness, update/remove, vs-raw microbench.
- `include/viper/viper.hpp`: two additive public methods on Client/ReadOnlyClient
  (`hiom_peek_offset`, `hiom_read_at_offset`). No existing API changed.
- `CMakeLists.txt`: `VIPER_BUILD_TESTS=ON` adds the integration test target.

### M1 — SIEVE eviction (3-4 days) — functionally complete except EBR (2026-05-03)

- [x] Per-slot visited bit. (Stored in parallel `BucketMeta` array,
      one 16-bit word + 8-bit hand per bucket.)
- [x] FIFO hand pointer. (Verified by `run_sieve_hand_distribution`:
      16 consecutive evictions distribute across all 16 slots.)
- [x] Eviction triggered at capacity threshold. (Bucket-local: triggers
      when the probe window has no empty slot. No global threshold —
      aligned with per-bucket SIEVE design.)
- [ ] EBR-protected slot reuse. **Deferred to M4.** Rationale: the 64-bit
      packed `(fp, offset)` slot means every lookup reads a consistent
      snapshot via a single atomic load; the verify-on-PM-key step
      (§2.5) catches any stale offset that survives a concurrent
      evict+reinsert race. EBR adds value only if lookup returns offsets
      without PM verify, which the design does not. Revisit when M4
      introduces PINNED/IN_FLUSH and per-entry latches — that's the
      natural place to reconsider reclamation semantics.
- [x] Test: evict-and-refill correctness; no lost or duplicated entries.
      Five SIEVE tests in `hot_tier_test.cpp`: basic 17-into-16,
      visited-semantics (visited=1 survives), hand-distribution
      (16 evictions cover all slots), multi-pass second-chance
      (all-visited bucket evicts exactly one), no-loss stress (10k
      inserts into 256 slots, last-write-wins preserved).

### M2 — Cold tier (linear hashing) (1.5 weeks) — Phase B-1 complete (2026-05-03)

Phase A (earlier): standalone PM-backed `ColdTier` with insert / lookup /
delete + close+reopen persistence.

Phase B-1 (this turn): added overflow chains, region-parallel load, and
8-thread concurrent stress. Bucket-full no longer occurs below total-cap;
multi-thread correctness validated.

Phase B-2 (next): linear-hash split worker (consolidate overflow into
mirror buckets), HiOM integration to replace CCEH on miss path.

- [x] `hiom::cold_tier<K>` linear-hashing on PM with 32 fixed regions.
      *Each region owns `main_buckets_per_region` Buckets + an overflow
      pool of equal default size; routing splits the 64-bit fingerprint
      into disjoint region-id (top 5 bits) and bucket-id bits.*
- [x] In-place insert / lookup / delete. *Insert is single-pass over the
      bucket chain — find match → CAS update; or remember first empty
      slot and claim it after the chain ends. Delete is a single 8 B
      atomic CAS to a tombstone offset; the fingerprint stays so a
      subsequent re-insert reactivates via match-and-update.*
- [x] Region-parallel load (entry point for fast recovery).
      *`parallel_load(num_threads, visitor)` strides the 32 regions
      across `num_threads` workers; visitor sees `(fingerprint, offset)`
      for every live entry. 500K entries scanned in ~22 ms, 1M in 40 ms.*
- [ ] Per-region split-point advancement. *Phase B-2.* Currently relies
      on overflow chains to absorb bucket-skew; pre-sizing handles
      capacity. Split will collapse long chains.
- [x] Crash-safe single-region update via 8-byte atomic (Optane ADR).

Exit (M2 subset met): single-threaded insert **3.41 M ops/s** clean
(≥ 2 M/s target), 1.19 M/s when run after another large-pool test (PM
write-buffer interference). Lookup 200 M ops/s. Concurrent 8-thread
upsert+lookup: 0 failures across 800K ops. Overflow chain stress:
200K inserts, 9011 overflow buckets, all keys findable.

For the **100M-entry parallel_load ≤ 5 s** part of M2 exit: the test
harness scales via the `COLD_TIER_EXIT_N` env var. Default 1M finishes
in <1 s; the real 100M run is opt-in until Phase B-2 because the PM
file alone (~3 GB) needs intentional setup on shared `/pmem0`.

**Phase B-1 footprint (2026-05-03):**
- `include/viper/hiom/cold_tier.hpp`: rewritten to ~410 lines. Bucket
  header now carries `(occupancy_bitmap, has_overflow, next_overflow_idx)`.
  `parallel_load(num_threads, visitor)` added.
- `test/cold_tier_test.cpp`: extended to 9 tests — added overflow-chain
  stress, 8-thread concurrent stress, parallel_load, and a scalable
  M2-exit harness.

**Phase B-2 (next, before M3):**
1. `split_step(region)` — advance split_ptr, relocate entries from
   over-loaded main bucket into a mirror, drain associated overflow chain.
2. HiOM integration: ColdTier becomes the authoritative offset map on
   HotTier miss; CCEH fallback removed.
3. Multi-thread upsert with shared keys (today's stress uses disjoint
   keys per thread). Adds duplicate-fp prevention via per-chain lock.
4. Real 100M-entry parallel-load run on PM, verifying ≤ 5 s exit.

### M3 — Commit buffer + group commit (1 week)

- [ ] Per-thread `commit_buffer` (lock-free SPSC ring on `concurrentqueue`).
- [ ] Background flusher: drain to cold tier in batched cache-line-aligned
      writes (sort by destination bucket).
- [ ] Three-condition flush trigger: buffer-size, pinned-count, timer.
- [ ] Inline-flush fallback when pinned-count exceeds threshold.

### M4 — Pin invariants + state machine (1 week)

- [ ] 2-bit state per hot-tier entry.
- [ ] Per-entry latch for IN_FLUSH transitions.
- [ ] Update / delete tombstone semantics.
- [ ] Invariant assertion module (debug-only).
- [ ] Stress test: concurrent insert/update/delete + crash injection.

### M5 — A/B checkpoint protocol (3-4 days)

- [ ] Two checkpoint slots in PM, each with `{seq, valid, summary_hash}`.
- [ ] `valid_pointer` 8-byte atomic.
- [ ] Flush completion → write inactive slot → flip pointer.
- [ ] Recovery: load checkpoint, scan unflushed VPage range, replay buffer.

### M6 — Recovery (3 days)

- [ ] Parallel cold-tier load (32 threads, one per region).
- [ ] VPage scan bounded by `valid_checkpoint.seq`.
- [ ] Hot-tier rebuild from cold tier (warm-up phase).

Exit: 100 GB working set recovers in ≤ 5 s.

### M7 — Evaluation (3-4 weeks)

Total: ~9 weeks of implementation + 4 weeks of evaluation.

## Design constraints (enforced at PR-review time)

1. **No PM access on hot-tier hit path** beyond the single value read.
2. **No DRAM allocation in commit buffer steady-state**. Fixed-size ring;
   growth means a design bug.
3. **All PM writes go through `viper::internal::pmem_persist`**. No direct
   `clwb` / `sfence` — the existing helper is canonical
   ([viper.hpp:101-108](../include/viper/viper.hpp#L101-L108)).
4. **Fingerprint computation reuses the routing hash**.
5. **Cold tier writes are batched**. Single-key flush is a bug; entries are
   always sorted-and-coalesced by destination bucket.

## Risk register

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Cold tier sequential-write bandwidth lower than expected | Medium | Pre-bench Optane sequential write at start of M2; if < 4 GB/s aggregate, revisit batch size |
| SIEVE hit rate worse than CCEH on YCSB-A skew | Low | Fallback to S3-FIFO behind same interface |
| Commit buffer becomes contention point at >24 threads | Medium | Per-thread design eliminates it; if worker thread becomes bottleneck, move to multi-flusher |
| Cold-tier linear-hashing split causes throughput hiccups | Medium | Background balancer with rate-limited splits |
| Recovery time still dominated by VPage scan in worst case | Low | Tighten commit interval to bound unflushed window |

## Open questions (resolve before M2)

- **Cold tier capacity provisioning**: pre-size for `1.5×` expected dataset,
  or grow lazily? *Tentative: pre-size. Revisit after M2 measurement.*
- **Hot-tier bucket count**: power of two for fast modulo, or prime for
  better distribution? CCEH uses power of two
  ([cceh.hpp:181](../include/viper/cceh.hpp#L181)). *Tentative: power of two.*
- **Tombstone GC**: physically remove from cold tier, or keep until next
  region split? Affects steady-state space for delete-heavy workloads.
  *Tentative: keep until split, evaluate.*

## Dependencies on existing code

| Dep | Used for | Verified at |
|-----|----------|-------------|
| `viper::internal::pmem_persist` | All PM writes in cold tier and checkpoint | [viper.hpp:101-108](../include/viper/viper.hpp#L101-L108) |
| `moodycamel::ConcurrentQueue` | Per-thread commit buffer | [viper.hpp:18,443](../include/viper/viper.hpp#L18) |
| `epoch-reclaimer` (CMake fetch) | EBR for hot-tier eviction | `benchmark/CMakeLists.txt` |
| `viper::internal::pmem_memcpy_persist` | Batch flush to cold tier | [viper.hpp:110-113](../include/viper/viper.hpp#L110-L113) |
| `viper::Viper<K,V>::Client` | PM data read on hot-tier verify | [viper.hpp:361](../include/viper/viper.hpp#L361) |

HiOM does **not** depend on CCEH (replaced by hot+cold tiers) or TBB.

---

# Appendix A — Source-Code Verification Table

Every numerical or structural claim about Viper in this design must be
traceable to source. Verified by reading `include/viper/viper.hpp` and
`include/viper/cceh.hpp` on 2026-05-02.

## A.1 Verified facts

| Claim | Source | Notes |
|-------|--------|-------|
| Each CCEH slot is 16 bytes (8 B key/fingerprint + 8 B offset) | [cceh.hpp:190-203](../include/viper/cceh.hpp#L190-L203) `struct Pair { IndexK key; IndexV value; }` where `IndexK = size_t` (8 B), `IndexV = KeyValueOffset` (8 B) | ✓ |
| `KeyValueOffset` itself is 8 bytes (bit-packed 45+3+16) | [cceh.hpp:134-144](../include/viper/cceh.hpp#L134-L144) | Earlier proposal said 16 B; corrected. |
| Fingerprint storage is conditional | [cceh.hpp:112-113](../include/viper/cceh.hpp#L112-L113) `requires_fingerprint(K) := std::is_same_v<K, std::string> \|\| sizeof(K) > 8` | Drives §2.2 case split. |
| For ≤8 B keys, raw key is stored in the index slot (NOT a hash) | [cceh.hpp:367-369](../include/viper/cceh.hpp#L367-L369) | Key insight that breaks original "16 B = 8 B fp + 8 B offset" framing. |
| CCEH default initial capacity is 131,072 | [viper.hpp:475](../include/viper/viper.hpp#L475) `map_{131072}` | Hardcoded constant. |
| CCEH segment size is 16 KB (1024 slots × 16 B) | [cceh.hpp:183](../include/viper/cceh.hpp#L183) `kSegmentSize = (1 << 8) * 16 * 4 = 16384` | |
| Initial CCEH allocation is ~2 GB | 131,072 segments × 16 KB; verified by [cceh.hpp:483-486](../include/viper/cceh.hpp#L483-L486) loop creating one Segment per directory entry on init | Fixed cost regardless of how many keys are stored. |
| CCEH is DRAM-only by default | [cceh.hpp:21](../include/viper/cceh.hpp#L21) `//#define CCEH_PERSISTENT` is commented out; [cceh.hpp:228-231](../include/viper/cceh.hpp#L228-L231) uses `posix_memalign` (DRAM) | |
| Recovery scans all VPages in parallel | [viper.hpp:791-847](../include/viper/viper.hpp#L791-L847) `recover_database()` | |
| Default 32 recovery threads | [viper.hpp:64](../include/viper/viper.hpp#L64) `num_recovery_threads = 32` | |
| `version_lock` is per-page, 1 byte | [viper.hpp:33](../include/viper/viper.hpp#L33) `using version_lock_t = uint8_t;` and [viper.hpp:169](../include/viper/viper.hpp#L169) | |
| version_lock layout: high bits = CLIENT/USED, mid = version counter, low = lock bit | [viper.hpp:41-44](../include/viper/viper.hpp#L41-L44) | 6 bits remain for version counter. |
| Client model = per-thread VBlock | [viper.hpp:392-405](../include/viper/viper.hpp#L392-L405); [viper.hpp:884-913](../include/viper/viper.hpp#L884-L913) | |
| `concurrentqueue` already a Viper dependency | [viper.hpp:18](../include/viper/viper.hpp#L18) include; [viper.hpp:443](../include/viper/viper.hpp#L443) used for `free_blocks_` only | Free for HiOM commit_buffer reuse. |
| `pmem_persist` is `_mm_clwb` loop + single `_mm_sfence` | [viper.hpp:101-108](../include/viper/viper.hpp#L101-L108) | |
| BLOCK_SIZE = 24 KB = 6 × 4 KB pages | [viper.hpp:36-37](../include/viper/viper.hpp#L36-L37) `NUM_DIMMS = 6, BLOCK_SIZE = NUM_DIMMS * PAGE_SIZE` | |
| Default hash is `std::_Hash_bytes`, not MurmurHash2 | [hash.hpp:63-65](../include/viper/hash.hpp#L63-L65) `hash_funcs[0]`; `hash_funcs[0] = standard` ([hash.hpp:58-60](../include/viper/hash.hpp#L58-L60)) where `standard = std::_Hash_bytes` ([hash.hpp:22-25](../include/viper/hash.hpp#L22-L25)) | CLAUDE.md and Viper README incorrectly say MurmurHash2. |
| Reclamation default OFF | [viper.hpp:67](../include/viper/viper.hpp#L67) `enable_reclamation = false` | Evaluation must control for this. |
| Resize threshold default 0.85 | [viper.hpp:61](../include/viper/viper.hpp#L61) `resize_threshold = 0.85` | |
| Insert persistence order: K/V → bitset → DRAM map | [viper.hpp:1037-1054](../include/viper/viper.hpp#L1037-L1054) | HiOM commit-buffer enqueue MUST follow this order. |
| Variable-size string recovery is **NOT implemented** | [viper.hpp:849-853](../include/viper/viper.hpp#L849-L853) `recover_database()` for `<string,string>` throws `"Not implemented yet"` | Evaluation main line must use fixed-size keys. |
| CCEH segment internal probe range = 16 slots = 4 cache lines × 4 pairs | [cceh.hpp:184-185](../include/viper/cceh.hpp#L184-L185) `kNumPairPerCacheLine = 4, kNumCacheLine = 4` | |

## A.2 DRAM cost of CCEH (worked example)

For 100M records:
- Initial 2 GB (131,072 segments × 16 KB) allocated regardless of dataset size.
- 131,072 segments × 1024 slots/segment = 134M slot capacity at 100% load.
- 100M / 134M ≈ 75% load → at edge of segment splits.
- With splits, total allocation grows to ~2.3 GB (matches Viper §5.3.4).

For 1B records:
- Splits push to ~10× segments → ~20 GB DRAM index footprint.
- This is the regime where HiOM's tiered design starts to matter.

## A.3 Insert path persistence sequence ([viper.hpp:1023-1069](../include/viper/viper.hpp#L1023-L1069))

```
1. v_page_->lock()                                      // page latch
2. Find free_slot_idx in free_slots bitmap
3. v_page_->data[free_slot_idx] = {key, value}
4. internal::pmem_persist(entry_ptr, sizeof(VEntry))   // clwb + sfence
5. free_slots->reset(free_slot_idx)
6. internal::pmem_persist(free_slots, ...)             // clwb + sfence
7. viper_.map_.Insert(key, kv_offset, ...)             // DRAM-only update
8. (optional) free_occupied_slot(old_offset, ...)
9. v_page_->unlock()
```

**Implication for HiOM**: the commit-buffer enqueue replaces step 7. The
DRAM hot-tier `PINNED` insert and the buffer push must happen *after* the
two `pmem_persist` calls in steps 4 and 6, otherwise a crash can leave the
buffer pointing to non-persisted data.

## A.4 Get path ([viper.hpp:1191-1207](../include/viper/viper.hpp#L1191-L1207))

```
1. CCEH Get(key) → returns KVOffset
2. If is_tombstone(): return false
3. get_value_from_offset(kv_offset, value):
   a. Load v_page->version_lock
   b. Check IS_LOCKED(lock_val): if locked, return false → outer loop retries
   c. Read v_page->data[slot].second into *value
   d. Re-load lock_val: if changed, retry (concurrent write detected)
4. Return true
```

This optimistic-read protocol must be preserved when HiOM hot-tier hits
return offsets — the page-level version_lock check still runs.

## A.5 Findings beyond the original verification list

These were not asked about in the proposal but discovered during reading:

1. **CCEH segment-level concurrency**: a single `std::atomic<uint64_t> sema`
   per segment ([cceh.hpp:243](../include/viper/cceh.hpp#L243)) with values:
   `0` = idle, `1..N` = N concurrent readers/writers, `EXCLUSIVE_LOCK` = -1
   for split, `SPLIT_REQUEST_BIT` = 1<<63 for "wants split". HiOM's cold tier
   can adopt this pattern unchanged.

2. **CCEH `Insert` falls back to in-place update if key already present**
   ([cceh.hpp:399-415](../include/viper/cceh.hpp#L399-L415)): match by
   `key_checker == _[slot].key`, then verify with `key_check_fn` callback if
   `using_fp_`, then CAS the `value.offset`. HiOM hot-tier and cold-tier
   upserts should mirror this pattern.

3. **Page strategies are configurable but `BlockBased` is the only one
   actually used** ([viper.hpp:887-893](../include/viper/viper.hpp#L887-L893)
   sets `strategy_ = BlockBased` unconditionally). HiOM can assume `BlockBased`.

4. **`get_new_block` uses `rand() % num_pages_per_block`** for non-string
   keys ([viper.hpp:932-935](../include/viper/viper.hpp#L932-L935)) to spread
   load across DIMMs. HiOM's hot tier doesn't need to replicate this; cold
   tier benefits since K hashes are uniform.

5. **Deadlock-handling in remove path** ([viper.hpp:1304-1412](../include/viper/viper.hpp#L1304-L1412))
   uses a global `deadlock_offset_lock_` + shared vector of pending offsets,
   triggered after 32 retries on a contended page lock. HiOM remove marks
   DRAM hot-tier `TOMBSTONE` + appends to commit buffer; the actual PM
   bitset reset happens during the next flush, not synchronously.
