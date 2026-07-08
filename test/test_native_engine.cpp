#include "test_framework.h"

#include "lexer.h"
#include "native_engine.h"
#include "optimizer.h"
#include "parser.h"

#include <chrono>
#include <filesystem>
#include <string>

struct NativeTestDatabase {
    std::filesystem::path path;
    NativeEngine engine;

    NativeTestDatabase()
        : path(std::filesystem::temp_directory_path() /
               ("skibidi-engine-test-" +
                std::to_string(
                    std::chrono::high_resolution_clock::now()
                        .time_since_epoch().count()))),
          engine(path, 8) {}

    ~NativeTestDatabase() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

static std::vector<NativeQueryResult> executeSource(
    NativeEngine& engine,
    const std::string& source) {
    Parser parser(Lexer(source).tokenize());
    auto statements = parser.parseAll();
    std::vector<NativeQueryResult> results;
    for (auto& statement : statements) {
        OptimizationReport report;
        Optimizer optimizer(false, &engine.catalog());
        statement = optimizer.optimize(std::move(statement), report);
        results.push_back(engine.execute(statement.get()));
    }
    return results;
}

static std::string fieldValue(const NativeQueryResult& result,
                              const std::string& field) {
    for (const auto& row : result.rows) {
        if (row.size() >= 2 && row[0] == Value(field)) {
            return row[1].toString();
        }
    }
    return "";
}

TEST(native_engine_crud_filter_order_and_limit) {
    NativeTestDatabase database;
    auto results = executeSource(
        database.engine,
        "manifest users ("
        "id INTEGER main-character, "
        "name TEXT no-cap-not ghosted, "
        "score REAL);"
        "yeet-into users (id, name, score) drip "
        "(1, 'Ada', 9.5), (2, 'Grace', 8.0), (3, 'Linus', 7.0);"
        "slay id, name no-cap users only-if score >= 8 "
        "hits-different score down-bad cap-at 1;");

    ASSERT_EQ(results.size(), (size_t)3);
    ASSERT_EQ(results[1].rowsAffected, (size_t)3);
    ASSERT_EQ(results[2].rows.size(), (size_t)1);
    ASSERT_TRUE(results[2].rows[0][0] == Value((std::int64_t)1));
    ASSERT_TRUE(results[2].rows[0][1] == Value(std::string("Ada")));

    database.engine.resetStats();
    executeSource(
        database.engine,
        "slay name no-cap users only-if id = 2;");
    const auto stats = database.engine.stats();
    ASSERT_EQ(stats.indexLookups, (size_t)1);
    ASSERT_EQ(stats.tableScans, (size_t)0);
}

TEST(native_engine_join_group_and_having) {
    NativeTestDatabase database;
    auto results = executeSource(
        database.engine,
        "manifest users (id INTEGER main-character, name TEXT);"
        "manifest orders (id INTEGER main-character, "
        "user_id INTEGER, total REAL);"
        "yeet-into users drip (1, 'Ada'), (2, 'Grace');"
        "yeet-into orders drip "
        "(10, 1, 25.0), (11, 1, 50.0), (12, 2, 5.0);"
        "slay u.name, stack(o.total) lowkey spent "
        "no-cap users lowkey u "
        "link-up orders lowkey o fr-fr u.id = o.user_id "
        "vibe-check u.name "
        "bussin-only stack(o.total) > 10 "
        "hits-different spent down-bad;");

    const auto& result = results.back();
    ASSERT_EQ(result.rows.size(), (size_t)1);
    ASSERT_TRUE(result.rows[0][0] == Value(std::string("Ada")));
    ASSERT_TRUE(result.rows[0][1] == Value(75.0));
    const auto stats = database.engine.stats();
    ASSERT_TRUE(stats.hashJoinProbes > 0);
    ASSERT_TRUE(stats.bloomFilterBuilds > 0);
    ASSERT_TRUE(stats.bloomFilterChecks > 0);
    ASSERT_EQ(stats.nestedLoopComparisons, (size_t)0);
}

TEST(native_engine_bloom_filter_rejects_missing_join_keys) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest lefts (id INTEGER main-character, payload INTEGER);"
        "manifest rights (id INTEGER main-character, left_id INTEGER);"
        "yeet-into lefts drip "
        "(1, 10), (2, 20), (3, 30), (4, 40), (5, 50), "
        "(6, 60), (7, 70), (8, 80);"
        "yeet-into rights drip (100, 1000);");

    database.engine.resetStats();
    const auto result = executeSource(
        database.engine,
        "slay headcount(*) no-cap lefts lowkey l "
        "link-up rights lowkey r fr-fr l.id = r.left_id;").front();

    ASSERT_EQ(result.rows.size(), (size_t)1);
    ASSERT_TRUE(result.rows[0][0] == Value((std::int64_t)0));
    const auto stats = database.engine.stats();
    ASSERT_TRUE(stats.bloomFilterBuilds > 0);
    ASSERT_TRUE(stats.bloomFilterChecks > 0);
    ASSERT_TRUE(stats.bloomFilterRejects > 0);
    ASSERT_TRUE(stats.hashJoinProbes < stats.bloomFilterChecks);
}

TEST(native_engine_update_delete_and_constraints) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest users (id INTEGER main-character, "
        "name TEXT no-cap-not ghosted);"
        "yeet-into users drip (1, 'Ada'), (2, 'Grace');"
        "glow-up users be-like name = 'Hopper' only-if id = 2;"
        "ratio users only-if id = 1;");

    auto result = executeSource(
        database.engine,
        "slay id, name no-cap users;").front();
    ASSERT_EQ(result.rows.size(), (size_t)1);
    ASSERT_TRUE(result.rows[0][0] == Value((std::int64_t)2));
    ASSERT_TRUE(result.rows[0][1] == Value(std::string("Hopper")));

    ASSERT_THROW(
        executeSource(database.engine,
                      "yeet-into users drip (2, 'Duplicate');"),
        std::runtime_error);
    result = executeSource(
        database.engine,
        "slay headcount(*) no-cap users;").front();
    ASSERT_TRUE(result.rows[0][0] == Value((std::int64_t)1));
}

TEST(native_engine_failed_multirow_insert_rolls_back_statement) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest users (id INTEGER main-character, name TEXT);");

    ASSERT_THROW(
        executeSource(
            database.engine,
            "yeet-into users drip "
            "(1, 'Ada'), (1, 'Duplicate');"),
        std::runtime_error);

    const auto result = executeSource(
        database.engine,
        "slay headcount(*) no-cap users;").front();
    ASSERT_TRUE(result.rows[0][0] == Value((std::int64_t)0));
}

TEST(native_engine_persists_catalog_and_rows) {
    const auto path = std::filesystem::temp_directory_path() /
        ("skibidi-persistence-test-" +
         std::to_string(
             std::chrono::high_resolution_clock::now()
                 .time_since_epoch().count()));
    {
        NativeEngine engine(path, 4);
        executeSource(
            engine,
            "manifest users (id INTEGER main-character, name TEXT);"
            "yeet-into users drip (1, 'Ada');");
        engine.flush();
    }
    {
        NativeEngine engine(path, 4);
        const auto result = executeSource(
            engine,
            "slay * no-cap users;").front();
        ASSERT_EQ(result.rows.size(), (size_t)1);
        ASSERT_TRUE(result.rows[0][1] == Value(std::string("Ada")));
    }
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

TEST(native_engine_left_join_and_window_rank) {
    NativeTestDatabase database;
    auto results = executeSource(
        database.engine,
        "manifest users (id INTEGER main-character, name TEXT);"
        "manifest scores (id INTEGER main-character, "
        "user_id INTEGER, points INTEGER);"
        "yeet-into users drip (1, 'Ada'), (2, 'Grace'), (3, 'Linus');"
        "yeet-into scores drip (10, 1, 100), (11, 2, 100);"
        "slay u.name, s.points "
        "no-cap users lowkey u "
        "left-link-up scores lowkey s fr-fr u.id = s.user_id "
        "hits-different u.id;"
        "slay name, era hits-different id down-bad lowkey rank "
        "no-cap users hits-different id;");

    const auto& leftJoin = results[4];
    ASSERT_EQ(leftJoin.rows.size(), (size_t)3);
    ASSERT_TRUE(leftJoin.rows[2][1].isNull());

    const auto& ranked = results[5];
    ASSERT_EQ(ranked.rows.size(), (size_t)3);
    ASSERT_TRUE(ranked.rows[0][1] == Value((std::int64_t)3));
    ASSERT_TRUE(ranked.rows[2][1] == Value((std::int64_t)1));
}

TEST(native_engine_explicit_transaction_commit_and_rollback) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest users (id INTEGER main-character, name TEXT);"
        "yeet-into users drip (1, 'Ada');");

    database.engine.beginTransaction();
    executeSource(
        database.engine,
        "yeet-into users drip (2, 'Grace');");
    database.engine.rollbackTransaction();
    auto result = executeSource(
        database.engine,
        "slay headcount(*) no-cap users;").front();
    ASSERT_TRUE(result.rows[0][0] == Value((std::int64_t)1));

    database.engine.beginTransaction();
    executeSource(
        database.engine,
        "yeet-into users drip (2, 'Grace');");
    database.engine.commitTransaction();
    result = executeSource(
        database.engine,
        "slay headcount(*) no-cap users;").front();
    ASSERT_TRUE(result.rows[0][0] == Value((std::int64_t)2));
}

TEST(native_engine_enforces_foreign_keys_on_insert_and_delete) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest users (id INTEGER main-character, name TEXT);"
        "manifest orders ("
        "id INTEGER main-character, "
        "user_id INTEGER side-character references users(id));"
        "yeet-into users drip (1, 'Ada');"
        "yeet-into orders drip (10, 1);");

    ASSERT_THROW(
        executeSource(database.engine,
                      "ratio users only-if id = 1;"),
        std::runtime_error);
    ASSERT_THROW(
        executeSource(database.engine,
                      "yeet-into orders drip (11, 999);"),
        std::runtime_error);

    const auto result = executeSource(
        database.engine,
        "slay headcount(*) no-cap users;").front();
    ASSERT_TRUE(result.rows[0][0] == Value((std::int64_t)1));
}

TEST(native_engine_directly_scans_filtered_aggregates) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest events ("
        "id INTEGER main-character, category INTEGER, score REAL);"
        "yeet-into events drip "
        "(1, 1, 10.0), (2, 1, 20.0), (3, 2, 30.0), "
        "(4, 2, 40.0), (5, 2, 50.0);");

    database.engine.resetStats();
    auto result = executeSource(
        database.engine,
        "slay category, headcount(*), stack(score) "
        "no-cap events only-if score >= 20 "
        "vibe-check category;").front();

    ASSERT_EQ(result.rows.size(), (size_t)2);
    std::int64_t totalCount = 0;
    double totalScore = 0.0;
    for (const auto& row : result.rows) {
        totalCount += row[1].asInteger();
        totalScore += row[2].asReal();
    }
    ASSERT_EQ(totalCount, (std::int64_t)4);
    ASSERT_TRUE(totalScore == 140.0);
    const auto stats = database.engine.stats();
    ASSERT_EQ(stats.directAggregateQueries, (size_t)1);
    ASSERT_EQ(stats.rawRowsScanned, (size_t)5);
    ASSERT_EQ(stats.rowCopiesAvoided, (size_t)5);
    ASSERT_EQ(stats.decodedColumns, (size_t)10);
    ASSERT_EQ(stats.skippedColumns, (size_t)5);
}

TEST(native_engine_vectorizes_filtered_projection) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest events ("
        "id INTEGER main-character, category INTEGER, score REAL);"
        "yeet-into events drip "
        "(1, 1, 10.0), (2, 1, 20.0), (3, 2, 30.0), "
        "(4, 2, 40.0), (5, 2, 50.0);");

    database.engine.resetStats();
    auto result = executeSource(
        database.engine,
        "slay category no-cap events only-if score >= 20;").front();

    ASSERT_EQ(result.rows.size(), (size_t)4);
    const auto stats = database.engine.stats();
    ASSERT_EQ(stats.directAggregateQueries, (size_t)0);
    ASSERT_EQ(stats.vectorizedQueries, (size_t)1);
    ASSERT_EQ(stats.vectorBatches, (size_t)1);
    ASSERT_EQ(stats.vectorRows, (size_t)5);
}

TEST(native_engine_cost_based_join_enumeration_changes_bad_order) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest facts ("
        "id INTEGER main-character, d1_id INTEGER, "
        "d2_id INTEGER, amount INTEGER);"
        "manifest dimension_one ("
        "id INTEGER main-character, label TEXT);"
        "manifest dimension_two ("
        "id INTEGER main-character, label TEXT);"
        "yeet-into dimension_one drip "
        "(1, 'a'), (2, 'b'), (3, 'c');"
        "yeet-into dimension_two drip (1, 'x'), (2, 'y');"
        "yeet-into facts drip "
        "(1, 1, 1, 10), (2, 2, 1, 20), (3, 3, 1, 30), "
        "(4, 1, 2, 40), (5, 2, 2, 50), (6, 3, 2, 60);");

    database.engine.resetStats();
    const auto result = executeSource(
        database.engine,
        "slay d2.label, stack(f.amount) "
        "no-cap facts lowkey f "
        "link-up dimension_one lowkey d1 fr-fr f.d1_id = d1.id "
        "link-up dimension_two lowkey d2 fr-fr f.d2_id = d2.id "
        "vibe-check d2.label "
        "bussin-only stack(f.amount) > 0;").front();

    ASSERT_EQ(result.rows.size(), (size_t)2);
    double total = 0.0;
    for (const auto& row : result.rows) total += row[1].asReal();
    ASSERT_TRUE(total == 210.0);
    const auto stats = database.engine.stats();
    ASSERT_TRUE(stats.joinPlansEnumerated > 0);
    ASSERT_EQ(stats.joinOrderChanges, (size_t)1);
    ASSERT_TRUE(stats.estimatedJoinCost > 0.0);
    ASSERT_EQ(stats.rowIdSeekJoinQueries, (size_t)0);
    ASSERT_TRUE(stats.hashJoinProbes > 0);
    ASSERT_EQ(stats.nestedLoopComparisons, (size_t)0);
}

TEST(native_engine_rowid_seek_join_aggregates_without_hash_materialization) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest facts ("
        "id INTEGER main-character, d1_id INTEGER, "
        "d2_id INTEGER, amount INTEGER);"
        "manifest dimension_one ("
        "id INTEGER main-character, label TEXT);"
        "manifest dimension_two ("
        "id INTEGER main-character, label TEXT);"
        "yeet-into dimension_one drip "
        "(1, 'a'), (2, 'b'), (3, 'c');"
        "yeet-into dimension_two drip (1, 'x'), (2, 'y');"
        "yeet-into facts drip "
        "(1, 1, 1, 10), (2, 2, 1, 20), (3, 3, 1, 30), "
        "(4, 1, 2, 40), (5, 2, 2, 50), (6, 3, 2, 60);");

    database.engine.resetStats();
    const auto result = executeSource(
        database.engine,
        "slay d2.label, stack(f.amount) "
        "no-cap facts lowkey f "
        "link-up dimension_one lowkey d1 fr-fr f.d1_id = d1.id "
        "link-up dimension_two lowkey d2 fr-fr f.d2_id = d2.id "
        "vibe-check d2.label;").front();

    ASSERT_EQ(result.rows.size(), (size_t)2);
    double total = 0.0;
    for (const auto& row : result.rows) total += row[1].asReal();
    ASSERT_TRUE(total == 210.0);
    const auto stats = database.engine.stats();
    ASSERT_EQ(stats.rowIdSeekJoinQueries, (size_t)1);
    ASSERT_EQ(stats.rowIdSeekJoinBaseRows, (size_t)6);
    ASSERT_EQ(stats.rowIdSeekJoinLookups, (size_t)12);
    ASSERT_EQ(stats.rowIdSeekJoinMisses, (size_t)0);
    ASSERT_EQ(stats.virtualMemoryScanQueries, (size_t)1);
    ASSERT_EQ(stats.virtualMemoryRowsScanned, (size_t)6);
    ASSERT_EQ(stats.virtualMemoryRowIdReads, (size_t)6);
    ASSERT_EQ(stats.hashJoinProbes, (size_t)0);
    ASSERT_EQ(stats.joinPlansEnumerated, (size_t)0);
}

TEST(native_engine_join_domain_pruning_skips_disjoint_pk_join) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest facts ("
        "id INTEGER main-character, d1_id INTEGER, "
        "d2_id INTEGER, amount INTEGER);"
        "manifest dimension_one ("
        "id INTEGER main-character, label TEXT);"
        "manifest dimension_two ("
        "id INTEGER main-character, label TEXT);"
        "yeet-into dimension_one drip "
        "(1, 'a'), (2, 'b'), (3, 'c');"
        "yeet-into dimension_two drip (1, 'x'), (2, 'y');"
        "yeet-into facts drip "
        "(1, 1, 100, 10), (2, 2, 101, 20), "
        "(3, 3, 102, 30), (4, 1, 103, 40), "
        "(5, 2, 104, 50), (6, 3, 105, 60);");

    const std::string query =
        "slay headcount(*) "
        "no-cap facts lowkey f "
        "link-up dimension_one lowkey d1 fr-fr f.d1_id = d1.id "
        "link-up dimension_two lowkey d2 fr-fr f.d2_id = d2.id;";

    (void)executeSource(database.engine, query).front();
    database.engine.resetStats();
    const auto result = executeSource(database.engine, query).front();

    ASSERT_EQ(result.rows.size(), (size_t)1);
    ASSERT_TRUE(result.rows[0][0] == Value((std::int64_t)0));
    const auto stats = database.engine.stats();
    ASSERT_EQ(stats.rowIdSeekJoinQueries, (size_t)1);
    ASSERT_EQ(stats.joinDomainFiltersChecked, (size_t)2);
    ASSERT_EQ(stats.joinDomainScansSkipped, (size_t)1);
    ASSERT_EQ(stats.joinDomainRowsSkipped, (size_t)6);
    ASSERT_EQ(stats.rowIdSeekJoinBaseRows, (size_t)0);
    ASSERT_EQ(stats.rowIdSeekJoinLookups, (size_t)0);
    ASSERT_EQ(stats.virtualMemoryScanQueries, (size_t)0);
    ASSERT_EQ(stats.rawRowsScanned, (size_t)0);
    ASSERT_EQ(stats.hashJoinProbes, (size_t)0);
}

TEST(native_engine_projected_vectors_preserve_null_semantics) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest events ("
        "id INTEGER main-character, category INTEGER, "
        "payload TEXT, score REAL);"
        "yeet-into events drip "
        "(1, 1, 'large-unused-a', 10.0), "
        "(2, 1, 'large-unused-b', ghosted), "
        "(3, 1, 'large-unused-c', 20.0);");

    database.engine.resetStats();
    const auto result = executeSource(
        database.engine,
        "slay headcount(score), stack(score) "
        "no-cap events only-if category = 1;").front();

    ASSERT_EQ(result.rows.size(), (size_t)1);
    ASSERT_TRUE(result.rows[0][0] == Value((std::int64_t)2));
    ASSERT_TRUE(result.rows[0][1] == Value(30.0));
    const auto stats = database.engine.stats();
    ASSERT_EQ(stats.directAggregateQueries, (size_t)1);
    ASSERT_EQ(stats.rawRowsScanned, (size_t)3);
    ASSERT_EQ(stats.rowCopiesAvoided, (size_t)3);
    ASSERT_EQ(stats.decodedColumns, (size_t)6);
    ASSERT_EQ(stats.skippedColumns, (size_t)6);
    ASSERT_EQ(stats.vectorNulls, (size_t)0);
}

TEST(native_engine_minmax_skips_impossible_filtered_count) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest nums (id INTEGER main-character, value INTEGER);"
        "yeet-into nums drip (1, 10), (2, 20), (3, 30);");

    (void)executeSource(
        database.engine,
        "slay headcount(*) no-cap nums only-if id > 99;");

    database.engine.resetStats();
    const auto result = executeSource(
        database.engine,
        "slay headcount(*) no-cap nums only-if id > 99;").front();

    ASSERT_EQ(result.rows.size(), (size_t)1);
    ASSERT_TRUE(result.rows[0][0] == Value((std::int64_t)0));
    const auto stats = database.engine.stats();
    ASSERT_EQ(stats.directAggregateQueries, (size_t)1);
    ASSERT_EQ(stats.minMaxFiltersChecked, (size_t)1);
    ASSERT_EQ(stats.minMaxScansSkipped, (size_t)1);
    ASSERT_EQ(stats.minMaxRowsSkipped, (size_t)3);
    ASSERT_EQ(stats.minMaxStatisticsBuilt, (size_t)0);
    ASSERT_EQ(stats.rowsRead, (size_t)0);
    ASSERT_EQ(stats.tableScans, (size_t)0);
}

TEST(native_engine_lone_wolf_counts_normal_outliers) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest samples (id INTEGER main-character, score REAL);"
        "yeet-into samples drip "
        "(1, 0.0), (2, 0.0), (3, 0.0), (4, 0.0), "
        "(5, 0.0), (6, 0.0), (7, 0.0), (8, 0.0), "
        "(9, 0.0), (10, 0.0), (11, 100.0);");

    database.engine.resetStats();
    const auto result = executeSource(
        database.engine,
        "slay LONE-WOLF(score) no-cap samples;").front();

    ASSERT_EQ(result.rows.size(), (size_t)1);
    ASSERT_TRUE(result.rows[0][0] == Value((std::int64_t)1));
    const auto stats = database.engine.stats();
    ASSERT_EQ(stats.directAggregateQueries, (size_t)1);
}

TEST(native_engine_lone_wolf_groups_by_category) {
    NativeTestDatabase database;
    executeSource(
        database.engine,
        "manifest samples ("
        "id INTEGER main-character, category TEXT, score REAL);"
        "yeet-into samples drip "
        "(1, 'wolf', 0.0), (2, 'wolf', 0.0), "
        "(3, 'wolf', 0.0), (4, 'wolf', 0.0), "
        "(5, 'wolf', 0.0), (6, 'wolf', 0.0), "
        "(7, 'wolf', 0.0), (8, 'wolf', 0.0), "
        "(9, 'wolf', 0.0), (10, 'wolf', 0.0), "
        "(11, 'wolf', 100.0), "
        "(12, 'pack', 1.0), (13, 'pack', 2.0), "
        "(14, 'pack', 3.0), (15, 'pack', 4.0), "
        "(16, 'pack', 5.0);");

    database.engine.resetStats();
    const auto result = executeSource(
        database.engine,
        "slay category, LONE-WOLF(score) no-cap samples "
        "vibe-check category;").front();

    ASSERT_EQ(result.rows.size(), (size_t)2);
    std::int64_t wolfOutliers = -1;
    std::int64_t packOutliers = -1;
    for (const auto& row : result.rows) {
        if (row[0] == Value(std::string("wolf"))) {
            wolfOutliers = row[1].asInteger();
        } else if (row[0] == Value(std::string("pack"))) {
            packOutliers = row[1].asInteger();
        }
    }
    ASSERT_EQ(wolfOutliers, (std::int64_t)1);
    ASSERT_EQ(packOutliers, (std::int64_t)0);
    const auto stats = database.engine.stats();
    ASSERT_EQ(stats.directAggregateQueries, (size_t)1);
}

TEST(native_engine_manifests_training_snapshot_and_spills_batch) {
    NativeTestDatabase database;
    auto results = executeSource(
        database.engine,
        "manifest events ("
        "id INTEGER main-character, user_id INTEGER, age INTEGER, "
        "country TEXT, clicked INTEGER, ts TEXT);"
        "yeet-into events drip "
        "(1, 101, 20, 'us', 1, '2025-01-01'), "
        "(2, 101, 21, 'us', 0, '2025-01-02'), "
        "(3, 202, 30, 'ca', 1, '2025-01-03'), "
        "(4, 303, 40, 'uk', 0, '2025-01-04');"
        "manifest-snapshot train_v1 lowkey "
        "slay age, country, clicked, user_id no-cap events "
        "only-if ts < '2026-01-01' "
        "split-by user_id with-seed 42 "
        "features (age FLOAT NORMALIZE ZSCORE, "
        "country CATEGORICAL ENCODE DICT) "
        "label clicked INT;");

    const auto& snapshotResult = results.back();
    ASSERT_EQ(fieldValue(snapshotResult, "snapshot"), std::string("train_v1"));
    ASSERT_EQ(fieldValue(snapshotResult, "rows"), std::string("4"));
    ASSERT_CONTAINS(fieldValue(snapshotResult, "features"), "age");
    ASSERT_TRUE(database.engine.catalog().hasSnapshot("train_v1"));

    const auto exportResult = executeSource(
        database.engine,
        "ship-torch train_v1 batch-size 2 shuffle deterministic "
        "epoch 0 rank 0 world-size 1;").front();
    ASSERT_CONTAINS(fieldValue(exportResult, "python"), "TensorQLDataset");

    const auto explainResult = executeSource(
        database.engine,
        "spill-batch train_v1 batch-size 2 epoch 0 batch 0 "
        "rank 0 world-size 1;").front();
    ASSERT_EQ(fieldValue(explainResult, "snapshot"), std::string("train_v1"));
    ASSERT_CONTAINS(fieldValue(explainResult, "source_rows"), ":");
    ASSERT_CONTAINS(fieldValue(explainResult, "feature_columns"), "country");
    ASSERT_CONTAINS(fieldValue(explainResult, "resume_token"), "epoch=0");
}

TEST(native_engine_warns_when_row_split_leaks_user_id) {
    NativeTestDatabase database;
    std::string source =
        "manifest events ("
        "id INTEGER main-character, user_id INTEGER, age INTEGER, "
        "clicked INTEGER);"
        "yeet-into events drip ";
    for (int id = 1; id <= 40; ++id) {
        if (id > 1) source += ", ";
        source += "(" + std::to_string(id) + ", 7, " +
                  std::to_string(20 + id) + ", " +
                  std::to_string(id % 2) + ")";
    }
    source += ";"
        "manifest-snapshot leak_v1 lowkey "
        "slay age, clicked, user_id no-cap events "
        "split-by random by row with-seed 42 "
        "features (age FLOAT) label clicked INT;";

    const auto result = executeSource(database.engine, source).back();
    ASSERT_CONTAINS(fieldValue(result, "warning"), "user_id");
    ASSERT_CONTAINS(fieldValue(result, "warning"), "prefer split-by user_id");
}

TEST(native_engine_contextql_maintains_prompt_view_with_receipts) {
    NativeTestDatabase database;
    auto results = executeSource(
        database.engine,
        "manifest-context convo_123;"
        "yeet-memory convo_123 drip "
        "(1, 'user', 'I live in Seattle.');"
        "yeet-memory convo_123 drip "
        "(88, 'user', 'Actually I moved to NYC.');"
        "spill-context convo_123 only-if 'Find restaurants near me' "
        "token-budget 200 receipts on;");

    ASSERT_EQ(results.size(), (size_t)4);
    const auto& appendFirst = results[1];
    ASSERT_EQ(fieldValue(appendFirst, "extracted_atoms"), std::string("1"));
    ASSERT_CONTAINS(fieldValue(appendFirst, "atom"), "Seattle");

    const auto& appendSecond = results[2];
    ASSERT_EQ(fieldValue(appendSecond, "invalidated_atoms"), std::string("1"));
    ASSERT_CONTAINS(fieldValue(appendSecond, "atom"), "NYC");

    const auto& spill = results[3];
    ASSERT_CONTAINS(fieldValue(spill, "current_context"), "user_location=NYC");
    ASSERT_CONTAINS(fieldValue(spill, "current_context"), "message_88");
    ASSERT_CONTAINS(fieldValue(spill, "invalidated"), "Seattle");
    ASSERT_CONTAINS(fieldValue(spill, "invalidated"), "invalidated_by=message_88");
}

TEST(native_engine_contextql_tabs_filter_and_retag_memory) {
    NativeTestDatabase database;
    auto results = executeSource(
        database.engine,
        "manifest-context convo_123;"
        "yeet-memory convo_123 drip "
        "(1, 'user', 'My dog likes salmon.') "
        "vibe-tab 'convo about dog';"
        "yeet-memory convo_123 drip "
        "(2, 'user', 'I live in Seattle.');"
        "yeet-memory convo_123 drip "
        "(3, 'user', 'Actually I moved to NYC.');"
        "spill-context convo_123 vibe-tab 'convo about dog' "
        "only-if 'what does my dog like?' token-budget 200 receipts on;"
        "vibe-tab convo_123 message 3 'convo about dog';"
        "spill-context convo_123 vibe-tab 'main' "
        "only-if 'where am I?' token-budget 200 receipts on;"
        "spill-context convo_123 vibe-tab 'convo about dog' "
        "only-if 'dog and location' token-budget 200 receipts on;");

    ASSERT_EQ(results.size(), (size_t)8);

    const auto& dogBeforeMove = results[4];
    ASSERT_EQ(fieldValue(dogBeforeMove, "tab"),
              std::string("convo about dog"));
    ASSERT_CONTAINS(fieldValue(dogBeforeMove, "current_context"),
                    "dog_preference=salmon");
    ASSERT_TRUE(fieldValue(dogBeforeMove, "current_context")
                    .find("user_location=NYC") == std::string::npos);

    const auto& retag = results[5];
    ASSERT_EQ(fieldValue(retag, "message"), std::string("message_3"));
    ASSERT_EQ(fieldValue(retag, "retagged_atoms"), std::string("1"));

    const auto& mainAfterMove = results[6];
    ASSERT_EQ(fieldValue(mainAfterMove, "tab"), std::string("main"));
    ASSERT_CONTAINS(fieldValue(mainAfterMove, "current_context"),
                    "user_location=Seattle");
    ASSERT_TRUE(fieldValue(mainAfterMove, "current_context")
                    .find("user_location=NYC") == std::string::npos);
    ASSERT_EQ(fieldValue(mainAfterMove, "invalidated"), std::string(""));

    const auto& dogAfterMove = results[7];
    ASSERT_CONTAINS(fieldValue(dogAfterMove, "current_context"),
                    "dog_preference=salmon");
    ASSERT_CONTAINS(fieldValue(dogAfterMove, "current_context"),
                    "user_location=NYC");
}

int main() {
    return run_all_tests("Native engine");
}
