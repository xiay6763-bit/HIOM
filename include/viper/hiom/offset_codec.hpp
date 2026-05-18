#pragma once

// Compact-offset codec for the HiOM hot tier.
//
// Viper's full KVOffset is 8 bytes:
//   block_number : 45 bits   (up to 2^45 blocks)
//   page_number  : 3 bits    (0..5 within a 6-page block)
//   data_offset  : 16 bits   (slot index within a page, or byte offset)
//
// HiOM compacts this to 4 bytes (§2.1.1 of design). Bit layout was
// widened in M3.5 (2026-05-17) so a single region covers a large
// fraction of the 64 GiB pool:
//   bits 0..7   : data_offset      (8 bits — slot index up to 255.
//                                   Smallest practical Viper entry
//                                   is 16 B (uint64+uint64) yielding
//                                   PAGE_SIZE/16 = 256 slots per page,
//                                   which just fits. Production
//                                   workloads with K8+V200 use 19.)
//   bits 8..10  : page_number      (3 bits, unchanged)
//   bits 11..31 : block_number_low (21 bits — 2 Mi blocks ≈ 48 GiB
//                                   per region. Covers all win-
//                                   condition dataset sizes (max
//                                   100 M × 208 B = 20.8 GiB) and
//                                   leaves margin. Multi-region
//                                   routing remains future work for
//                                   pool usage > 48 GiB.)
//
// The high 32 bits of block_number are recovered by lookup into a
// per-region block_base_map (§3 routes keys to one of 32 regions).
// For M0 / M2 — where there is exactly one region and
// block_base_map[0] == 0 — the encoded form addresses up to 2^21 =
// 2097152 blocks ≈ 48 GiB of PMem. M3+ multi-region routing remains
// future work for pool usage > 48 GiB.
//
// Reserved sentinel: kInvalidCompactOffset == 0xFFFFFFFFu — still
// reachable only via (page=7, ...) which is impossible because
// page_number is bounded by NUM_DIMMS=6.

#include <array>
#include <cstdint>
#include <optional>

namespace viper::hiom {

using compact_offset_t = std::uint32_t;

inline constexpr compact_offset_t kInvalidCompactOffset = 0xFFFFFFFFu;

inline constexpr unsigned kBlockLowBits = 21;
inline constexpr unsigned kPageBits = 3;
inline constexpr unsigned kDataOffsetBits = 8;
static_assert(kBlockLowBits + kPageBits + kDataOffsetBits == 32,
              "compact offset must pack into 32 bits");

inline constexpr std::uint64_t kBlockLowMask = (1ull << kBlockLowBits) - 1;
inline constexpr std::uint64_t kPageMask     = (1ull << kPageBits) - 1;
inline constexpr std::uint64_t kDataOffMask  = (1ull << kDataOffsetBits) - 1;

inline constexpr std::size_t kNumRegions = 32;

// 32-entry block_base_map. Each entry is the high 32 bits of the base
// block number for that region. region_id ∈ [0, 32). For M0 the table
// is degenerate: all zeros, single region used.
//
// We deliberately store this as std::array (POD-like) so HiOM can hold
// one per instance and pass references around without ownership concerns.
using BlockBaseMap = std::array<std::uint32_t, kNumRegions>;

// Encode a (block, page, data_offset) into a 4-byte compact offset,
// given the region the block belongs to. Returns std::nullopt if the
// block is out of range for the current base (i.e., requires a region
// the codec doesn't know about — should be impossible if region routing
// is consistent; see HiOM::route_block_to_region).
inline std::optional<compact_offset_t> encode(std::uint64_t block_number,
                                              std::uint8_t page_number,
                                              std::uint16_t data_offset,
                                              std::size_t region_id,
                                              const BlockBaseMap& base_map) {
    if (region_id >= kNumRegions) return std::nullopt;
    const std::uint64_t base = static_cast<std::uint64_t>(base_map[region_id])
                               << kBlockLowBits;
    if (block_number < base) return std::nullopt;
    const std::uint64_t low = block_number - base;
    if (low > kBlockLowMask) return std::nullopt;
    if (page_number > kPageMask) return std::nullopt;
    // M3.5: data_offset must fit in kDataOffsetBits (7 bits = 127).
    // Old codec used 16 bits, so uint16_t input always fit. With 7-bit
    // field, slot indices > 127 (only possible for entry size < 32 B)
    // or byte offsets (variable-size path) would silently corrupt the
    // page_number bits via OR. Reject these explicitly — caller treats
    // nullopt as "not encodable" and skips HotTier mirroring.
    if (data_offset > kDataOffMask) return std::nullopt;
    const compact_offset_t packed
        = (static_cast<compact_offset_t>(low) << (kPageBits + kDataOffsetBits))
        | (static_cast<compact_offset_t>(page_number & kPageMask)
           << kDataOffsetBits)
        | static_cast<compact_offset_t>(data_offset);
    if (packed == kInvalidCompactOffset) return std::nullopt;  // collision
    return packed;
}

// Decode a 4-byte compact offset back into (block, page, data_offset).
// Caller supplies the same region_id that encode() used.
struct DecodedOffset {
    std::uint64_t block_number;
    std::uint8_t  page_number;
    std::uint16_t data_offset;
};

inline DecodedOffset decode(compact_offset_t packed, std::size_t region_id,
                            const BlockBaseMap& base_map) {
    DecodedOffset d{};
    d.data_offset
        = static_cast<std::uint16_t>(packed & kDataOffMask);
    d.page_number
        = static_cast<std::uint8_t>((packed >> kDataOffsetBits) & kPageMask);
    const std::uint64_t low
        = (packed >> (kPageBits + kDataOffsetBits)) & kBlockLowMask;
    const std::uint64_t base = (region_id < kNumRegions)
        ? (static_cast<std::uint64_t>(base_map[region_id]) << kBlockLowBits)
        : 0;
    d.block_number = base + low;
    return d;
}

// Default (M0) routing: every key lives in region 0. M2/M3 will replace
// this with real hash-bit routing once cold tier exists.
inline std::size_t route_to_region_default() { return 0; }

}  // namespace viper::hiom
