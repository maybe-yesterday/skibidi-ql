#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <cstring>

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "metadata.h"
#include "optimizer.h"
#include "codegen.h"

#include <sqlite3.h>

// -----------------------------------------------------------------------
// Options
// -----------------------------------------------------------------------
struct Options {
    std::string filePath;
    std::string dbPath = ":memory:";
    bool verbose = false;
    bool transpileOnly = false;
    bool repl = false;
};

// -----------------------------------------------------------------------
// SQLite execution
// -----------------------------------------------------------------------
static int sqliteCallback(void* /* unused */, int numCols, char** colVals, char** colNames) {
    for (int i = 0; i < numCols; ++i) {
        if (i > 0) std::cout << " | ";
        std::cout << (colNames[i] ? colNames[i] : "?") << "=";
        std::cout << (colVals[i] ? colVals[i] : "NULL");
    }
    std::cout << "\n";
    return 0;
}

static bool executeSql(sqlite3* db, const std::string& sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), sqliteCallback, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQLite error: " << (errMsg ? errMsg : "unknown") << "\n";
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------
// Update catalog based on statement
// -----------------------------------------------------------------------
static void updateCatalog(Catalog& catalog, const ASTNode* node) {
    if (auto* cs = dynamic_cast<const CreateStmt*>(node)) {
        TableMeta tm;
        tm.name = cs->table;
        for (auto& cd : cs->columns) {
            ColumnMeta cm;
            cm.name = cd.name;
            cm.type = cd.type;
            cm.primary_key = cd.primary_key;
            cm.not_null = cd.not_null;
            cm.fk_table = cd.fk_table;
            cm.fk_col = cd.fk_col;
            tm.columns.push_back(cm);
        }
        catalog.addTable(tm);
        catalog.save();
    } else if (auto* ds = dynamic_cast<const DropStmt*>(node)) {
        catalog.removeTable(ds->table);
        catalog.save();
    }
}

// -----------------------------------------------------------------------
// Process a single query string through the full pipeline
// -----------------------------------------------------------------------
static bool processQuery(const std::string& queryText, const Options& opts,
                         Catalog& catalog, sqlite3* db) {
    // Skip empty queries
    bool allSpace = true;
    for (char c : queryText) {
        if (!std::isspace((unsigned char)c)) { allSpace = false; break; }
    }
    if (allSpace) return true;

    try {
        // 1. Lex
        Lexer lexer(queryText);
        std::vector<Token> tokens = lexer.tokenize();

        if (opts.verbose) {
            std::cout << "=== TOKENS ===\n";
            for (auto& t : tokens) {
                if (t.type != TokenType::EOF_TOKEN) {
                    std::cout << "  [" << t.line << ":" << t.col << "] "
                              << t.value << "\n";
                }
            }
        }

        // 2. Parse
        Parser parser(tokens);
        std::vector<std::unique_ptr<ASTNode>> stmts = parser.parseAll();

        for (auto& stmt : stmts) {
            if (!stmt) continue;

            if (opts.verbose) {
                std::cout << "=== AST ===\n";
                stmt->print(std::cout, 0);
            }

            // 3. Metadata update (catalog)
            updateCatalog(catalog, stmt.get());

            // 4. Optimize
            OptimizationReport report;
            Optimizer optimizer(opts.verbose);
            stmt = optimizer.optimize(std::move(stmt), report);

            if (opts.verbose && !report.notes.empty()) {
                std::cout << "=== OPTIMIZER ===\n";
                for (auto& note : report.notes) {
                    std::cout << "  - " << note << "\n";
                }
            }

            // 5. Code generation
            CodeGen cg;
            std::string sql = cg.generate(stmt.get());

            if (opts.verbose || opts.transpileOnly) {
                std::cout << "=== SQL ===\n" << sql << "\n";
            }

            // 6. Execute (if not transpile-only)
            if (!opts.transpileOnly && db) {
                if (!executeSql(db, sql)) {
                    return false;
                }
            }
        }

        return true;
    } catch (const LexerError& e) {
        std::cerr << "Lexer error: " << e.what() << "\n";
        return false;
    } catch (const ParseError& e) {
        std::cerr << "Parse error: " << e.what() << "\n";
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return false;
    }
}

// -----------------------------------------------------------------------
// REPL
// -----------------------------------------------------------------------
static void runRepl(const Options& opts, Catalog& catalog, sqlite3* db) {
    std::cout << "SkibidiQL REPL v1.0.0\n";
    std::cout << "Type your SkibidiQL queries (end with ';') or 'exit' to quit.\n\n";

    std::string accumulated;
    bool running = true;

    while (running) {
        if (accumulated.empty()) {
            std::cout << "skibidi> ";
        } else {
            std::cout << "      -> ";
        }
        std::cout.flush();

        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }

        // Check for exit command
        std::string trimmed = line;
        size_t start = trimmed.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) trimmed = trimmed.substr(start);
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);

        if (trimmed == "exit" || trimmed == "quit" || trimmed == ".exit") {
            running = false;
            break;
        }

        accumulated += line + "\n";

        // Check if the accumulated query contains a semicolon
        bool hasSemi = accumulated.find(';') != std::string::npos;
        if (hasSemi) {
            processQuery(accumulated, opts, catalog, db);
            accumulated.clear();
        }
    }

    std::cout << "\nBye!\n";
}

// -----------------------------------------------------------------------
// Process a file
// -----------------------------------------------------------------------
static bool processFile(const std::string& path, const Options& opts,
                        Catalog& catalog, sqlite3* db) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "Error: Cannot open file: " << path << "\n";
        return false;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    return processQuery(content, opts, catalog, db);
}

// -----------------------------------------------------------------------
// Print usage
// -----------------------------------------------------------------------
static void printUsage(const char* progName) {
    std::cout << "Usage: " << progName << " [options]\n"
              << "\nOptions:\n"
              << "  --file <path>       Read and execute a .skql file\n"
              << "  --db <path>         SQLite database path (default: :memory:)\n"
              << "  --verbose           Print tokens, AST, optimizer report, and SQL\n"
              << "  --transpile-only    Only transpile to SQL, do not execute\n"
              << "  --help              Print this help message\n"
              << "\nWith no --file argument, starts an interactive REPL.\n";
}

// -----------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------
int main(int argc, char* argv[]) {
    Options opts;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            opts.filePath = argv[++i];
        } else if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            opts.dbPath = argv[++i];
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            opts.verbose = true;
        } else if (std::strcmp(argv[i], "--transpile-only") == 0) {
            opts.transpileOnly = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    // Open SQLite database
    sqlite3* db = nullptr;
    if (!opts.transpileOnly) {
        int rc = sqlite3_open(opts.dbPath.c_str(), &db);
        if (rc != SQLITE_OK) {
            std::cerr << "Error: Cannot open database '" << opts.dbPath
                      << "': " << sqlite3_errmsg(db) << "\n";
            sqlite3_close(db);
            return 1;
        }
    }

    // Load catalog
    Catalog catalog;
    catalog.load();

    int exitCode = 0;

    if (!opts.filePath.empty()) {
        if (!processFile(opts.filePath, opts, catalog, db)) {
            exitCode = 1;
        }
    } else {
        if (opts.transpileOnly) {
            // Read from stdin
            std::ostringstream ss;
            ss << std::cin.rdbuf();
            if (!processQuery(ss.str(), opts, catalog, db)) {
                exitCode = 1;
            }
        } else {
            opts.repl = true;
            runRepl(opts, catalog, db);
        }
    }

    if (db) sqlite3_close(db);
    return exitCode;
}
