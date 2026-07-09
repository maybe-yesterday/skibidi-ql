#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "compiler.h"
#include "native_engine.h"
#include "parser.h"
#include "skibidi_config.h"

#ifdef SKIBIDI_WITH_SQLITE
#include "executor.h"
#include <sqlite3.h>
#endif

enum class EngineKind { Native, SQLite };

struct Options {
    std::string filePath;
    std::string dbPath = ".skibidi_db";
    EngineKind engine = EngineKind::Native;
    bool verbose = false;
    bool transpileOnly = false;
    bool cacheEnabled = true;
    bool cacheStats = false;
    std::size_t cacheEntries =
        skibidi::config::defaultCompilationCacheEntries();
    std::size_t bufferPages =
        skibidi::config::defaultBufferPoolPages();
#ifdef SKIBIDI_WITH_SQLITE
    bool statementCacheEnabled = true;
    std::size_t statementCacheEntries =
        skibidi::config::defaultSqliteStatementCacheEntries();
#endif
};

struct Runtime {
    EngineKind engine = EngineKind::Native;
    NativeEngine* native = nullptr;
#ifdef SKIBIDI_WITH_SQLITE
    sqlite3* sqlite = nullptr;
    SqliteExecutor* sqliteExecutor = nullptr;
    Catalog* sqliteCatalog = nullptr;
#endif

    Catalog& catalog() {
        if (engine == EngineKind::Native) return native->catalog();
#ifdef SKIBIDI_WITH_SQLITE
        return *sqliteCatalog;
#else
        throw std::runtime_error("SQLite backend is not built");
#endif
    }
};

static bool isBlank(const std::string& text) {
    for (unsigned char ch : text) {
        if (!std::isspace(ch)) return false;
    }
    return true;
}

static void printNativeResult(const NativeQueryResult& result) {
    for (const auto& row : result.rows) {
        for (std::size_t index = 0; index < row.size(); ++index) {
            if (index > 0) std::cout << " | ";
            const std::string name = index < result.columns.size()
                ? result.columns[index]
                : "column" + std::to_string(index + 1);
            std::cout << name << "=" << row[index].toString();
        }
        std::cout << "\n";
    }
}

#ifdef SKIBIDI_WITH_SQLITE
static int sqliteCallback(void*, int count, char** values, char** names) {
    for (int index = 0; index < count; ++index) {
        if (index > 0) std::cout << " | ";
        std::cout << (names[index] ? names[index] : "?") << "="
                  << (values[index] ? values[index] : "NULL");
    }
    std::cout << "\n";
    return 0;
}

static bool updateSqliteCatalog(Catalog& catalog, const ASTNode* statement) {
    if (auto* create = dynamic_cast<const CreateStmt*>(statement)) {
        TableMeta table;
        table.name = create->table;
        for (const auto& definition : create->columns) {
            ColumnMeta column;
            column.name = definition.name;
            column.type = definition.type;
            column.primary_key = definition.primary_key;
            column.not_null = definition.not_null;
            column.fk_table = definition.fk_table;
            column.fk_col = definition.fk_col;
            table.columns.push_back(std::move(column));
        }
        catalog.addTable(table);
        catalog.save();
        return true;
    }
    if (auto* drop = dynamic_cast<const DropStmt*>(statement)) {
        catalog.removeTable(drop->table);
        catalog.save();
        return true;
    }
    return false;
}
#endif

static bool processQuery(const std::string& queryText,
                         const Options& options,
                         Runtime& runtime,
                         QueryCompiler& compiler) {
    if (isBlank(queryText)) return true;

    try {
        FastCompilationResult compilation = compiler.compileFast(
            queryText, runtime.catalog(), options.cacheEnabled);

        if (options.verbose) {
            if (compilation.cacheHit) {
                std::cout << "=== CACHE ===\n  compilation cache hit\n";
            } else {
                std::cout << "=== TOKENS ===\n";
                for (const auto& token : compilation.detailed.tokens) {
                    if (token.type != TokenType::EOF_TOKEN) {
                        std::cout << "  [" << token.line << ":" << token.col
                                  << "] " << token.value << "\n";
                    }
                }
            }
        }

        for (std::size_t index = 0;
             index < compilation.output->statements.size();
             ++index) {
            const auto& cached = compilation.output->statements[index];
            const ASTNode* ast = cached.ast.get();
            if (!ast) {
                throw std::runtime_error(
                    "Compiled statement is missing its execution plan");
            }

            if (options.verbose) {
                std::cout << "=== AST ===\n";
                ast->print(std::cout);
                if (!cached.optimizationNotes.empty()) {
                    std::cout << "=== OPTIMIZER ===\n";
                    for (const auto& note : cached.optimizationNotes) {
                        std::cout << "  - " << note << "\n";
                    }
                }
            }

            if (options.verbose || options.transpileOnly) {
                std::cout << "=== SQL ===\n" << cached.sql << "\n";
            }
            if (options.transpileOnly) continue;

            if (runtime.engine == EngineKind::Native) {
                const auto result = runtime.native->execute(ast);
                printNativeResult(result);
                if (cached.schemaChanging) compiler.clearCache();
                continue;
            }

#ifdef SKIBIDI_WITH_SQLITE
            std::string error;
            const bool cacheStatement =
                options.statementCacheEnabled && !cached.schemaChanging;
            if (!runtime.sqliteExecutor->execute(
                    cached.sql, sqliteCallback, nullptr,
                    cacheStatement, &error)) {
                std::cerr << "SQLite error: " << error << "\n";
                return false;
            }
            if (cached.schemaChanging) {
                runtime.sqliteExecutor->clear();
                updateSqliteCatalog(*runtime.sqliteCatalog, ast);
                compiler.clearCache();
            }
#else
            throw std::runtime_error("SQLite backend is not built");
#endif
        }
        return true;
    } catch (const LexerError& error) {
        std::cerr << "Lexer error: " << error.what() << "\n";
    } catch (const ParseError& error) {
        std::cerr << "Parse error: " << error.what() << "\n";
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
    }
    return false;
}

static bool processFile(const std::string& path,
                        const Options& options,
                        Runtime& runtime,
                        QueryCompiler& compiler) {
    std::ifstream input(path);
    if (!input.is_open()) {
        std::cerr << "Error: Cannot open file: " << path << "\n";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return processQuery(contents.str(), options, runtime, compiler);
}

static void runRepl(const Options& options,
                    Runtime& runtime,
                    QueryCompiler& compiler) {
    std::cout << "SkibidiQL REPL v2.0.0 ("
              << (runtime.engine == EngineKind::Native
                      ? "native engine" : "SQLite")
              << ")\n";
    std::cout << "End queries with ';' or type 'exit'.\n\n";

    std::string accumulated;
    while (true) {
        std::cout << (accumulated.empty() ? "skibidi> " : "      -> ");
        std::cout.flush();
        std::string line;
        if (!std::getline(std::cin, line)) break;

        std::string trimmed = line;
        const auto first = trimmed.find_first_not_of(" \t\r\n");
        const auto last = trimmed.find_last_not_of(" \t\r\n");
        trimmed = first == std::string::npos
            ? "" : trimmed.substr(first, last - first + 1);
        if (trimmed == "exit" || trimmed == "quit" ||
            trimmed == ".exit") {
            break;
        }
        if (runtime.engine == EngineKind::Native &&
            (trimmed == ".begin" || trimmed == ".commit" ||
             trimmed == ".rollback")) {
            try {
                if (trimmed == ".begin") {
                    runtime.native->beginTransaction();
                } else if (trimmed == ".commit") {
                    runtime.native->commitTransaction();
                } else {
                    runtime.native->rollbackTransaction();
                    compiler.clearCache();
                }
                std::cout << trimmed.substr(1) << " ok\n";
            } catch (const std::exception& error) {
                std::cerr << "Transaction error: " << error.what() << "\n";
            }
            continue;
        }

        accumulated += line + "\n";
        if (accumulated.find(';') != std::string::npos) {
            processQuery(accumulated, options, runtime, compiler);
            accumulated.clear();
        }
    }
    std::cout << "\nBye!\n";
}

static void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n\n"
        << "Options:\n"
        << "  --file <path>       Execute a .skql file\n"
        << "  --db <path>         Native database directory "
           "(default: .skibidi_db)\n"
        << "  --engine native     Use the built-in page/heap engine (default)\n"
#ifdef SKIBIDI_WITH_SQLITE
        << "  --engine sqlite     Use optional SQLite compatibility backend\n"
#endif
        << "  --buffer-pages <n>  Native buffer-pool capacity "
           "(default: env SKIBIDI_BUFFER_PAGES or "
        << skibidi::config::kDefaultBufferPoolPages << ")\n"
        << "  --verbose           Print tokens, AST, optimizer report, and SQL\n"
        << "  --transpile-only    Generate SQL without executing\n"
        << "  --no-cache          Disable compilation cache\n"
        << "  --cache-entries <n> Maximum compiled plans "
           "(default: env SKIBIDI_CACHE_ENTRIES or "
        << skibidi::config::kDefaultCompilationCacheEntries << ")\n"
        << "  --cache-stats       Print runtime cache and engine statistics\n"
#ifdef SKIBIDI_WITH_SQLITE
        << "  --no-statement-cache\n"
        << "                      Disable SQLite prepared-statement reuse\n"
        << "  --statement-cache-entries <n>\n"
        << "                      Maximum SQLite prepared statements "
           "(default: env SKIBIDI_STATEMENT_CACHE_ENTRIES or "
        << skibidi::config::kDefaultSqliteStatementCacheEntries << ")\n"
#endif
        << "  --help              Print this help\n"
        << "\nEnvironment tuning knobs:\n"
        << "  SKIBIDI_BUFFER_PAGES, SKIBIDI_CACHE_ENTRIES,\n"
#ifdef SKIBIDI_WITH_SQLITE
        << "  SKIBIDI_STATEMENT_CACHE_ENTRIES,\n"
#endif
        << "  SKIBIDI_VECTOR_BATCH_ROWS, SKIBIDI_BLOOM_MIN_BITS,\n"
        << "  SKIBIDI_BLOOM_BITS_PER_VALUE, "
           "SKIBIDI_EXACT_VALUE_COUNT_LIMIT\n";
}

int main(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--file" && index + 1 < argc) {
            options.filePath = argv[++index];
        } else if (argument == "--db" && index + 1 < argc) {
            options.dbPath = argv[++index];
        } else if (argument == "--engine" && index + 1 < argc) {
            const std::string engine = argv[++index];
            if (engine == "native") options.engine = EngineKind::Native;
            else if (engine == "sqlite") options.engine = EngineKind::SQLite;
            else {
                std::cerr << "Unknown engine: " << engine << "\n";
                return 1;
            }
        } else if (argument == "--buffer-pages" && index + 1 < argc) {
            options.bufferPages =
                static_cast<std::size_t>(std::stoull(argv[++index]));
        } else if (argument == "--cache-entries" && index + 1 < argc) {
            options.cacheEntries =
                static_cast<std::size_t>(std::stoull(argv[++index]));
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else if (argument == "--transpile-only") {
            options.transpileOnly = true;
        } else if (argument == "--no-cache") {
            options.cacheEnabled = false;
        } else if (argument == "--cache-stats") {
            options.cacheStats = true;
#ifdef SKIBIDI_WITH_SQLITE
        } else if (argument == "--no-statement-cache") {
            options.statementCacheEnabled = false;
        } else if (argument == "--statement-cache-entries" &&
                   index + 1 < argc) {
            options.statementCacheEntries =
                static_cast<std::size_t>(std::stoull(argv[++index]));
#endif
        } else if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argument << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

#ifndef SKIBIDI_WITH_SQLITE
    if (options.engine == EngineKind::SQLite) {
        std::cerr << "This build does not include SQLite. Configure with "
                     "-DSKIBIDI_WITH_SQLITE=ON to enable it.\n";
        return 1;
    }
#endif

    Runtime runtime;
    runtime.engine = options.engine;
    std::unique_ptr<NativeEngine> native;
#ifdef SKIBIDI_WITH_SQLITE
    sqlite3* sqlite = nullptr;
    std::unique_ptr<SqliteExecutor> sqliteExecutor;
    std::unique_ptr<Catalog> sqliteCatalog;
#endif

    if (options.engine == EngineKind::Native) {
        native = std::make_unique<NativeEngine>(
            options.dbPath, options.bufferPages);
        runtime.native = native.get();
    }
#ifdef SKIBIDI_WITH_SQLITE
    else {
        const std::string sqlitePath =
            options.dbPath == ".skibidi_db" ? ":memory:" : options.dbPath;
        if (sqlite3_open(sqlitePath.c_str(), &sqlite) != SQLITE_OK) {
            std::cerr << "Cannot open SQLite database: "
                      << sqlite3_errmsg(sqlite) << "\n";
            if (sqlite) sqlite3_close(sqlite);
            return 1;
        }
        sqliteCatalog = std::make_unique<Catalog>(
            sqlitePath == ":memory:"
                ? Catalog::CATALOG_FILE
                : sqlitePath + ".catalog.json");
        sqliteCatalog->load();
        sqliteExecutor = std::make_unique<SqliteExecutor>(
            sqlite, options.statementCacheEntries);
        runtime.sqlite = sqlite;
        runtime.sqliteExecutor = sqliteExecutor.get();
        runtime.sqliteCatalog = sqliteCatalog.get();
    }
#endif

    QueryCompiler compiler(options.cacheEntries);
    int exitCode = 0;
    if (!options.filePath.empty()) {
        if (!processFile(options.filePath, options, runtime, compiler)) {
            exitCode = 1;
        }
    } else if (options.transpileOnly) {
        std::ostringstream input;
        input << std::cin.rdbuf();
        if (!processQuery(input.str(), options, runtime, compiler)) {
            exitCode = 1;
        }
    } else {
        runRepl(options, runtime, compiler);
    }

    if (options.cacheStats) {
        const auto cache = compiler.cacheStats();
        std::cout << "compile-cache hits=" << cache.hits
                  << " misses=" << cache.misses
                  << " evictions=" << cache.evictions
                  << " entries=" << cache.entries
                  << " bytes=" << cache.bytes << "\n";
        if (native) {
            const auto stats = native->stats();
            std::cout << "native resident-pages=" << stats.residentPages
                      << " buffer-capacity-pages="
                      << stats.bufferCapacityPages
                      << " buffer-page-reads="
                      << stats.bufferPageReads
                      << " buffer-evictions="
                      << stats.bufferEvictions
                      << " table-scans=" << stats.tableScans
                      << " rows-read=" << stats.rowsRead
                      << " rows-written=" << stats.rowsWritten
                      << " index-lookups=" << stats.indexLookups
                      << " bloom-checks=" << stats.bloomFilterChecks
                      << " bloom-rejects=" << stats.bloomFilterRejects
                      << " minmax-skips=" << stats.minMaxScansSkipped
                      << " minmax-rows-skipped="
                      << stats.minMaxRowsSkipped
                      << " streaming-aggregates="
                      << stats.streamingAggregateQueries
                      << " rowid-seek-joins="
                      << stats.rowIdSeekJoinQueries
                      << " rowid-seek-lookups="
                      << stats.rowIdSeekJoinLookups
                      << " rowid-seek-misses="
                      << stats.rowIdSeekJoinMisses
                      << " vm-scans="
                      << stats.virtualMemoryScanQueries
                      << " vm-rows="
                      << stats.virtualMemoryRowsScanned
                      << " vm-rowid-reads="
                      << stats.virtualMemoryRowIdReads
                      << " join-domain-skips="
                      << stats.joinDomainScansSkipped
                      << " join-domain-rows-skipped="
                      << stats.joinDomainRowsSkipped
                      << " hash-join-probes=" << stats.hashJoinProbes
                      << " join-comparisons="
                      << stats.nestedLoopComparisons << "\n";
        }
#ifdef SKIBIDI_WITH_SQLITE
        if (sqliteExecutor) {
            const auto stats = sqliteExecutor->stats();
            std::cout << "statement-cache hits=" << stats.hits
                      << " misses=" << stats.misses
                      << " entries=" << stats.entries
                      << " bytes=" << stats.bytes << "\n";
        }
#endif
    }

    if (native) native->flush();
#ifdef SKIBIDI_WITH_SQLITE
    sqliteExecutor.reset();
    if (sqlite) sqlite3_close(sqlite);
#endif
    return exitCode;
}
