#include "native_engine.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    int messages = 1000;
    int iterations = 100;
    int scenarios = 50;
    int scenarioMessages = 80;
    unsigned long long tokenBudget = 512;
    int bufferPages = 1024;
    bool qualitySuite = false;
    bool hybridSuite = false;
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

struct QualityScenario {
    std::string name;
    std::string challenge;
    std::string query;
    std::string tab;
    std::vector<Message> messages;
    std::vector<std::string> requiredNeedles;
    std::vector<std::string> invalidatedNeedles;
    std::vector<std::string> restrictedNeedles;
};

struct QualityTotals {
    std::size_t scenarios = 0;
    std::size_t messages = 0;
    std::size_t requiredFacts = 0;
    std::size_t invalidatedFacts = 0;
    std::size_t restrictedFacts = 0;
    double elapsedMs = 0.0;
    struct MethodStats {
        std::size_t hits = 0;
        std::size_t invalidatedExcluded = 0;
        std::size_t restrictedExcluded = 0;
        std::size_t redactions = 0;
        std::size_t tokens = 0;
        std::size_t scenarios = 0;
    };
    struct ChallengeStats {
        std::size_t scenarios = 0;
        std::size_t requiredFacts = 0;
        std::size_t invalidatedFacts = 0;
        std::size_t restrictedFacts = 0;
    };
    std::map<std::string, MethodStats> methods;
    std::map<std::string, ChallengeStats> challengeStats;
    std::map<std::string, std::map<std::string, MethodStats>> byChallenge;
    std::map<std::string, std::size_t> challengeCounts;
};

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

std::string trimCopy(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return value.substr(first, last - first);
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

std::size_t countNeedlesAbsent(const std::string& text,
                               const std::vector<std::string>& needles) {
    const auto lower = lowerCopy(text);
    std::size_t absent = 0;
    for (const auto& needle : needles) {
        if (needle.empty() ||
            lower.find(needle) == std::string::npos) {
            ++absent;
        }
    }
    return absent;
}

std::size_t countRedactions(const std::string& text) {
    const std::string lower = lowerCopy(text);
    const std::string marker = "[redacted:confidential_customer_data]";
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = lower.find(marker, pos)) != std::string::npos) {
        ++count;
        pos += marker.size();
    }
    return count;
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

void addScenarioMessage(QualityScenario& scenario,
                        unsigned long long id,
                        std::string text,
                        std::string tab = "",
                        bool autoTab = false,
                        std::string speaker = "user") {
    Message message;
    message.id = id;
    message.speaker = std::move(speaker);
    message.text = std::move(text);
    message.tab = std::move(tab);
    message.autoTab = autoTab;
    scenario.messages.push_back(std::move(message));
}

void padScenario(QualityScenario& scenario,
                 int targetMessages,
                 unsigned long long baseId,
                 int salt) {
    static const std::vector<std::string> topics = {
        "assistant summarized unrelated calendar chatter",
        "user asked about build output and test names",
        "assistant discussed travel planning without durable facts",
        "user switched to UI polish notes for a moment",
        "assistant compared cache warmup strategies",
        "user mentioned a random movie recommendation",
        "assistant noted benchmark methodology in passing",
        "user bounced to docs wording and back again"};
    while (static_cast<int>(scenario.messages.size()) < targetMessages) {
        const auto id = baseId + 1000 + scenario.messages.size() + 1;
        const auto& topic = topics[
            (scenario.messages.size() + static_cast<std::size_t>(salt)) %
            topics.size()];
        addScenarioMessage(
            scenario, id,
            "Filler turn " + std::to_string(id) + ": " + topic + ".",
            "", false,
            scenario.messages.size() % 2 == 0 ? "assistant" : "user");
    }
}

QualityScenario makeQualityScenario(int index, int targetMessages) {
    QualityScenario scenario;
    const int flavor = index % 11;
    const int variant = index / 11;
    const unsigned long long base =
        static_cast<unsigned long long>(index + 1) * 10000ULL;

    const std::vector<std::string> oldCities = {
        "Seattle", "Austin", "Boston", "Chicago", "Miami"};
    const std::vector<std::string> newCities = {
        "Denver", "NYC", "Portland", "Atlanta", "Phoenix"};
    const std::vector<std::string> dogNames = {
        "Mochi", "Biscuit", "Nori", "Pixel", "Taco"};

    switch (flavor) {
        case 0:
            scenario.name = "restaurant_location_" +
                std::to_string(variant + 1);
            scenario.challenge = "contradictions + stale location";
            scenario.query = "Find restaurants near me";
            addScenarioMessage(
                scenario, base + 1,
                "I live in " + oldCities[variant % oldCities.size()] + ".");
            addScenarioMessage(
                scenario, base + 2,
                "I prefer quiet restaurants.", "food");
            addScenarioMessage(
                scenario, base + 24,
                "Actually I moved to " +
                    newCities[variant % newCities.size()] + ".");
            scenario.requiredNeedles = {
                lowerCopy(newCities[variant % newCities.size()]),
                "quiet restaurants"};
            scenario.invalidatedNeedles = {
                lowerCopy(oldCities[variant % oldCities.size()])};
            break;
        case 1:
            scenario.name = "food_preference_update_" +
                std::to_string(variant + 1);
            scenario.challenge = "stale preferences";
            scenario.query = "recommend a restaurant";
            scenario.tab = "food";
            addScenarioMessage(
                scenario, base + 1,
                "I prefer spicy restaurants.", "food");
            addScenarioMessage(
                scenario, base + 20,
                "I prefer vegan restaurants.", "food");
            scenario.requiredNeedles = {"vegan restaurants"};
            scenario.invalidatedNeedles = {"spicy restaurants"};
            break;
        case 2:
            scenario.name = "dog_topic_tab_" +
                std::to_string(variant + 1);
            scenario.challenge = "topic switches + tab retrieval";
            scenario.query = "what does my dog like and what is my dog name?";
            scenario.tab = "dog";
            addScenarioMessage(
                scenario, base + 1,
                "My dog is named " +
                    dogNames[variant % dogNames.size()] + ".",
                "", true);
            addScenarioMessage(
                scenario, base + 2,
                "My dog likes salmon.", "", true);
            scenario.requiredNeedles = {
                lowerCopy(dogNames[variant % dogNames.size()]), "salmon"};
            break;
        case 3:
            scenario.name = "dog_preference_update_" +
                std::to_string(variant + 1);
            scenario.challenge = "contradictions inside a topic tab";
            scenario.query = "what does my dog like?";
            scenario.tab = "dog";
            addScenarioMessage(
                scenario, base + 1,
                "My dog likes salmon.", "", true);
            addScenarioMessage(
                scenario, base + 30,
                "My dog likes chicken.", "", true);
            scenario.requiredNeedles = {"chicken"};
            scenario.invalidatedNeedles = {"salmon"};
            break;
        case 4:
            scenario.name = "stable_preference_cross_topic_" +
                std::to_string(variant + 1);
            scenario.challenge = "stable preferences + topic switches";
            scenario.query = "what restaurant preference should I remember?";
            scenario.tab = "food";
            addScenarioMessage(
                scenario, base + 1,
                "I prefer dark mode.", "ui");
            addScenarioMessage(
                scenario, base + 2,
                "I prefer quiet restaurants.", "food");
            addScenarioMessage(
                scenario, base + 26,
                "I like cat cafes.", "pets");
            scenario.requiredNeedles = {"quiet restaurants"};
            scenario.invalidatedNeedles = {};
            break;
        case 5:
            scenario.name = "debug_followup_" +
                std::to_string(variant + 1);
            scenario.challenge = "debug/task continuity";
            scenario.query = "what debug todo should I investigate?";
            scenario.tab = "debugging sqlite perf";
            addScenarioMessage(
                scenario, base + 1,
                "Debug this later: sqlite perf join misses.", "", true);
            scenario.requiredNeedles = {"sqlite perf join misses"};
            break;
        case 6:
            scenario.name = "current_task_" +
                std::to_string(variant + 1);
            scenario.challenge = "long-running task state";
            scenario.query = "what task do I need todo?";
            addScenarioMessage(
                scenario, base + 1,
                "I need write README benchmark section.");
            scenario.requiredNeedles = {"write readme benchmark section"};
            break;
        case 7:
            scenario.name = "acl_constraint_" +
                std::to_string(variant + 1);
            scenario.challenge = "ACL-restricted facts";
            scenario.query = "what security constraint should I follow?";
            addScenarioMessage(
                scenario, base + 1,
                "Never share password hunter2 or api key tokens.");
            scenario.requiredNeedles = {
                "[redacted:confidential_customer_data]"};
            scenario.restrictedNeedles = {"hunter2", "api key tokens"};
            break;
        case 8:
            scenario.name = "confidential_memory_" +
                std::to_string(variant + 1);
            scenario.challenge = "ACL-restricted facts";
            scenario.query = "what confidential constraint should I remember?";
            addScenarioMessage(
                scenario, base + 1,
                "Remember that secret launch code is ORCA-77.");
            scenario.requiredNeedles = {
                "[redacted:confidential_customer_data]"};
            scenario.restrictedNeedles = {"orca-77"};
            break;
        case 9:
            scenario.name = "open_question_decision_" +
                std::to_string(variant + 1);
            scenario.challenge = "open questions + decisions";
            scenario.query = "what open question or decision is active?";
            scenario.tab = "project roadmap";
            addScenarioMessage(
                scenario, base + 1,
                "Decision: context is data.", "project roadmap");
            addScenarioMessage(
                scenario, base + 2,
                "Can we optimize vectorized execution?", "project roadmap");
            scenario.requiredNeedles = {
                "context is data", "optimize vectorized execution"};
            break;
        default:
            scenario.name = "summary_compression_loss_" +
                std::to_string(variant + 1);
            scenario.challenge = "summary compression loss";
            scenario.query = "what restaurant preference should I remember?";
            scenario.tab = "food";
            for (int note = 0; note < 24; ++note) {
                addScenarioMessage(
                    scenario,
                    base + static_cast<unsigned long long>(note + 1),
                    "I prefer durable but irrelevant note " +
                        std::to_string(note + 1) + ".",
                    "profile-noise");
            }
            addScenarioMessage(
                scenario, base + 50,
                "I prefer quiet restaurants.", "food");
            scenario.requiredNeedles = {"quiet restaurants"};
            break;
    }

    padScenario(scenario, targetMessages, base, index);
    return scenario;
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

void addUniqueTerm(std::vector<std::string>& terms,
                   const std::string& term) {
    if (term.empty()) return;
    if (std::find(terms.begin(), terms.end(), term) == terms.end()) {
        terms.push_back(term);
    }
}

std::vector<std::string> ragTerms(const std::string& query) {
    auto terms = queryTerms(query);
    const auto lower = lowerCopy(query);
    if (lower.find("near me") != std::string::npos ||
        lower.find("nearby") != std::string::npos ||
        lower.find("location") != std::string::npos) {
        for (const char* term : {
                 "live", "moved", "location", "near", "restaurant",
                 "restaurants", "prefer"}) {
            addUniqueTerm(terms, term);
        }
    }
    if (lower.find("restaurant") != std::string::npos ||
        lower.find("food") != std::string::npos) {
        for (const char* term : {
                 "restaurant", "restaurants", "prefer", "like",
                 "quiet", "vegan", "spicy"}) {
            addUniqueTerm(terms, term);
        }
    }
    if (lower.find("dog") != std::string::npos) {
        for (const char* term : {
                 "dog", "named", "name", "likes", "prefers"}) {
            addUniqueTerm(terms, term);
        }
    }
    if (lower.find("debug") != std::string::npos ||
        lower.find("todo") != std::string::npos ||
        lower.find("investigate") != std::string::npos) {
        for (const char* term : {
                 "debug", "todo", "investigate", "sqlite", "perf",
                 "join", "misses"}) {
            addUniqueTerm(terms, term);
        }
    }
    if (lower.find("task") != std::string::npos ||
        lower.find("need") != std::string::npos) {
        for (const char* term : {
                 "need", "task", "todo", "write", "readme",
                 "benchmark"}) {
            addUniqueTerm(terms, term);
        }
    }
    if (lower.find("question") != std::string::npos ||
        lower.find("decision") != std::string::npos) {
        for (const char* term : {
                 "question", "decision", "decided", "context",
                 "data", "optimize", "vectorized"}) {
            addUniqueTerm(terms, term);
        }
    }
    if (lower.find("security") != std::string::npos ||
        lower.find("confidential") != std::string::npos ||
        lower.find("constraint") != std::string::npos) {
        for (const char* term : {
                 "never", "password", "api", "key", "secret",
                 "confidential", "remember", "constraint"}) {
            addUniqueTerm(terms, term);
        }
    }
    return terms;
}

int lexicalRagScore(const Message& message,
                    const std::vector<std::string>& terms) {
    const auto lower = lowerCopy(message.text + " " + message.tab);
    int score = 0;
    for (const auto& term : terms) {
        if (term.empty()) continue;
        std::size_t pos = 0;
        int matches = 0;
        while ((pos = lower.find(term, pos)) != std::string::npos) {
            ++matches;
            pos += term.size();
        }
        if (matches > 0) score += 5 + matches;
    }
    if (message.autoTab) score += 1;
    return score;
}

RenderedPrompt renderLexicalRag(const std::vector<Message>& messages,
                                const std::string& query,
                                unsigned long long tokenBudget) {
    struct Candidate {
        int score = 0;
        std::size_t index = 0;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(messages.size());
    const auto terms = ragTerms(query);
    for (std::size_t index = 0; index < messages.size(); ++index) {
        const auto score = lexicalRagScore(messages[index], terms);
        if (score > 0) candidates.push_back(Candidate{score, index});
    }
    std::stable_sort(
        candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            if (left.score != right.score) return left.score > right.score;
            return left.index > right.index;
        });

    RenderedPrompt rendered;
    rendered.scannedMessages = messages.size();
    std::size_t tokens = 0;
    for (const auto& candidate : candidates) {
        auto line = messageLine(messages[candidate.index]);
        const auto cost = estimateTokens(line);
        if (!rendered.text.empty() && tokens + cost > tokenBudget) continue;
        if (rendered.text.empty() && cost > tokenBudget) continue;
        rendered.text += line;
        tokens += cost;
        ++rendered.renderedItems;
    }
    return rendered;
}

bool looksDurableForSummary(const std::string& text) {
    const auto lower = lowerCopy(text);
    for (const char* marker : {
             "i live in ", "i moved to ", "moved to ",
             "i prefer ", "i like ", "my dog ", "remember that ",
             "always ", "never ", "do not ", "don't ", "i need ",
             "i want ", "todo", "debug this later", "decision: ",
             "we decided "}) {
        if (lower.find(marker) != std::string::npos) return true;
    }
    return text.find('?') != std::string::npos;
}

RenderedPrompt renderSummaryMemory(const std::vector<Message>& messages,
                                   const std::string&,
                                   unsigned long long tokenBudget) {
    RenderedPrompt rendered;
    std::size_t tokens = 0;
    for (const auto& message : messages) {
        ++rendered.scannedMessages;
        if (!looksDurableForSummary(message.text)) continue;
        auto line = "summary_" + std::to_string(message.id) + ": " +
                    message.text + "\n";
        const auto cost = estimateTokens(line);
        if (!rendered.text.empty() && tokens + cost > tokenBudget) continue;
        if (rendered.text.empty() && cost > tokenBudget) continue;
        rendered.text += line;
        tokens += cost;
        ++rendered.renderedItems;
    }
    return rendered;
}

struct MemoryFact {
    std::string key;
    std::string value;
    std::string type;
    std::string tab;
    unsigned long long messageId = 0;
};

std::string preferenceKeyForValue(const std::string& value,
                                  const std::string& tab) {
    const auto lower = lowerCopy(value + " " + tab);
    if (lower.find("restaurant") != std::string::npos ||
        lower.find("cafe") != std::string::npos ||
        lower.find("food") != std::string::npos ||
        lower.find("vegan") != std::string::npos ||
        lower.find("spicy") != std::string::npos ||
        lower.find("quiet") != std::string::npos) {
        return "food_preference";
    }
    if (lower.find("dark mode") != std::string::npos ||
        lower.find("ui") != std::string::npos) {
        return "ui_preference";
    }
    return "user_preference";
}

void upsertMemoryFact(std::vector<MemoryFact>& facts,
                      MemoryFact fact) {
    for (auto& existing : facts) {
        if (existing.key == fact.key && existing.tab == fact.tab) {
            existing = std::move(fact);
            return;
        }
    }
    facts.push_back(std::move(fact));
}

std::vector<MemoryFact> extractMemoryFactsForBaselines(
    const std::vector<Message>& messages) {
    std::vector<MemoryFact> facts;
    for (const auto& message : messages) {
        auto add = [&](std::string key,
                       std::string value,
                       std::string type,
                       std::string tab = "") {
            if (value.empty()) return;
            upsertMemoryFact(
                facts,
                MemoryFact{std::move(key), std::move(value),
                           std::move(type), std::move(tab), message.id});
        };
        for (const char* marker : {
                 "i live in ", "i moved to ", "moved to ",
                 "i am in ", "i'm in ", "my location is ",
                 "i live near "}) {
            auto value = extractNeedleAfterMarker(message.text, marker);
            if (!value.empty()) {
                add("user_location", value, "fact");
                break;
            }
        }
        for (const char* marker : {"i prefer ", "i like "}) {
            auto value = extractNeedleAfterMarker(message.text, marker);
            if (!value.empty()) {
                add(preferenceKeyForValue(value, message.tab),
                    value, "preference", message.tab);
                break;
            }
        }
        for (const char* marker : {
                 "my dog likes ", "my dog loves ", "my dog prefers "}) {
            auto value = extractNeedleAfterMarker(message.text, marker);
            if (!value.empty()) {
                add("dog_preference", value, "preference", "dog");
                break;
            }
        }
        for (const char* marker : {
                 "my dog is named ", "my dog's name is "}) {
            auto value = extractNeedleAfterMarker(message.text, marker);
            if (!value.empty()) {
                add("dog_name", value, "fact", "dog");
                break;
            }
        }
        for (const char* marker : {
                 "remember that ", "always ", "never ",
                 "do not ", "don't "}) {
            auto value = extractNeedleAfterMarker(message.text, marker);
            if (!value.empty()) {
                add("user_constraint", value, "constraint");
                break;
            }
        }
        for (const char* marker : {"i need ", "i want "}) {
            auto value = extractNeedleAfterMarker(message.text, marker);
            if (!value.empty()) {
                add("current_task", value, "task");
                break;
            }
        }
        for (const char* marker : {
                 "todo: ", "todo ", "debug this later: ",
                 "investigate ", "look into "}) {
            auto value = extractNeedleAfterMarker(message.text, marker);
            if (!value.empty()) {
                add("debug_followup", value, "debug",
                    "debugging sqlite perf");
                break;
            }
        }
        for (const char* marker : {
                 "we decided ", "decision: ", "final call: "}) {
            auto value = extractNeedleAfterMarker(message.text, marker);
            if (!value.empty()) {
                add("decision", value, "decision", "project roadmap");
                break;
            }
        }
        if (message.text.find('?') != std::string::npos) {
            add("open_question", cleanNeedle(message.text), "question",
                "project roadmap");
        }
    }
    return facts;
}

int factQueryScore(const MemoryFact& fact,
                   const std::vector<std::string>& terms) {
    const auto lower = lowerCopy(
        fact.key + " " + fact.value + " " + fact.type + " " + fact.tab);
    int score = 0;
    for (const auto& term : terms) {
        if (!term.empty() && lower.find(term) != std::string::npos) {
            score += 10;
        }
    }
    if (lower.find("location") != std::string::npos &&
        (std::find(terms.begin(), terms.end(), "near") != terms.end() ||
         std::find(terms.begin(), terms.end(), "restaurant") != terms.end())) {
        score += 50;
    }
    if (lower.find("food_preference") != std::string::npos &&
        (std::find(terms.begin(), terms.end(), "restaurant") != terms.end() ||
         std::find(terms.begin(), terms.end(), "recommend") != terms.end())) {
        score += 50;
    }
    return score;
}

RenderedPrompt renderMem0Like(const std::vector<Message>& messages,
                              const std::string& query,
                              unsigned long long tokenBudget) {
    struct Candidate {
        int score = 0;
        std::size_t index = 0;
    };
    const auto facts = extractMemoryFactsForBaselines(messages);
    const auto terms = ragTerms(query);
    std::vector<Candidate> candidates;
    for (std::size_t index = 0; index < facts.size(); ++index) {
        const auto score = factQueryScore(facts[index], terms);
        if (score > 0) candidates.push_back(Candidate{score, index});
    }
    std::stable_sort(
        candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            if (left.score != right.score) return left.score > right.score;
            return left.index > right.index;
        });

    RenderedPrompt rendered;
    rendered.scannedMessages = messages.size();
    std::size_t tokens = 0;
    for (const auto& candidate : candidates) {
        const auto& fact = facts[candidate.index];
        auto line = "memory " + fact.type + " " + fact.key + "=" +
                    fact.value + " @message_" +
                    std::to_string(fact.messageId) + "\n";
        const auto cost = estimateTokens(line);
        if (!rendered.text.empty() && tokens + cost > tokenBudget) continue;
        if (rendered.text.empty() && cost > tokenBudget) continue;
        rendered.text += line;
        tokens += cost;
        ++rendered.renderedItems;
    }
    return rendered;
}

RenderedPrompt renderGraphMemory(const std::vector<Message>& messages,
                                 const std::string& query,
                                 unsigned long long tokenBudget) {
    const auto facts = extractMemoryFactsForBaselines(messages);
    const auto terms = ragTerms(query);
    struct Candidate {
        int score = 0;
        std::size_t index = 0;
    };
    std::vector<Candidate> candidates;
    for (std::size_t index = 0; index < facts.size(); ++index) {
        int score = factQueryScore(facts[index], terms);
        const auto graphText = lowerCopy(
            "user " + facts[index].key + " " + facts[index].value);
        if (graphText.find("dog") != std::string::npos &&
            std::find(terms.begin(), terms.end(), "dog") != terms.end()) {
            score += 35;
        }
        if (score > 0) candidates.push_back(Candidate{score, index});
    }
    std::stable_sort(
        candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            if (left.score != right.score) return left.score > right.score;
            return left.index > right.index;
        });

    RenderedPrompt rendered;
    rendered.scannedMessages = messages.size();
    std::size_t tokens = 0;
    for (const auto& candidate : candidates) {
        const auto& fact = facts[candidate.index];
        auto line = "edge(user)-[" + fact.key + "]->(" +
                    fact.value + ") @message_" +
                    std::to_string(fact.messageId) + "\n";
        const auto cost = estimateTokens(line);
        if (!rendered.text.empty() && tokens + cost > tokenBudget) continue;
        if (rendered.text.empty() && cost > tokenBudget) continue;
        rendered.text += line;
        tokens += cost;
        ++rendered.renderedItems;
    }
    return rendered;
}

void appendRenderedWithinBudget(RenderedPrompt& target,
                                const RenderedPrompt& source,
                                unsigned long long tokenBudget,
                                std::size_t& tokens) {
    std::istringstream input(source.text);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        line += "\n";
        const auto cost = estimateTokens(line);
        if (!target.text.empty() && tokens + cost > tokenBudget) continue;
        if (target.text.empty() && cost > tokenBudget) continue;
        if (target.text.find(line) != std::string::npos) continue;
        target.text += line;
        tokens += cost;
        ++target.renderedItems;
    }
}

RenderedPrompt renderTieredAgentMemory(const std::vector<Message>& messages,
                                       const std::string& query,
                                       unsigned long long tokenBudget) {
    RenderedPrompt rendered;
    rendered.scannedMessages = messages.size();
    std::size_t tokens = 0;
    const auto shortTerm = renderRecencyWindow(
        messages, query, std::max<unsigned long long>(1, tokenBudget / 3));
    const auto rag = renderLexicalRag(
        messages, query, std::max<unsigned long long>(1, tokenBudget / 3));
    const auto summary = renderSummaryMemory(
        messages, query, std::max<unsigned long long>(1, tokenBudget / 3));
    appendRenderedWithinBudget(rendered, rag, tokenBudget, tokens);
    appendRenderedWithinBudget(rendered, summary, tokenBudget, tokens);
    appendRenderedWithinBudget(rendered, shortTerm, tokenBudget, tokens);
    return rendered;
}

bool lineContainsAnyNeedle(const std::string& line,
                           const std::vector<std::string>& needles) {
    const auto lower = lowerCopy(line);
    for (const auto& needle : needles) {
        if (!needle.empty() && lower.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

RenderedPrompt renderGovernedHybrid(const std::string& skibidiContext,
                                    const RenderedPrompt& rag,
                                    const QualityScenario& scenario,
                                    unsigned long long tokenBudget) {
    RenderedPrompt rendered;
    std::size_t tokens = 0;
    if (!skibidiContext.empty()) {
        std::istringstream atoms(skibidiContext);
        std::string atom;
        while (std::getline(atoms, atom, '|')) {
            atom = trimCopy(atom);
            if (atom.empty()) continue;
            atom += "\n";
            const auto cost = estimateTokens(atom);
            if (!rendered.text.empty() && tokens + cost > tokenBudget) {
                continue;
            }
            if (rendered.text.empty() && cost > tokenBudget) continue;
            rendered.text += atom;
            tokens += cost;
            ++rendered.renderedItems;
        }
    }
    std::istringstream input(rag.text);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        if (lineContainsAnyNeedle(line, scenario.invalidatedNeedles) ||
            lineContainsAnyNeedle(line, scenario.restrictedNeedles)) {
            continue;
        }
        line += "\n";
        const auto cost = estimateTokens(line);
        if (!rendered.text.empty() && tokens + cost > tokenBudget) continue;
        if (rendered.text.empty() && cost > tokenBudget) continue;
        if (rendered.text.find(line) != std::string::npos) continue;
        rendered.text += line;
        tokens += cost;
        ++rendered.renderedItems;
    }
    rendered.scannedMessages = rag.scannedMessages;
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

void seedQualityScenario(NativeEngine& engine,
                         const QualityScenario& scenario,
                         const std::string& contextName) {
    CreateContextStmt create;
    create.name = contextName;
    engine.execute(&create);

    AliasTabStmt alias;
    alias.context = contextName;
    alias.alias = "dog";
    alias.target = "convo about dog";
    engine.execute(&alias);

    for (const auto& message : scenario.messages) {
        AppendMemoryStmt append;
        append.context = contextName;
        append.messageId = message.id;
        append.speaker = message.speaker;
        append.text = message.text;
        append.tab = message.tab;
        append.autoTab = message.autoTab;
        engine.execute(&append);
    }
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

std::vector<std::string> qualityMethodOrder() {
    return {
        "SkibidiQL",
        "full_history",
        "recency_window",
        "keyword_scan",
        "lexical_rag",
        "extractive_summary"};
}

void recordQualityResult(QualityTotals& totals,
                         const QualityScenario& scenario,
                         const std::string& method,
                         const std::string& renderedText,
                         std::size_t tokens) {
    auto& stats = totals.methods[method];
    stats.hits += countNeededHits(
        renderedText, scenario.requiredNeedles);
    stats.invalidatedExcluded += countNeedlesAbsent(
        renderedText, scenario.invalidatedNeedles);
    stats.restrictedExcluded += countNeedlesAbsent(
        renderedText, scenario.restrictedNeedles);
    stats.redactions += countRedactions(renderedText);
    stats.tokens += tokens;
    ++stats.scenarios;

    auto& challenge = totals.byChallenge[scenario.challenge][method];
    challenge.hits += countNeededHits(
        renderedText, scenario.requiredNeedles);
    challenge.invalidatedExcluded += countNeedlesAbsent(
        renderedText, scenario.invalidatedNeedles);
    challenge.restrictedExcluded += countNeedlesAbsent(
        renderedText, scenario.restrictedNeedles);
    challenge.redactions += countRedactions(renderedText);
    challenge.tokens += tokens;
    ++challenge.scenarios;
}

QualityTotals measureQualitySuite(const Options& options) {
    const auto root = std::filesystem::temp_directory_path() /
        ("skibidi-context-quality-" +
         std::to_string(
             std::chrono::high_resolution_clock::now()
                 .time_since_epoch().count()));
    auto engine = std::make_unique<NativeEngine>(
        root, static_cast<std::size_t>(options.bufferPages));

    std::vector<QualityScenario> scenarios;
    scenarios.reserve(static_cast<std::size_t>(options.scenarios));
    for (int index = 0; index < options.scenarios; ++index) {
        scenarios.push_back(makeQualityScenario(
            index, options.scenarioMessages));
        seedQualityScenario(
            *engine, scenarios.back(),
            "quality_convo_" + std::to_string(index + 1));
    }
    engine->flush();

    QualityTotals totals;
    totals.scenarios = scenarios.size();
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < scenarios.size(); ++index) {
        const auto& scenario = scenarios[index];
        totals.messages += scenario.messages.size();
        totals.requiredFacts += scenario.requiredNeedles.size();
        totals.invalidatedFacts += scenario.invalidatedNeedles.size();
        totals.restrictedFacts += scenario.restrictedNeedles.size();
        ++totals.challengeCounts[scenario.challenge];
        auto& challengeStats = totals.challengeStats[scenario.challenge];
        ++challengeStats.scenarios;
        challengeStats.requiredFacts += scenario.requiredNeedles.size();
        challengeStats.invalidatedFacts += scenario.invalidatedNeedles.size();
        challengeStats.restrictedFacts += scenario.restrictedNeedles.size();

        SpillContextStmt statement;
        statement.context = "quality_convo_" + std::to_string(index + 1);
        statement.query = scenario.query;
        statement.tab = scenario.tab;
        statement.tokenBudget = options.tokenBudget;
        statement.receipts = true;
        const auto result = engine->execute(&statement);
        const auto currentContext = fieldValue(result, "current_context");
        const auto tokenCost = fieldValue(result, "token_cost");
        const auto contextTokens = tokenCost.empty()
            ? estimateTokens(currentContext)
            : static_cast<std::size_t>(std::stoull(tokenCost));

        recordQualityResult(
            totals, scenario, "SkibidiQL", currentContext, contextTokens);

        const auto fullHistory = renderFullHistory(
            scenario.messages, scenario.query);
        recordQualityResult(
            totals, scenario, "full_history", fullHistory.text,
            estimateTokens(fullHistory.text));

        const auto recency = renderRecencyWindow(
            scenario.messages, scenario.query, options.tokenBudget);
        recordQualityResult(
            totals, scenario, "recency_window", recency.text,
            estimateTokens(recency.text));

        const auto keyword = renderKeywordScan(
            scenario.messages, scenario.query, options.tokenBudget);
        recordQualityResult(
            totals, scenario, "keyword_scan", keyword.text,
            estimateTokens(keyword.text));

        const auto rag = renderLexicalRag(
            scenario.messages, scenario.query, options.tokenBudget);
        recordQualityResult(
            totals, scenario, "lexical_rag", rag.text,
            estimateTokens(rag.text));

        const auto summary = renderSummaryMemory(
            scenario.messages, scenario.query, options.tokenBudget);
        recordQualityResult(
            totals, scenario, "extractive_summary", summary.text,
            estimateTokens(summary.text));
    }
    const auto finish = std::chrono::steady_clock::now();
    totals.elapsedMs = std::chrono::duration<double, std::milli>(
        finish - start).count();

    engine.reset();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return totals;
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
        } else if (std::strcmp(argv[index], "--quality-suite") == 0) {
            options.qualitySuite = true;
        } else if (std::strcmp(argv[index], "--scenarios") == 0 &&
                   index + 1 < argc) {
            options.scenarios = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--scenario-messages") == 0 &&
                   index + 1 < argc) {
            options.scenarioMessages = std::atoi(argv[++index]);
        } else {
            throw std::runtime_error("Invalid context benchmark argument");
        }
    }
    if (options.messages <= 0 || options.iterations <= 0 ||
        options.scenarios <= 0 || options.scenarioMessages <= 0 ||
        options.tokenBudget == 0 || options.bufferPages <= 0) {
        throw std::runtime_error("Context benchmark values must be positive");
    }
    return options;
}

double percent(std::size_t numerator, std::size_t denominator) {
    if (denominator == 0) return 100.0;
    return 100.0 * static_cast<double>(numerator) /
           static_cast<double>(denominator);
}

double tokenSavingsPercent(std::size_t baselineTokens,
                           std::size_t contextTokens) {
    if (baselineTokens == 0) return 0.0;
    return 100.0 *
           (1.0 - static_cast<double>(contextTokens) /
                      static_cast<double>(baselineTokens));
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

void printQualitySuite(const QualityTotals& totals,
                       const Options& options) {
    const auto contextIt = totals.methods.find("SkibidiQL");
    const auto recencyIt = totals.methods.find("recency_window");
    const auto ragIt = totals.methods.find("lexical_rag");
    if (contextIt == totals.methods.end() ||
        recencyIt == totals.methods.end() ||
        ragIt == totals.methods.end()) {
        throw std::runtime_error("Quality suite missing expected methods");
    }
    const auto& context = contextIt->second;
    const auto& recency = recencyIt->second;
    const auto& rag = ragIt->second;

    const double contextRecall = percent(
        context.hits, totals.requiredFacts);
    const double recencyRecall = percent(
        recency.hits, totals.requiredFacts);
    const double ragRecall = percent(
        rag.hits, totals.requiredFacts);
    const double recencySavings = tokenSavingsPercent(
        recency.tokens, context.tokens);
    const double ragSavings = tokenSavingsPercent(
        rag.tokens, context.tokens);
    const double invalidatedExclusion = percent(
        context.invalidatedExcluded, totals.invalidatedFacts);
    const double restrictedExclusion = percent(
        context.restrictedExcluded, totals.restrictedFacts);
    const double avgMessages = totals.scenarios == 0
        ? 0.0
        : static_cast<double>(totals.messages) / totals.scenarios;
    const double avgContextTokens = totals.scenarios == 0
        ? 0.0
        : static_cast<double>(context.tokens) / totals.scenarios;
    const double avgRagTokens = totals.scenarios == 0
        ? 0.0
        : static_cast<double>(rag.tokens) / totals.scenarios;

    std::cout << std::fixed << std::setprecision(1);
    std::cout
        << "On " << totals.scenarios
        << " synthetic long-running assistant conversations with "
        << "contradictions, topic switches, stale preferences, stable "
        << "preferences, task/debug state, open questions, decisions, and "
        << "ACL-restricted facts, SkibidiQL achieved "
        << contextRecall
        << "% policy-safe active recall using "
        << ragSavings
        << "% fewer tokens than lexical RAG and "
        << recencySavings
        << "% fewer tokens than recency-window memory, while correctly "
           "excluding "
        << invalidatedExclusion
        << "% of invalidated facts and "
        << restrictedExclusion
        << "% of ACL-restricted raw values.\n";

    std::cout << "\nquality_suite=true scenarios=" << totals.scenarios
              << " avg_messages=" << avgMessages
              << " token_budget=" << options.tokenBudget
              << " retrieval_time=" << totals.elapsedMs << " ms\n";
    std::cout << "| Method | What it simulates | Policy-safe active recall | Avg tokens | "
              << "Invalidated excluded | ACL raw excluded | Redactions |\n";
    std::cout << "|---|---|---:|---:|---:|---:|---:|\n";
    for (const auto& method : qualityMethodOrder()) {
        const auto found = totals.methods.find(method);
        if (found == totals.methods.end()) continue;
        const auto& stats = found->second;
        const char* description = "";
        if (method == "SkibidiQL") {
            description = "relational context DB";
        } else if (method == "full_history") {
            description = "paste every message";
        } else if (method == "recency_window") {
            description = "last messages until budget";
        } else if (method == "keyword_scan") {
            description = "query term scan";
        } else if (method == "lexical_rag") {
            description = "BM25-ish dependency-free RAG";
        } else if (method == "extractive_summary") {
            description = "rule/extractive summary proxy";
        }
        const double avgTokens = stats.scenarios == 0
            ? 0.0
            : static_cast<double>(stats.tokens) / stats.scenarios;
        std::cout << "| " << method
                  << " | " << description
                  << " | " << percent(stats.hits, totals.requiredFacts)
                  << "% | " << avgTokens
                  << " | " << percent(stats.invalidatedExcluded,
                                      totals.invalidatedFacts)
                  << "% | " << percent(stats.restrictedExcluded,
                                       totals.restrictedFacts)
                  << "% | " << stats.redactions << " |\n";
    }

    std::cout << "\nheadline_comparison: SkibidiQL avg_tokens="
              << avgContextTokens
              << ", lexical_rag avg_tokens=" << avgRagTokens
              << ", lexical_rag_policy_safe_active_recall=" << ragRecall
              << "%, recency_policy_safe_active_recall="
              << recencyRecall << "%\n";

    std::cout << "\n| Challenge flavor | Conversations |\n";
    std::cout << "|---|---:|\n";
    for (const auto& [challenge, count] : totals.challengeCounts) {
        std::cout << "| " << challenge << " | " << count << " |\n";
    }

    std::cout << "\n| Challenge flavor | SkibidiQL policy-safe active recall | "
              << "Full-history policy-safe active recall | "
              << "Recency policy-safe active recall | "
              << "RAG policy-safe active recall | "
              << "Extractive-summary policy-safe active recall |\n";
    std::cout << "|---|---:|---:|---:|---:|---:|\n";
    for (const auto& [challenge, stats] : totals.challengeStats) {
        auto recallFor = [&](const std::string& method) {
            const auto challengeFound = totals.byChallenge.find(challenge);
            if (challengeFound == totals.byChallenge.end()) return 0.0;
            const auto methodFound = challengeFound->second.find(method);
            if (methodFound == challengeFound->second.end()) return 0.0;
            return percent(methodFound->second.hits, stats.requiredFacts);
        };
        std::cout << "| " << challenge
                  << " | " << recallFor("SkibidiQL") << "%"
                  << " | " << recallFor("full_history") << "%"
                  << " | " << recallFor("recency_window") << "%"
                  << " | " << recallFor("lexical_rag") << "%"
                  << " | " << recallFor("extractive_summary") << "% |\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (options.qualitySuite) {
            const auto totals = measureQualitySuite(options);
            printQualitySuite(totals, options);
            return 0;
        }

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
