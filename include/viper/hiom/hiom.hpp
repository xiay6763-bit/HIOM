#pragma once

// HiOM<K,V> — a tiered offset-map wrapper around viper::Viper<K,V>.
//
// Architecture (M0/M1 scope):
//   - HiOM holds a *reference* to a caller-owned Viper<K,V>.
//   - HotTier (DRAM) sits in front of Viper's CCEH offset map. On lookup,
//     a HotTier hit produces a 4-byte compact offset; we decode it into a
//     full KVOffset, read PM via Viper::ReadOnlyClient::hiom_read_at_offset,
//     and verify the PM-side key matches the lookup key (catches the
//     1/2^32 fp collision case).
//   - On HotTier miss we fall through to Viper's normal get(), then warm
//     the hot tier with the recovered offset.
//   - On put/update we go through Viper's existing put/update (so PM
//     persistence ordering is unchanged), then ask Viper for the offset
//     it just installed (Client::hiom_peek_offset) and mirror it into
//     HotTier.
//
// What's deferred (later milestones):
//   - Cold tier (M2): for now HiOM still relies on Viper's CCEH as the
//     authoritative offset map. Cold tier replaces CCEH in M2/M3.
//   - Pin / commit-buffer (M3, M4): puts here are not pinned because
//     there is no buffered flush. HotTier eviction is safe to drop a
//     recent insert because the offset is still in CCEH. This safety
//     net disappears when CCEH is removed.
//   - True 32-region routing (M3): every key maps to region 0; the
//     compact offset addresses up to 8K Viper blocks (~192 MB).
//
// Concurrency: each thread should call HiOM::get_client() and use that
// Client object exclusively. HotTier itself is thread-safe; the wrapping
// Client just forwards.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <utility>

#include "viper/cceh.hpp"
#include "viper/viper.hpp"
#include "viper/hiom/hot_tier.hpp"
#include "viper/hiom/offset_codec.hpp"

namespace viper::hiom {

// Compute a 4-byte fingerprint from a key. Reuses Viper's routing hash
// (cceh::h, see hash.hpp:63-65) so we do not pay a second hash.
// fp == 0 is reserved by HotTier (kEmptyFp), so we bias to 1.
template <typename K>
inline std::uint32_t key_fingerprint(const K& key) {
    const std::size_t h = cceh::h(&key, sizeof(K));
    const std::uint32_t fp = static_cast<std::uint32_t>(h);
    return fp == 0 ? 1u : fp;
}

template <typename K, typename V>
class HiOM {
  public:
    using ViperT = viper::Viper<K, V>;
    using KVOffset = viper::KeyValueOffset;

    struct Stats {
        std::atomic<std::uint64_t> hot_hits{0};
        std::atomic<std::uint64_t> hot_misses{0};
        std::atomic<std::uint64_t> hot_fp_collisions{0};
        std::atomic<std::uint64_t> hot_warmups{0};
    };

    HiOM(ViperT& viper, std::size_t hot_buckets_pow2)
        : viper_(viper),
          hot_(hot_buckets_pow2),
          base_map_{}  // M0: all zero, single region 0
    {}

    HiOM(const HiOM&) = delete;
    HiOM& operator=(const HiOM&) = delete;

    class Client {
      public:
        Client(HiOM& hiom, typename ViperT::Client viper_client)
            : hiom_(hiom), viper_(std::move(viper_client)) {}

        bool put(const K& key, const V& value) {
            // Step 1: viper does the PM-side write (data + bitset persisted)
            // and updates its own CCEH. Persistence ordering is unchanged
            // from the original Viper put path (viper.hpp ~1037-1054).
            if (!viper_.put(key, value)) return false;

            // Step 2: recover the KVOffset that Viper just installed and
            // mirror into HotTier.
            mirror_into_hot(key);
            return true;
        }

        bool get(const K& key, V* value) {
            const std::uint32_t fp = key_fingerprint(key);
            if (auto packed = hiom_.hot_.lookup(fp)) {
                if (verify_and_read(key, *packed, value)) {
                    hiom_.stats_.hot_hits.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                // Verify failed: fp collision OR concurrent update raced
                // with our optimistic read. Fall through to viper.
                hiom_.stats_.hot_fp_collisions.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                hiom_.stats_.hot_misses.fetch_add(1, std::memory_order_relaxed);
            }

            // Fallback through Viper's CCEH. Warm the hot tier so the
            // next lookup of the same key hits.
            if (!viper_.get(key, value)) return false;
            mirror_into_hot(key);
            hiom_.stats_.hot_warmups.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        bool remove(const K& key) {
            const bool ok = viper_.remove(key);
            if (ok) hiom_.hot_.remove(key_fingerprint(key));
            return ok;
        }

        template <typename UpdateFn>
        bool update(const K& key, UpdateFn fn) {
            if (!viper_.update(key, std::move(fn))) return false;
            // Update wrote a new VPage slot; refresh hot-tier mirror.
            mirror_into_hot(key);
            return true;
        }

      private:
        // Verify the hot-tier offset really points at `key`'s value, and
        // if so fill *value. Returns false on any mismatch (fp collision,
        // stale offset, locked page, version torn) — caller falls back.
        bool verify_and_read(const K& key, compact_offset_t packed, V* value) {
            const auto d = decode(packed, route_to_region_default(),
                                  hiom_.base_map_);
            const KVOffset off{
                static_cast<viper::block_size_t>(d.block_number),
                static_cast<viper::page_size_t>(d.page_number),
                static_cast<viper::data_offset_size_t>(d.data_offset)};
            K pm_key;
            V pm_val;
            // viper_ is a Client (derives from ReadOnlyClient), so we can
            // call hiom_read_at_offset directly.
            if (!viper_.hiom_read_at_offset(off, &pm_key, &pm_val)) {
                return false;  // locked or version-torn
            }
            if (!(pm_key == key)) return false;  // fp collision
            *value = std::move(pm_val);
            return true;
        }

        // Look up the current KVOffset for `key` via Viper's CCEH, encode
        // it to 4 bytes, and stuff into HotTier. Skips silently if the
        // block is out of range for M0's degenerate 1-region codec
        // (caller correctness still holds — CCEH still has the offset).
        void mirror_into_hot(const K& key) {
            KVOffset off = viper_.hiom_peek_offset(key);
            if (off.is_tombstone()) return;
            const auto [block, page, slot] = off.get_offsets();
            auto packed = encode(block,
                                 static_cast<std::uint8_t>(page),
                                 static_cast<std::uint16_t>(slot),
                                 route_to_region_default(),
                                 hiom_.base_map_);
            if (!packed) return;
            hiom_.hot_.upsert(key_fingerprint(key), *packed);
        }

        HiOM& hiom_;
        typename ViperT::Client viper_;
    };

    Client get_client() { return Client{*this, viper_.get_client()}; }

    HotTier& hot_tier() { return hot_; }
    const Stats& stats() const { return stats_; }
    BlockBaseMap& base_map() { return base_map_; }

  private:
    ViperT& viper_;
    HotTier hot_;
    BlockBaseMap base_map_;
    Stats stats_;
};

}  // namespace viper::hiom
