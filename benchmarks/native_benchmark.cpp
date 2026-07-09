#include "lexer.h"
#include "native_engine.h"
#include "optimizer.h"
#include "parser.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

struct Options {
    std::string workload = "point";
    int iterations = 10000;
    int rows = 10000;
    int bufferPages = 1024;
};

static std::unique_ptr<ASTNode> parseOne(const std::string& source,
                                         const Catalog& catalog) {
    Parser parser(Lexer(source).tokenize());
    auto statements = parser.parseAll();
    if (statements.size() != 1) {
        throw std::runtime_error("Benchmark query must contain one statement");
    }
    OptimizationReport report;
    Optimizer optimizer(false, &catalog);
    return optimizer.optimize(std::move(statements.front()), report);
}

static void seed(NativeEngine& engine, int rowCount) {
    CreateStmt create;
    create.table = "users";
    create.columns = {
        {"id", "INTEGER", true, true, "", ""},
        {"name", "TEXT", false, true, "", ""},
        {"active", "INTEGER", false, true, "", ""},
        {"category", "INTEGER", false, true, "", ""},
        {"score", "REAL", false, true, "", ""}
    };
    engine.execute(&create);

    constexpr int batchSize = 500;
    for (int first = 1; first <= rowCount; first += batchSize) {
        InsertStmt insert;
        insert.table = "users";
        const int last = std::min(rowCount, first + batchSize - 1);
        for (int id = first; id <= last; ++id) {
            std::vector<std::unique_ptr<ASTNode>> row;
            row.push_back(Literal::makeInt(id));
            row.push_back(Literal::makeString(
                "user-" + std::to_string(id)));
            row.push_back(Literal::makeInt(id % 2));
            row.push_back(Literal::makeInt(id % 20));
            row.push_back(Literal::makeFloat(
                static_cast<double>(id % 1000) / 10.0));
            insert.valueRows.push_back(std::move(row));
        }
        engine.execute(&insert);
    }
    engine.flush();
}

static bool isContextWorkload(const std::string& workload) {
    return workload == "prompt" ||
           workload == "context_schema" ||
           workload == "context_spill" ||
           workload == "context_spill_acl" ||
           workload == "context_objects";
}

static void seedPromptLog(NativeEngine& engine,
                          int messageCount,
                          bool includeSensitive = false) {
    CreateContextStmt create;
    create.name = "bench_convo";
    engine.execute(&create);

    AliasTabStmt alias;
    alias.context = "bench_convo";
    alias.alias = "dog";
    alias.target = "convo about dog";
    engine.execute(&alias);

    for (int id = 1; id <= messageCount; ++id) {
        AppendMemoryStmt append;
        append.context = "bench_convo";
        append.messageId = static_cast<unsigned long long>(id);
        append.speaker = "user";
        if (includeSensitive && id % 7 == 0) {
            append.text = "Never share passwords or api key tokens.";
            append.tab = "constraints";
            engine.execute(&append);
            continue;
        }
        switch (id % 5) {
            case 0:
                append.text = "My dog likes salmon.";
                append.autoTab = true;
                break;
            case 1:
                append.text = "I like cat cafes.";
                append.tab = "pet stuff";
                break;
            case 2:
                append.text =
                    "Decision: keep prompt views inside SkibidiQL.";
                append.tab = "project roadmap";
                break;
            case 3:
                append.text =
                    "Debug this later: sqlite perf join misses.";
                append.autoTab = true;
                break;
            default:
                append.text = "I live in Seattle.";
                break;
        }
        engine.execute(&append);
    }

    MergeTabsStmt merge;
    merge.context = "bench_convo";
    merge.fromTab = "pet stuff";
    merge.toTab = "dog";
    engine.execute(&merge);
    engine.flush();
}

static Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--workload") == 0 &&
            index + 1 < argc) {
            options.workload = argv[++index];
        } else if (std::strcmp(argv[index], "--iterations") == 0 &&
                   index + 1 < argc) {
            options.iterations = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--rows") == 0 &&
                   index + 1 < argc) {
            options.rows = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--buffer-pages") == 0 &&
                   index + 1 < argc) {
            options.bufferPages = std::atoi(argv[++index]);
        } else {
            throw std::runtime_error("Invalid benchmark argument");
        }
    }
    if (options.rows <= 0 || options.iterations <= 0 ||
        options.bufferPages <= 0) {
        throw std::runtime_error("Benchmark values must be positive");
    }
    return options;
}

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        const auto root = std::filesystem::temp_directory_path() /
            ("skibidi-native-benchmark-" +
             std::to_string(
                 std::chrono::high_resolution_clock::now()
                     .time_since_epoch().count()));
        auto engine = std::make_unique<NativeEngine>(
            root, static_cast<std::size_t>(options.bufferPages));
        if (isContextWorkload(options.workload)) {
            if (options.workload != "context_schema") {
                seedPromptLog(*engine, options.rows,
                              options.workload == "context_spill_acl" ||
                              options.workload == "context_objects");
            }
        } else {
            seed(*engine, options.rows);
        }

        std::string source;
        if (options.workload == "point") {
            source = "slay id, name no-cap users only-if id = " +
                     std::to_string(options.rows / 2) + ";";
        } else if (options.workload == "scan") {
            source =
                "slay headcount(name) no-cap users only-if active = 1;";
        } else if (options.workload == "aggregate") {
            source =
                "slay category, stack(score) no-cap users "
                "only-if active = 1 vibe-check category;";
        } else if (options.workload == "prompt") {
            source =
                "spill-context bench_convo vibe-tab 'dog' "
                "only-if 'pet preferences' token-budget 512 receipts on;";
        } else if (options.workload == "context_schema") {
            source = "show-context-schemas;";
        } else if (options.workload == "context_spill") {
            source =
                "spill-context bench_convo vibe-tab 'dog' "
                "only-if 'pet preferences' token-budget 512 receipts on;";
        } else if (options.workload == "context_spill_acl") {
            source =
                "spill-context bench_convo "
                "only-if 'constraints password policy' "
                "token-budget 512 receipts on;";
        } else if (options.workload == "context_objects") {
            source = "show-context-objects bench_convo;";
        } else {
            throw std::runtime_error(
                "Unknown workload: " + options.workload);
        }
        auto query = parseOne(source, engine->catalog());

        (void)engine->execute(query.get());
        engine->resetStats();
        std::uint64_t checksum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int iteration = 0;
             iteration < options.iterations;
             ++iteration) {
            const auto result = engine->execute(query.get());
            checksum += result.rows.size();
            for (const auto& row : result.rows) {
                for (const auto& value : row) {
                    checksum += value.toString().size();
                }
            }
        }
        const auto finish = std::chrono::steady_clock::now();
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(
                finish - start).count();
        const auto stats = engine->stats();

        std::cout << std::fixed << std::setprecision(3)
                  << "{"
#ifdef NDEBUG
                  << "\"release_build\":true,"
#else
                  << "\"release_build\":false,"
#endif
                  << "\"workload\":\"" << options.workload << "\","
                  << "\"iterations\":" << options.iterations << ","
                  << "\"rows\":" << options.rows << ","
                  << "\"elapsed_ms\":" << elapsedMs << ","
                  << "\"ops_per_sec\":"
                  << options.iterations * 1000.0 / elapsedMs << ","
                  << "\"resident_pages\":" << stats.residentPages << ","
                  << "\"estimated_memory_bytes\":"
                  << stats.estimatedMemoryBytes << ","
                  << "\"buffer_capacity_pages\":"
                  << stats.bufferCapacityPages << ","
                  << "\"buffer_page_reads\":"
                  << stats.bufferPageReads << ","
                  << "\"buffer_evictions\":"
                  << stats.bufferEvictions << ","
                  << "\"table_scans\":" << stats.tableScans << ","
                  << "\"rows_read\":" << stats.rowsRead << ","
                  << "\"index_lookups\":" << stats.indexLookups << ","
                  << "\"vectorized_queries\":"
                  << stats.vectorizedQueries << ","
                  << "\"vector_batches\":" << stats.vectorBatches << ","
                  << "\"decoded_columns\":"
                  << stats.decodedColumns << ","
                  << "\"skipped_columns\":"
                  << stats.skippedColumns << ","
                  << "\"vector_nulls\":"
                  << stats.vectorNulls << ","
                  << "\"raw_point_queries\":"
                  << stats.rawPointQueries << ","
                  << "\"raw_point_hits\":"
                  << stats.rawPointHits << ","
                  << "\"direct_aggregate_queries\":"
                  << stats.directAggregateQueries << ","
                  << "\"value_count_queries\":"
                  << stats.valueCountQueries << ","
                  << "\"value_count_rows_answered\":"
                  << stats.valueCountRowsAnswered << ","
                  << "\"dense_group_aggregate_queries\":"
                  << stats.denseGroupAggregateQueries << ","
                  << "\"dense_group_aggregate_rows\":"
                  << stats.denseGroupAggregateRows << ","
                  << "\"raw_rows_scanned\":"
                  << stats.rawRowsScanned << ","
                  << "\"row_copies_avoided\":"
                  << stats.rowCopiesAvoided << ","
                  << "\"min_max_filters_checked\":"
                  << stats.minMaxFiltersChecked << ","
                  << "\"min_max_scans_skipped\":"
                  << stats.minMaxScansSkipped << ","
                  << "\"min_max_rows_skipped\":"
                  << stats.minMaxRowsSkipped << ","
                  << "\"min_max_statistics_built\":"
                  << stats.minMaxStatisticsBuilt << ","
                  << "\"min_max_build_rows\":"
                  << stats.minMaxBuildRows << ","
                  << "\"streaming_aggregate_queries\":"
                  << stats.streamingAggregateQueries << ","
                  << "\"streaming_aggregate_rows\":"
                  << stats.streamingAggregateRows << ","
                  << "\"rowid_seek_join_queries\":"
                  << stats.rowIdSeekJoinQueries << ","
                  << "\"rowid_seek_join_base_rows\":"
                  << stats.rowIdSeekJoinBaseRows << ","
                  << "\"rowid_seek_join_lookups\":"
                  << stats.rowIdSeekJoinLookups << ","
                  << "\"rowid_seek_join_misses\":"
                  << stats.rowIdSeekJoinMisses << ","
                  << "\"virtual_memory_scan_queries\":"
                  << stats.virtualMemoryScanQueries << ","
                  << "\"virtual_memory_rows_scanned\":"
                  << stats.virtualMemoryRowsScanned << ","
                  << "\"virtual_memory_rowid_reads\":"
                  << stats.virtualMemoryRowIdReads << ","
                  << "\"join_domain_filters_checked\":"
                  << stats.joinDomainFiltersChecked << ","
                  << "\"join_domain_scans_skipped\":"
                  << stats.joinDomainScansSkipped << ","
                  << "\"join_domain_rows_skipped\":"
                  << stats.joinDomainRowsSkipped << ","
                  << "\"join_plans_enumerated\":"
                  << stats.joinPlansEnumerated << ","
                  << "\"join_order_changes\":"
                  << stats.joinOrderChanges << ","
                  << "\"bloom_filter_builds\":"
                  << stats.bloomFilterBuilds << ","
                  << "\"bloom_filter_checks\":"
                  << stats.bloomFilterChecks << ","
                  << "\"bloom_filter_rejects\":"
                  << stats.bloomFilterRejects << ","
                  << "\"context_schema_queries\":"
                  << stats.contextSchemaQueries << ","
                  << "\"context_spill_queries\":"
                  << stats.contextSpillQueries << ","
                  << "\"context_object_queries\":"
                  << stats.contextObjectQueries << ","
                  << "\"context_cache_hits\":"
                  << stats.contextCacheHits << ","
                  << "\"context_cache_misses\":"
                  << stats.contextCacheMisses << ","
                  << "\"context_atoms_scored\":"
                  << stats.contextAtomsScored << ","
                  << "\"context_atoms_rendered\":"
                  << stats.contextAtomsRendered << ","
                  << "\"context_atoms_redacted\":"
                  << stats.contextAtomsRedacted << ","
                  << "\"checksum\":" << checksum
                  << "}\n";

        engine.reset();
        std::error_code error;
        std::filesystem::remove_all(root, error);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native benchmark error: " << error.what() << "\n";
        return 1;
    }
}
