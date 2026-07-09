#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace skibidi::hash {

// FNV-1a-style 64-bit constants. The offset basis is SkibidiQL's historical
// seed, not the canonical FNV offset basis. Keep it stable: catalog
// fingerprints and plan-cache keys use it to decide when compiled artifacts
// are still valid after schema/context changes.
inline constexpr std::uint64_t kFnv1a64OffsetBasis =
    1469598103934665603ULL;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;
inline constexpr unsigned char kFnv1a64FieldSeparator = 0xff;

// Constants from the SplitMix64 finalizer. We use this as a small deterministic
// avalanche step for snapshot shuffles and Bloom-filter probe derivation.
inline constexpr std::uint64_t kGoldenRatio64 = 0x9e3779b97f4a7c15ULL;
inline constexpr std::uint64_t kSplitMix64Multiplier1 =
    0xff51afd7ed558ccdULL;
inline constexpr std::uint64_t kSplitMix64Multiplier2 =
    0xc4ceb9fe1a85ec53ULL;

inline std::uint64_t fnv1a64AppendByte(std::uint64_t hash,
                                       unsigned char byte) {
    hash ^= byte;
    hash *= kFnv1a64Prime;
    return hash;
}

inline std::uint64_t fnv1a64Append(std::uint64_t hash,
                                   std::string_view text) {
    for (unsigned char ch : text) {
        hash = fnv1a64AppendByte(hash, ch);
    }
    return hash;
}

inline std::uint64_t fnv1a64(std::string_view text) {
    return fnv1a64Append(kFnv1a64OffsetBasis, text);
}

inline std::uint64_t avalanche64(std::uint64_t value) {
    value ^= value >> 33;
    value *= kSplitMix64Multiplier1;
    value ^= value >> 33;
    value *= kSplitMix64Multiplier2;
    value ^= value >> 33;
    return value;
}

inline std::size_t combine(std::size_t seed, std::size_t value) {
    return seed ^ (value + static_cast<std::size_t>(kGoldenRatio64) +
                   (seed << 6) + (seed >> 2));
}

} // namespace skibidi::hash
