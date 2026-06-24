#pragma once

#include "ast.h"

#include <cstdint>
#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct CachedStatement {
    std::string sql;
    std::vector<std::string> optimizationNotes;
    bool schemaChanging = false;
    std::shared_ptr<const ASTNode> ast;
};

struct CachedCompilation {
    std::vector<CachedStatement> statements;
};

using CachedCompilationHandle = std::shared_ptr<const CachedCompilation>;

struct CacheStats {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
    std::size_t entries = 0;
    std::size_t bytes = 0;
};

class CompilationCache {
public:
    explicit CompilationCache(std::size_t maxEntries = 128,
                              std::size_t maxBytes = 4 * 1024 * 1024);

    bool get(std::string_view source,
             std::uint64_t schemaFingerprint,
             CachedCompilationHandle& value);
    CachedCompilationHandle put(std::string source,
                                std::uint64_t schemaFingerprint,
                                CachedCompilation value);
    void clear();
    void resetStats();

    CacheStats stats() const;

private:
    struct Entry {
        std::string source;
        std::uint64_t schemaFingerprint = 0;
        std::uint64_t hash = 0;
        CachedCompilationHandle value;
        std::size_t bytes = 0;
    };

    std::size_t maxEntries_;
    std::size_t maxBytes_;
    std::size_t bytes_ = 0;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    std::size_t evictions_ = 0;
    std::list<Entry> lru_;
    std::unordered_multimap<std::uint64_t,
                            std::list<Entry>::iterator> entries_;

    static std::uint64_t hashKey(std::string_view source,
                                 std::uint64_t schemaFingerprint);
    static std::size_t estimateBytes(const std::string& source,
                                     const CachedCompilation& value);
    std::list<Entry>::iterator find(std::string_view source,
                                    std::uint64_t schemaFingerprint,
                                    std::uint64_t hash);
    void erase(std::list<Entry>::iterator entry);
    void evictOldest();
};
