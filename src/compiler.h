#pragma once

#include "ast.h"
#include "cache.h"
#include "lexer.h"
#include "metadata.h"
#include "optimizer.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct CompiledStatement {
    std::unique_ptr<ASTNode> ast;
    std::string sql;
    OptimizationReport report;
};

struct CompilationResult {
    std::vector<Token> tokens;
    std::vector<CompiledStatement> statements;
    bool cacheHit = false;
    bool cacheable = false;
};

struct FastCompilationResult {
    CachedCompilationHandle output;
    CompilationResult detailed;
    bool cacheHit = false;
    bool cacheable = false;
};

class QueryCompiler {
public:
    explicit QueryCompiler(
        std::size_t cacheEntries =
            skibidi::config::defaultCompilationCacheEntries(),
        std::size_t cacheBytes =
            skibidi::config::kDefaultMaxCompilationCacheBytes);

    CompilationResult compile(const std::string& source,
                              const Catalog& catalog,
                              bool useCache = true);
    FastCompilationResult compileFast(const std::string& source,
                                      const Catalog& catalog,
                                      bool useCache = true);

    void clearCache();
    CacheStats cacheStats() const;
    void resetCacheStats();

private:
    CompilationCache cache_;

    CompilationResult compileUncached(const std::string& source,
                                      const Catalog& catalog,
                                      CachedCompilation& cacheValue);
};
