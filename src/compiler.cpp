#include "compiler.h"

#include "codegen.h"
#include "parser.h"

#include <utility>

QueryCompiler::QueryCompiler(std::size_t cacheEntries, std::size_t cacheBytes)
    : cache_(cacheEntries, cacheBytes) {}

CompilationResult QueryCompiler::compile(const std::string& source,
                                         const Catalog& catalog,
                                         bool useCache) {
    const std::uint64_t schemaFingerprint =
        catalog.schemaFingerprint();

    if (useCache) {
        CachedCompilationHandle cached;
        if (cache_.get(source, schemaFingerprint, cached)) {
            CompilationResult result;
            result.cacheHit = true;
            result.cacheable = true;
            result.statements.reserve(cached->statements.size());
            for (const auto& cachedStatement : cached->statements) {
                CompiledStatement statement;
                statement.sql = cachedStatement.sql;
                statement.report.notes =
                    cachedStatement.optimizationNotes;
                result.statements.push_back(std::move(statement));
            }
            return result;
        }
    }

    CachedCompilation cacheValue;
    CompilationResult result =
        compileUncached(source, catalog, cacheValue);
    if (useCache && result.cacheable) {
        cache_.put(source, schemaFingerprint, std::move(cacheValue));
    }
    return result;
}

FastCompilationResult QueryCompiler::compileFast(
    const std::string& source,
    const Catalog& catalog,
    bool useCache) {
    FastCompilationResult result;
    const std::uint64_t schemaFingerprint =
        catalog.schemaFingerprint();

    if (useCache &&
        cache_.get(source, schemaFingerprint, result.output)) {
        result.cacheHit = true;
        result.cacheable = true;
        return result;
    }

    CachedCompilation cacheValue;
    result.detailed = compileUncached(source, catalog, cacheValue);
    result.cacheable = result.detailed.cacheable;
    if (useCache && result.cacheable) {
        result.output = cache_.put(
            source, schemaFingerprint, std::move(cacheValue));
    } else {
        result.output = std::make_shared<const CachedCompilation>(
            std::move(cacheValue));
    }
    return result;
}

CompilationResult QueryCompiler::compileUncached(
    const std::string& source,
    const Catalog& catalog,
    CachedCompilation& cacheValue) {
    CompilationResult result;
    result.tokens = Lexer(source).tokenize();
    Parser parser(result.tokens);
    auto statements = parser.parseAll();
    result.statements.reserve(statements.size());

    bool schemaChanging = false;
    for (auto& ast : statements) {
        const bool statementChangesSchema =
            dynamic_cast<CreateStmt*>(ast.get()) ||
            dynamic_cast<DropStmt*>(ast.get());
        if (statementChangesSchema) {
            schemaChanging = true;
        }

        CompiledStatement statement;
        Optimizer optimizer(false, &catalog);
        statement.ast =
            optimizer.optimize(std::move(ast), statement.report);

        CodeGen codegen;
        statement.sql = codegen.generate(statement.ast.get());

        CachedStatement cachedStatement;
        cachedStatement.sql = statement.sql;
        cachedStatement.optimizationNotes = statement.report.notes;
        cachedStatement.schemaChanging = statementChangesSchema;
        cachedStatement.ast = std::shared_ptr<const ASTNode>(
            statement.ast->clone().release());
        cacheValue.statements.push_back(std::move(cachedStatement));
        result.statements.push_back(std::move(statement));
    }

    result.cacheable = !schemaChanging && !result.statements.empty();
    return result;
}

void QueryCompiler::clearCache() {
    cache_.clear();
}

CacheStats QueryCompiler::cacheStats() const {
    return cache_.stats();
}

void QueryCompiler::resetCacheStats() {
    cache_.resetStats();
}
