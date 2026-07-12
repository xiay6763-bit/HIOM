// Offline fp64-collision scan for the HiOM ColdTier identity assumption.
//
// ColdTier uses key_fingerprint64(key) as the SOLE identity: upsert overwrites
// the offset in place on an fp64 match, lookup returns the single stored offset,
// and the read path then key-verifies the VPage record. If two DISTINCT keys in
// the workload share an fp64, the second upsert clobbers the first's offset, so
// one key becomes permanently unreadable (read_success_rate < 1) — a real
// semantic error, not a timing race.
//
// This program recomputes fp64 for the EXACT prefill keyspace (dense integer
// keys 0..N-1, each packed as KeyType8 = two uint32 lanes both = (uint32_t)key)
// with the EXACT hash (cceh::key_fingerprint64 => std::_Hash_bytes over 8 bytes,
// seed 0xc70697UL) and reports any collision pair. It is deterministic and has
// no flusher/timing dependency, so it separates "true 64-bit collision in this
// dataset" from "concurrent stale-offset race" definitively.
//
// Build: see test/CMakeLists wiring (links only the header-only hash + hiom).

#include "viper/hiom/hiom.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

// Reproduce KeyType8 = BMRecord<uint32_t,2> EXACTLY (benchmark.hpp:27-86):
// an 8-byte key of two uint32 lanes, both set to (uint32_t)key by the
// fill-constructor. Defined inline here so this scanner does not pull in the
// google-benchmark tree via benchmark.hpp — the ONLY thing we need is the
// key's byte layout, which drives the hash.
template <typename T, int N>
struct BMRecord {
    std::array<T, N> data;
    BMRecord() { data.fill(0); }
    explicit BMRecord(std::uint64_t x) { data.fill(static_cast<T>(x)); }
};
using KeyType8 = BMRecord<std::uint32_t, 2>;
static_assert(sizeof(KeyType8) == 8, "KeyType8 must be 8 bytes");

int main(int argc, char** argv) {
    const std::uint64_t N =
        (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 10'000'000ull;
    std::printf("=== fp64 collision scan over dense keyspace [0, %llu) ===\n",
                (unsigned long long)N);
    std::printf("key packing: KeyType8{key} (two uint32 lanes = (uint32_t)key)\n");
    std::printf("hash: key_fingerprint64 => std::_Hash_bytes(&k8, 8, seed)\n\n");

    // fp64 -> first key that produced it. On a second key with the same fp64,
    // report the colliding pair with full identity.
    std::unordered_map<std::uint64_t, std::uint64_t> seen;
    seen.reserve(N * 2);

    std::size_t collisions = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    for (std::uint64_t k = 0; k < N; ++k) {
        const KeyType8 key{k};
        const std::uint64_t fp = viper::hiom::key_fingerprint64(key);
        auto it = seen.find(fp);
        if (it == seen.end()) {
            seen.emplace(fp, k);
        } else {
            ++collisions;
            if (pairs.size() < 32) pairs.emplace_back(it->second, k);
        }
        if ((k % 2'000'000) == 0 && k > 0) {
            std::printf("  scanned %llu / %llu  collisions so far=%zu\n",
                        (unsigned long long)k, (unsigned long long)N, collisions);
        }
    }

    std::printf("\n=== RESULT: %zu fp64-collision(s) in [0, %llu) ===\n",
                collisions, (unsigned long long)N);
    for (auto& pr : pairs) {
        const KeyType8 a{pr.first};
        const KeyType8 b{pr.second};
        std::printf("  COLLISION: key %llu and key %llu share fp64=0x%016llx\n",
                    (unsigned long long)pr.first, (unsigned long long)pr.second,
                    (unsigned long long)viper::hiom::key_fingerprint64(a));
        // Sanity: recompute both independently and confirm equality.
        if (viper::hiom::key_fingerprint64(a) != viper::hiom::key_fingerprint64(b)) {
            std::printf("    (!! recompute mismatch — NOT a stable collision)\n");
        }
    }
    if (collisions == 0) {
        std::printf("  No true fp64 collision in this keyspace. A read failure\n"
                    "  in the running system is therefore a CONCURRENCY/stale-\n"
                    "  offset race, not a hash collision — look at flusher/read\n"
                    "  visibility, not ColdTier multi-candidate support.\n");
    } else {
        std::printf("  TRUE 64-bit collision(s) exist in this dataset. ColdTier's\n"
                    "  single-fp identity is unsound for it: it needs same-fp\n"
                    "  multi-candidate + full-key verify (do NOT just reseed or\n"
                    "  relax the read gate).\n");
    }
    return collisions == 0 ? 0 : 3;  // exit 3 marks true collisions found
}
