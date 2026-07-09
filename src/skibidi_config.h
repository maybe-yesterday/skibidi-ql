#pragma once

#include <cstddef>
#include <cstdint>

namespace skibidi::config {

inline constexpr const char* kBufferPoolPagesEnv =
    "SKIBIDI_BUFFER_PAGES";
inline constexpr const char* kCompilationCacheEntriesEnv =
    "SKIBIDI_CACHE_ENTRIES";
inline constexpr const char* kSqliteStatementCacheEntriesEnv =
    "SKIBIDI_STATEMENT_CACHE_ENTRIES";
inline constexpr const char* kBloomMinimumBitsEnv =
    "SKIBIDI_BLOOM_MIN_BITS";
inline constexpr const char* kBloomBitsPerValueEnv =
    "SKIBIDI_BLOOM_BITS_PER_VALUE";
inline constexpr const char* kExactValueCountLimitEnv =
    "SKIBIDI_EXACT_VALUE_COUNT_LIMIT";
inline constexpr const char* kVectorBatchRowsEnv =
    "SKIBIDI_VECTOR_BATCH_ROWS";

inline constexpr std::size_t kDefaultBufferPoolPages = 1024;
inline constexpr std::size_t kDefaultCompilationCacheEntries = 128;
inline constexpr std::size_t kDefaultMaxCompilationCacheBytes =
    4 * 1024 * 1024;
inline constexpr std::size_t kDefaultSqliteStatementCacheEntries = 128;
inline constexpr std::size_t kEstimatedIndexLevelBytes = 1024;

// Storage-format constants: these are not runtime-tunable without creating a
// new on-disk format.
inline constexpr std::size_t kPageSizeBytes = 4096;
inline constexpr std::uint32_t kSlottedPageMagic = 0x534B5047; // "SKPG"

// Statistics/optimizer defaults. Bucket count is compiled into the catalog's
// in-memory metadata shape; the others are safe runtime knobs.
inline constexpr std::size_t kStatisticsBucketCount = 16;
inline constexpr std::size_t kDefaultExactValueCountLimit = 4096;
inline constexpr std::int64_t kDenseGroupMaxDomainSpan = 4096;
inline constexpr std::size_t kDefaultBloomMinimumBits = 64;
inline constexpr std::size_t kDefaultBloomBitsPerValue = 12;
inline constexpr std::size_t kDefaultVectorBatchRows = 1024;

// TensorQL snapshot split defaults. Snapshots persist the assigned split per
// row, so these names document the default policy used while creating one.
inline constexpr std::uint64_t kSnapshotSplitBuckets = 100;
inline constexpr std::uint64_t kDefaultTrainSplitPercent = 80;
inline constexpr std::uint64_t kDefaultValidationSplitPercent = 10;

std::size_t readEnvSizeT(const char* name,
                         std::size_t fallback,
                         std::size_t minimum = 1);
std::size_t defaultBufferPoolPages();
std::size_t defaultCompilationCacheEntries();
std::size_t defaultSqliteStatementCacheEntries();
std::size_t bloomMinimumBits();
std::size_t bloomBitsPerValue();
std::size_t exactValueCountLimit();
std::size_t vectorBatchRows();

} // namespace skibidi::config
