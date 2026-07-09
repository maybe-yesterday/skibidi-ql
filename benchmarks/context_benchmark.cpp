#include "native_engine.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    int messages = 1000;
    int iterations = 100;
    unsigned long long tokenBudget = 512;
    int bufferPages = 1024;
};

struct Message {
    unsigned long long id = 0;
    std::string speaker;
    std::string text;
    std::string tab;
    bool autoTab = false;
};

struct RenderedPrompt {
    std::string text;
    std::size_t scannedMessages = 0;
    std::size_t renderedItems = 0;
};

struct BenchmarkRow {
    std::string method;
    std::string simulates;
    double elapsedMs = 0.0;
    double opsPerSec = 0.0;
    std::size_t avgTokens = 0;
    std::size_t avgScannedMessages = 0;
    double avgRenderedItems = 0.0;
    double tokensPerInputMessage = 0.0;
    double tokensPerRenderedItem = 0.0;
    double avgNeededHits = 0.0;
    double neededRecall = 0.0;
    std::size_t neededFacts = 0;
    std::size_t estimatedMemoryBytes = 0;
    std::size_t contextCacheHits = 0;
    std::size_t contextCacheMisses = 0;
    std::size_t atomsScored = 0;
    std::size_t atomsRendered = 0;
    std::uint64_t checksum = 0;
};

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

std::size_t estimateTokens(const std::string& text) {
    return std::max<std::size_t>(1, (text.size() + 3) / 4);
}

std::string cleanNeedle(std::string value) {
    value = lowerCopy(std::move(value));
    while (!value.empty() &&
           !std::isalnum(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           !std::isalnum(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string extractNeedleAfterMarker(const std::string& text,
                                     const std::string& marker) {
    const auto lower = lowerCopy(text);
    const auto pos = lower.find(marker);
    if (pos == std::string::npos) return "";
    auto value = text.substr(pos + marker.size());
    const auto punctuation = value.find_first_of(".!?;\n\r");
    if (punctuation != std::string::npos) {
        value = value.substr(0, punctuation);
    }
    return cleanNeedle(std::move(value));
}

std::string latestLocationNeedle(const std::vector<Message>& messages) {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        for (const char* marker : {
                 "i live in ", "i moved to ", "moved to ",
                 "i am in ", "i'm in ", "my location is ",
                 "i live near "}) {
            auto value = extractNeedleAfterMarker(it->text, marker);
            if (!value.empty()) return value;
        }
    }
    return "";
}

std::vector<std::string> requiredNeedlesForQuery(
    const std::vector<Message>& messages,
    const std::string& query) {
    std::vector<std::string> needles;
    const auto lowerQuery = lowerCopy(query);
    if (lowerQuery.find("restaurant") != std::string::npos ||
        lowerQuery.find("near me") != std::string::npos ||
        lowerQuery.find("nearby") != std::string::npos) {
        auto location = latestLocationNeedle(messages);
        if (!location.empty()) needles.push_back(std::move(location));
        needles.push_back("quiet restaurants");
    }
    return needles;
}

std::size_t countNeededHits(const std::string& text,
                            const std::vector<std::string>& needles) {
    const auto lower = lowerCopy(text);
    std::size_t hits = 0;
    for (const auto& needle : needles) {
        if (!needle.empty() &&
            lower.find(needle) != std::string::npos) {
            ++hits;
        }
    }
    return hits;
}

std::string messageLine(const Message& message) {
    return "message_" + std::to_string(message.id) + " " +
           message.speaker + ": " + message.text + "\n";
}

std::vector<std::string> queryTerms(const std::string& query) {
    std::vector<std::string> terms;
    std::istringstream input(lowerCopy(query));
    std::string term;
    while (input >> term) {
        while (!term.empty() &&
               !std::isalnum(static_cast<unsigned char>(term.back()))) {
            term.pop_back();
        }
        while (!term.empty() &&
               !std::isalnum(static_cast<unsigned char>(term.front()))) {
            term.erase(term.begin());
        }
        if (term.size() >= 3) terms.push_back(term);
    }
    return terms;
}

bool matchesQuery(const Message& message,
                  const std::vector<std::string>& terms) {
    const std::string lower = lowerCopy(message.text);
    for (const auto& term : terms) {
        if (lower.find(term) != std::string::npos) return true;
    }
    return false;
}

std::vector<Message> makeMessages(int count) {
    std::vector<Message> messages;
    messages.reserve(static_cast<std::size_t>(count));
    for (int id = 1; id <= count; ++id) {
        Message message;
        message.id = static_cast<unsigned long long>(id);
        message.speaker = "user";
        switch (id % 7) {
            case 0:
                message.text = "Never share passwords or api key tokens.";
                message.tab = "constraints";
                break;
            case 1:
                message.text = "I like cat cafes.";
                message.tab = "pet stuff";
                break;
            case 2:
                message.text =
                    "Decision: keep prompt views inside SkibidiQL.";
                message.tab = "project roadmap";
                break;
            case 3:
                message.text =
                    "Debug this later: sqlite perf join misses.";
                message.autoTab = true;
                break;
            case 4:
                message.text = "I prefer quiet restaurants.";
                message.tab = "preferences";
                break;
            case 5:
                message.text = "My dog likes salmon.";
                message.autoTab = true;
                break;
            default:
                message.text =
                    id % 11 == 0 ? "Actually I moved to NYC."
                                 : "I live in Seattle.";
                break;
        }
        messages.push_back(std::move(message));
    }
    return messages;
}

std::size_t estimateMessagesMemory(const std::vector<Message>& messages) {
    std::size_t bytes = sizeof(messages) +
        messages.capacity() * sizeof(Message);
    for (const auto& message : messages) {
        bytes += message.speaker.capacity();
        bytes += message.text.capacity();
        bytes += message.tab.capacity();
    }
    return bytes;
}

RenderedPrompt renderFullHistory(const std::vector<Message>& messages,
                                 const std::string&) {
    RenderedPrompt rendered;
    rendered.scannedMessages = messages.size();
    rendered.renderedItems = messages.size();
    for (const auto& message : messages) {
        rendered.text += messageLine(message);
    }
    return rendered;
}

RenderedPrompt renderRecencyWindow(const std::vector<Message>& messages,
                                   const std::string&,
                                   unsigned long long tokenBudget) {
    RenderedPrompt rendered;
    std::vector<std::string> selected;
    std::size_t tokens = 0;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        ++rendered.scannedMessages;
        auto line = messageLine(*it);
        const auto cost = estimateTokens(line);
        if (!selected.empty() && tokens + cost > tokenBudget) break;
        if (selected.empty() && cost > tokenBudget) break;
        selected.push_back(std::move(line));
        tokens += cost;
    }
    rendered.renderedItems = selected.size();
    for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
        rendered.text += *it;
    }
    return rendered;
}

RenderedPrompt renderKeywordScan(const std::vector<Message>& messages,
                                 const std::string& query,
                                 unsigned long long tokenBudget) {
    RenderedPrompt rendered;
    const auto terms = queryTerms(query);
    std::size_t tokens = 0;
    for (const auto& message : messages) {
        ++rendered.scannedMessages;
        if (!matchesQuery(message, terms)) continue;
        auto line = messageLine(message);
        const auto cost = estimateTokens(line);
        if (!rendered.text.empty() && tokens + cost > tokenBudget) continue;
        if (rendered.text.empty() && cost > tokenBudget) continue;
        rendered.text += line;
        tokens += cost;
        ++rendered.renderedItems;
    }
    return rendered;
}

std::string fieldValue(const NativeQueryResult& result,
                       const std::string& field) {
    for (const auto& row : result.rows) {
        if (row.size() >= 2 && row[0].toString() == field) {
            return row[1].toString();
        }
    }
    return "";
}

std::uint64_t checksumPrompt(const std::string& text) {
    std::uint64_t checksum = text.size();
    for (unsigned char ch : text) {
        checksum = checksum * 131 + ch;
    }
    return checksum;
}

template <typename Renderer>
BenchmarkRow measureBaseline(std::string method,
                             std::string simulates,
                             const std::vector<Message>& messages,
                             const std::vector<std::string>& requiredNeedles,
                             int iterations,
                             Renderer renderer) {
    std::uint64_t checksum = 0;
    std::size_t totalTokens = 0;
    std::size_t totalScanned = 0;
    std::size_t totalRenderedItems = 0;
    std::size_t totalNeededHits = 0;
    std::size_t memory = estimateMessagesMemory(messages);
    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const auto rendered = renderer(iteration);
        checksum += checksumPrompt(rendered.text);
        totalTokens += estimateTokens(rendered.text);
        totalScanned += rendered.scannedMessages;
        totalRenderedItems += rendered.renderedItems;
        totalNeededHits += countNeededHits(rendered.text, requiredNeedles);
        memory = std::max(memory,
                          estimateMessagesMemory(messages) +
                              rendered.text.capacity());
    }
    const auto finish = std::chrono::steady_clock::now();
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(
            finish - start).count();

    BenchmarkRow row;
    row.method = std::move(method);
    row.simulates = std::move(simulates);
    row.elapsedMs = elapsedMs;
    row.opsPerSec = iterations * 1000.0 / elapsedMs;
    row.avgTokens = totalTokens / static_cast<std::size_t>(iterations);
    row.avgScannedMessages =
        totalScanned / static_cast<std::size_t>(iterations);
    row.avgRenderedItems =
        static_cast<double>(totalRenderedItems) / iterations;
    row.tokensPerInputMessage =
        static_cast<double>(row.avgTokens) / messages.size();
    row.tokensPerRenderedItem = row.avgRenderedItems == 0.0
        ? 0.0
        : static_cast<double>(row.avgTokens) / row.avgRenderedItems;
    row.avgNeededHits = static_cast<double>(totalNeededHits) / iterations;
    row.neededFacts = requiredNeedles.size();
    row.neededRecall = requiredNeedles.empty()
        ? 1.0
        : row.avgNeededHits / requiredNeedles.size();
    row.estimatedMemoryBytes = memory;
    row.checksum = checksum;
    return row;
}

void seedContext(NativeEngine& engine, const std::vector<Message>& messages) {
    CreateContextStmt create;
    create.name = "bench_convo";
    engine.execute(&create);

    AliasTabStmt alias;
    alias.context = "bench_convo";
    alias.alias = "dog";
    alias.target = "convo about dog";
    engine.execute(&alias);

    for (const auto& message : messages) {
        AppendMemoryStmt append;
        append.context = "bench_convo";
        append.messageId = message.id;
        append.speaker = message.speaker;
        append.text = message.text;
        append.tab = message.tab;
        append.autoTab = message.autoTab;
        engine.execute(&append);
    }

    MergeTabsStmt merge;
    merge.context = "bench_convo";
    merge.fromTab = "pet stuff";
    merge.toTab = "dog";
    engine.execute(&merge);
    engine.flush();
}

BenchmarkRow measureContextQL(const std::vector<Message>& messages,
                              const Options& options,
                              const std::vector<std::string>& requiredNeedles,
                              bool cached) {
    const auto root = std::filesystem::temp_directory_path() /
        ("skibidi-context-benchmark-" +
         std::to_string(
             std::chrono::high_resolution_clock::now()
                 .time_since_epoch().count()) +
         (cached ? "-cached" : "-varied"));
    auto engine = std::make_unique<NativeEngine>(
        root, static_cast<std::size_t>(options.bufferPages));
    seedContext(*engine, messages);

    SpillContextStmt statement;
    statement.context = "bench_convo";
    statement.query = "Find restaurants near me";
    statement.tokenBudget = options.tokenBudget;
    statement.receipts = true;

    if (cached) {
        (void)engine->execute(&statement);
    }
    engine->resetStats();

    std::uint64_t checksum = 0;
    std::size_t totalTokens = 0;
    std::size_t totalRenderedItems = 0;
    std::size_t totalNeededHits = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < options.iterations; ++iteration) {
        if (!cached) {
            statement.query =
                "Find restaurants near me " + std::to_string(iteration);
        }
        const auto result = engine->execute(&statement);
        const auto currentContext = fieldValue(result, "current_context");
        checksum += checksumPrompt(currentContext);
        const auto tokenCost = fieldValue(result, "token_cost");
        totalTokens += tokenCost.empty()
            ? 0
            : static_cast<std::size_t>(std::stoull(tokenCost));
        const auto renderedAtoms = fieldValue(result, "rendered_atoms");
        totalRenderedItems += renderedAtoms.empty()
            ? 0
            : static_cast<std::size_t>(std::stoull(renderedAtoms));
        totalNeededHits += countNeededHits(currentContext, requiredNeedles);
    }
    const auto finish = std::chrono::steady_clock::now();
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(
            finish - start).count();
    const auto stats = engine->stats();

    BenchmarkRow row;
    row.method = cached ? "skibidi_contextql_cached"
                        : "skibidi_contextql_varied";
    row.simulates = cached
        ? "maintained prompt view, same query, revision-aware cache"
        : "maintained prompt view, changing query, no cache reuse";
    row.elapsedMs = elapsedMs;
    row.opsPerSec = options.iterations * 1000.0 / elapsedMs;
    row.avgTokens = totalTokens / static_cast<std::size_t>(options.iterations);
    row.avgScannedMessages = cached ? 0 : messages.size();
    row.avgRenderedItems =
        static_cast<double>(totalRenderedItems) / options.iterations;
    row.tokensPerInputMessage =
        static_cast<double>(row.avgTokens) / messages.size();
    row.tokensPerRenderedItem = row.avgRenderedItems == 0.0
        ? 0.0
        : static_cast<double>(row.avgTokens) / row.avgRenderedItems;
    row.avgNeededHits =
        static_cast<double>(totalNeededHits) / options.iterations;
    row.neededFacts = requiredNeedles.size();
    row.neededRecall = requiredNeedles.empty()
        ? 1.0
        : row.avgNeededHits / requiredNeedles.size();
    row.estimatedMemoryBytes = stats.estimatedMemoryBytes;
    row.contextCacheHits = stats.contextCacheHits;
    row.contextCacheMisses = stats.contextCacheMisses;
    row.atomsScored = stats.contextAtomsScored;
    row.atomsRendered = stats.contextAtomsRendered;
    row.checksum = checksum;

    engine.reset();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return row;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--messages") == 0 &&
            index + 1 < argc) {
            options.messages = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--iterations") == 0 &&
                   index + 1 < argc) {
            options.iterations = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--token-budget") == 0 &&
                   index + 1 < argc) {
            options.tokenBudget =
                static_cast<unsigned long long>(
                    std::strtoull(argv[++index], nullptr, 10));
        } else if (std::strcmp(argv[index], "--buffer-pages") == 0 &&
                   index + 1 < argc) {
            options.bufferPages = std::atoi(argv[++index]);
        } else {
            throw std::runtime_error("Invalid context benchmark argument");
        }
    }
    if (options.messages <= 0 || options.iterations <= 0 ||
        options.tokenBudget == 0 || options.bufferPages <= 0) {
        throw std::runtime_error("Context benchmark values must be positive");
    }
    return options;
}

void printRows(const std::vector<BenchmarkRow>& rows,
               const Options& options) {
    std::cout << "messages=" << options.messages
              << " iterations=" << options.iterations
              << " token_budget=" << options.tokenBudget << "\n";
    std::cout << "benchmark meaning: seed "
              << options.messages
              << " prior messages, then run "
              << options.iterations
              << " retrieval calls against that fixed conversation\n";
    std::cout << "| Method | Simulates | Time | Ops/sec | Avg prompt tokens | "
              << "Tok/input msg | Avg returned items | Tok/item | "
              << "Needed hit/total | Needed recall | Avg scanned msgs | "
              << "Est mem | Cache hits/misses | "
              << "Atoms scored/rendered |\n";
    std::cout << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& row : rows) {
        std::cout << "| " << row.method
                  << " | " << row.simulates
                  << " | " << std::fixed << std::setprecision(3)
                  << row.elapsedMs << " ms"
                  << " | " << std::setprecision(1) << row.opsPerSec
                  << " | " << row.avgTokens
                  << " | " << std::setprecision(3)
                  << row.tokensPerInputMessage
                  << " | " << std::setprecision(1)
                  << row.avgRenderedItems
                  << " | " << std::setprecision(1)
                  << row.tokensPerRenderedItem
                  << " | " << std::setprecision(1)
                  << row.avgNeededHits << "/" << row.neededFacts
                  << " | " << std::setprecision(0)
                  << row.neededRecall * 100.0 << "%"
                  << " | " << row.avgScannedMessages
                  << " | " << std::setprecision(2)
                  << row.estimatedMemoryBytes / 1048576.0 << " MiB"
                  << " | " << row.contextCacheHits << "/"
                  << row.contextCacheMisses
                  << " | " << row.atomsScored << "/"
                  << row.atomsRendered << " |\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        const auto messages = makeMessages(options.messages);
        const std::string query = "Find restaurants near me";
        const auto requiredNeedles = requiredNeedlesForQuery(
            messages, query);

        std::vector<BenchmarkRow> rows;
        rows.push_back(measureBaseline(
            "full_history_prompt",
            "stuff every raw message into the prompt",
            messages,
            requiredNeedles,
            options.iterations,
            [&](int) {
                return renderFullHistory(messages, query);
            }));
        rows.push_back(measureBaseline(
            "recency_window",
            "last messages until token budget",
            messages,
            requiredNeedles,
            options.iterations,
            [&](int) {
                return renderRecencyWindow(
                    messages, query, options.tokenBudget);
            }));
        rows.push_back(measureBaseline(
            "keyword_scan",
            "scan raw messages for query terms",
            messages,
            requiredNeedles,
            options.iterations,
            [&](int) {
                return renderKeywordScan(
                    messages, query, options.tokenBudget);
            }));
        rows.push_back(measureContextQL(
            messages, options, requiredNeedles, false));
        rows.push_back(measureContextQL(
            messages, options, requiredNeedles, true));

        printRows(rows, options);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Context benchmark error: " << error.what() << "\n";
        return 1;
    }
}
