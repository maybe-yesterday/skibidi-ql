#include "cache.h"

#include <cstring>
#include <iterator>
#include <utility>

CompilationCache::CompilationCache(std::size_t maxEntries,
                                   std::size_t maxBytes)
    : maxEntries_(maxEntries), maxBytes_(maxBytes) {}

bool CompilationCache::get(std::string_view source,
                           std::uint64_t schemaFingerprint,
                           CachedCompilationHandle& value) {
    const std::uint64_t hash = hashKey(source, schemaFingerprint);
    auto entry = find(source, schemaFingerprint, hash);
    if (entry == lru_.end()) {
        ++misses_;
        return false;
    }

    ++hits_;
    value = entry->value;
    lru_.splice(lru_.begin(), lru_, entry);
    return true;
}

CachedCompilationHandle CompilationCache::put(
    std::string source,
    std::uint64_t schemaFingerprint,
    CachedCompilation value) {
    auto handle =
        std::make_shared<const CachedCompilation>(std::move(value));
    if (maxEntries_ == 0 || maxBytes_ == 0) return handle;

    const std::uint64_t hash = hashKey(source, schemaFingerprint);
    auto existing = find(source, schemaFingerprint, hash);
    if (existing != lru_.end()) {
        erase(existing);
    }

    const std::size_t itemBytes = estimateBytes(source, *handle);
    if (itemBytes > maxBytes_) return handle;

    Entry entry;
    entry.source = std::move(source);
    entry.schemaFingerprint = schemaFingerprint;
    entry.hash = hash;
    entry.value = handle;
    entry.bytes = itemBytes;
    lru_.push_front(std::move(entry));
    bytes_ += itemBytes;
    entries_.emplace(hash, lru_.begin());

    while (entries_.size() > maxEntries_ || bytes_ > maxBytes_) {
        evictOldest();
    }
    return handle;
}

void CompilationCache::clear() {
    lru_.clear();
    entries_.clear();
    bytes_ = 0;
}

void CompilationCache::resetStats() {
    hits_ = 0;
    misses_ = 0;
    evictions_ = 0;
}

CacheStats CompilationCache::stats() const {
    CacheStats result;
    result.hits = hits_;
    result.misses = misses_;
    result.evictions = evictions_;
    result.entries = entries_.size();
    result.bytes = bytes_;
    return result;
}

std::size_t CompilationCache::estimateBytes(
    const std::string& source,
    const CachedCompilation& value) {
    std::size_t bytes = source.capacity() + 1 + sizeof(Entry);
    for (const auto& statement : value.statements) {
        bytes += statement.sql.capacity() + 1;
        bytes += statement.optimizationNotes.capacity() * sizeof(std::string);
        for (const auto& note : statement.optimizationNotes) {
            bytes += note.capacity() + 1;
        }
    }
    return bytes;
}

std::uint64_t CompilationCache::hashKey(
    std::string_view source,
    std::uint64_t schemaFingerprint) {
    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    for (unsigned char ch : source) {
        hash ^= ch;
        hash *= prime;
    }
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= (schemaFingerprint >> shift) & 0xff;
        hash *= prime;
    }
    return hash;
}

std::list<CompilationCache::Entry>::iterator CompilationCache::find(
    std::string_view source,
    std::uint64_t schemaFingerprint,
    std::uint64_t hash) {
    const auto range = entries_.equal_range(hash);
    for (auto candidate = range.first;
         candidate != range.second;
         ++candidate) {
        const auto entry = candidate->second;
        if (entry->schemaFingerprint == schemaFingerprint &&
            entry->source.size() == source.size() &&
            std::memcmp(entry->source.data(),
                        source.data(),
                        source.size()) == 0) {
            return entry;
        }
    }
    return lru_.end();
}

void CompilationCache::erase(std::list<Entry>::iterator entry) {
    bytes_ -= entry->bytes;
    const auto range = entries_.equal_range(entry->hash);
    for (auto candidate = range.first;
         candidate != range.second;
         ++candidate) {
        if (candidate->second == entry) {
            entries_.erase(candidate);
            break;
        }
    }
    lru_.erase(entry);
}

void CompilationCache::evictOldest() {
    if (lru_.empty()) return;
    erase(std::prev(lru_.end()));
    ++evictions_;
}
