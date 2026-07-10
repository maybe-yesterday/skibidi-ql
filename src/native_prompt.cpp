#include "native_engine.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

std::string joinStrings(const std::vector<std::string>& values,
                        const std::string& separator) {
    std::string out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) out += separator;
        out += values[index];
    }
    return out;
}

void addKeyValue(NativeQueryResult& result,
                 const std::string& field,
                 const std::string& value) {
    result.rows.push_back(Tuple{Value(field), Value(value)});
}

std::string lowerText(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

std::string trimText(const std::string& value) {
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

std::string cleanAtomValue(std::string value) {
    value = trimText(value);
    const auto lower = lowerText(value);
    for (const char* stop : {" but ", " because "}) {
        const auto pos = lower.find(stop);
        if (pos != std::string::npos) {
            value = value.substr(0, pos);
            break;
        }
    }
    while (!value.empty() &&
           (value.back() == '.' || value.back() == '!' ||
            value.back() == '?' || value.back() == ',' ||
            value.back() == ';')) {
        value.pop_back();
    }
    return trimText(value);
}

void addUnique(std::vector<std::string>& values,
               const std::string& value) {
    if (value.empty()) return;
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

void removeValue(std::vector<std::string>& values,
                 const std::string& value) {
    values.erase(std::remove(values.begin(), values.end(), value),
                 values.end());
}

bool containsAny(const std::string& lower,
                 const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        if (lower.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool isWordChar(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

bool containsWord(const std::string& lower, const std::string& word) {
    std::size_t pos = lower.find(word);
    while (pos != std::string::npos) {
        const bool leftOk = pos == 0 || !isWordChar(lower[pos - 1]);
        const std::size_t end = pos + word.size();
        const bool rightOk = end >= lower.size() || !isWordChar(lower[end]);
        if (leftOk && rightOk) return true;
        pos = lower.find(word, pos + 1);
    }
    return false;
}

std::vector<std::string> tokenizeLower(const std::string& text) {
    std::vector<std::string> tokens;
    std::string token;
    auto flush = [&]() {
        if (!token.empty()) {
            tokens.push_back(token);
            token.clear();
        }
    };
    for (unsigned char ch : lowerText(text)) {
        if (std::isalnum(ch)) {
            token.push_back(static_cast<char>(ch));
        } else {
            flush();
        }
    }
    flush();
    return tokens;
}

bool isQueryStopword(const std::string& token) {
    static const std::unordered_set<std::string> stopwords = {
        "and", "are", "can", "for", "find", "from", "how",
        "near", "the", "this", "that", "with", "you"};
    return stopwords.find(token) != stopwords.end();
}

int queryValueOverlapScore(const std::string& lowerValue,
                           const std::string& lowerQuery) {
    int score = 0;
    std::string token;
    auto flush = [&]() {
        if (token.size() >= 3 && !isQueryStopword(token) &&
            lowerValue.find(token) != std::string::npos) {
            score += 20;
        }
        token.clear();
    };
    for (unsigned char ch : lowerQuery) {
        if (std::isalnum(ch)) {
            token.push_back(static_cast<char>(ch));
        } else {
            flush();
        }
    }
    flush();
    return std::min(score, 80);
}

std::vector<std::string> inferAccessLabels(const std::string& text) {
    const auto lower = lowerText(text);
    std::vector<std::string> labels;
    labels.push_back("AGENT_INTERNAL");
    if (containsAny(lower, {
            "password", "secret", "api key", "api token",
            "access token", "auth token", "bearer token",
            "refresh token", "session token", "credential token",
            "ssn", "social security", "credit card", "confidential",
            "private key"})) {
        labels.push_back("CONFIDENTIAL_CUSTOMER_DATA");
    }
    if (containsAny(lower, {"tool call", "tool trace", "stack trace"})) {
        labels.push_back("TOOL_TRACE");
    }
    return labels;
}

std::vector<std::string> inferMentionedEntities(const std::string& text) {
    const auto lower = lowerText(text);
    std::vector<std::string> entities;
    if (containsWord(lower, "dog") ||
        containsWord(lower, "puppy") ||
        containsWord(lower, "pet")) {
        addUnique(entities, "dog");
    }
    if (containsAny(lower, {"sqlite", "sql", "b-tree", "btree"})) {
        addUnique(entities, "sqlite");
    }
    if (containsAny(lower, {"benchmark", "perf", "slow", "latency"})) {
        addUnique(entities, "performance");
    }
    if (containsAny(lower, {"wal", "lock", "recovery", "transaction"})) {
        addUnique(entities, "storage-safety");
    }
    if (containsAny(lower, {"readme", "docs", "documentation"})) {
        addUnique(entities, "docs");
    }
    if (containsAny(lower, {"pytorch", "torch", "dataset", "training"})) {
        addUnique(entities, "ml-dataset");
    }
    return entities;
}

std::string normalizedTab(const std::string& tab);

struct TagSuggestion {
    std::string tag;
    int score = 0;
    std::string reason;
};

void addTagSuggestion(std::vector<TagSuggestion>& suggestions,
                      std::string tag,
                      int score,
                      std::string reason) {
    tag = normalizedTab(std::move(tag));
    for (auto& existing : suggestions) {
        if (existing.tag == tag) {
            if (score > existing.score) existing.score = score;
            if (!reason.empty() &&
                existing.reason.find(reason) == std::string::npos) {
                if (!existing.reason.empty()) existing.reason += "+";
                existing.reason += reason;
            }
            return;
        }
    }
    suggestions.push_back(TagSuggestion{std::move(tag), score,
                                        std::move(reason)});
}

std::vector<TagSuggestion> inferContextTagSuggestions(
    const std::string& text) {
    const auto lower = lowerText(text);
    std::vector<TagSuggestion> suggestions;

    if (containsWord(lower, "dog") ||
        containsWord(lower, "puppy") ||
        containsWord(lower, "pet")) {
        addTagSuggestion(suggestions, "convo about dog", 90,
                         "matched pet/dog term");
    }
    if (containsAny(lower, {"sqlite", "b-tree", "btree"}) ||
        containsWord(lower, "sql") || containsWord(lower, "join")) {
        addTagSuggestion(suggestions, "debugging sqlite perf", 85,
                         "matched sql/sqlite/join term");
    }
    if (containsAny(lower, {"benchmark", "recall", "avg tokens",
                            "average tokens"})) {
        addTagSuggestion(suggestions, "benchmarks", 82,
                         "matched benchmark metric term");
    }
    if (containsAny(lower, {"mem0", "rag", "recency-window"})) {
        addTagSuggestion(suggestions, "memory baselines", 80,
                         "matched memory baseline term");
    }
    if (containsAny(lower, {"agent", "context pack", "current_context",
                            "dogfood", "helper"})) {
        addTagSuggestion(suggestions, "agent integration", 78,
                         "matched agent/context term");
    }
    if (containsAny(lower, {"roadmap", "next step", "project"})) {
        addTagSuggestion(suggestions, "project roadmap", 76,
                         "matched project planning term");
    }
    if (containsAny(lower, {"readme", "docs", "documentation"})) {
        addTagSuggestion(suggestions, "docs", 74,
                         "matched docs/readme term");
    }
    if (containsAny(lower, {"debug this later", "investigate",
                            "look into"})) {
        addTagSuggestion(suggestions, "debug this later", 95,
                         "matched follow-up marker");
    }
    if (lower.find("?") != std::string::npos) {
        addTagSuggestion(suggestions, "open questions", 92,
                         "contains question mark");
    }
    if (containsAny(lower, {"remember that", "always ", "never ",
                            "don't ", "do not "})) {
        addTagSuggestion(suggestions, "constraints", 88,
                         "matched constraint marker");
    }
    if (containsAny(lower, {"i like ", "i prefer "})) {
        addTagSuggestion(suggestions, "preferences", 86,
                         "matched preference marker");
    }
    if (containsAny(lower, {"i need ", "i want ", "todo"})) {
        addTagSuggestion(suggestions, "current tasks", 89,
                         "matched task marker");
    }

    if (suggestions.empty()) {
        addTagSuggestion(suggestions, "main", 1, "fallback");
    }
    std::stable_sort(suggestions.begin(), suggestions.end(),
                     [](const TagSuggestion& left,
                        const TagSuggestion& right) {
                         return left.score > right.score;
                     });
    return suggestions;
}

std::vector<std::string> tagsFromSuggestions(
    const std::vector<TagSuggestion>& suggestions) {
    std::vector<std::string> tags;
    for (const auto& suggestion : suggestions) {
        addUnique(tags, suggestion.tag);
    }
    return tags;
}

std::vector<std::string> reasonsFromSuggestions(
    const std::vector<TagSuggestion>& suggestions) {
    std::vector<std::string> reasons;
    for (const auto& suggestion : suggestions) {
        addUnique(reasons, suggestion.tag + ":" +
                  std::to_string(suggestion.score) + ":" +
                  suggestion.reason);
    }
    return reasons;
}

std::string suggestionSummary(const TagSuggestion& suggestion) {
    return suggestion.tag + " score=" + std::to_string(suggestion.score) +
           " reason=" + suggestion.reason;
}

bool hasAccessLabel(const std::vector<std::string>& labels,
                    const std::string& label) {
    return std::find(labels.begin(), labels.end(), label) != labels.end();
}

bool shouldRedact(const std::vector<std::string>& labels) {
    return hasAccessLabel(labels, "CONFIDENTIAL_CUSTOMER_DATA");
}

std::string redactIfNeeded(const std::string& value,
                           const std::vector<std::string>& labels,
                           bool* redacted = nullptr) {
    if (!shouldRedact(labels)) {
        if (redacted) *redacted = false;
        return value;
    }
    if (redacted) *redacted = true;
    return "[redacted:CONFIDENTIAL_CUSTOMER_DATA]";
}

struct ExtractedValue {
    std::string value;
    std::uint64_t sourceStart = 0;
    std::uint64_t sourceEnd = 0;
    std::string originalSnippet;
};

std::optional<ExtractedValue> extractAfterMarker(const std::string& text,
                                                 const std::string& marker) {
    const auto lower = lowerText(text);
    const auto pos = lower.find(marker);
    if (pos == std::string::npos) return std::nullopt;
    const std::size_t start = pos + marker.size();
    auto value = text.substr(start);
    std::size_t boundary = std::string::npos;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (ch == '\n' || ch == '\r' || ch == '!' ||
            ch == '?' || ch == ';') {
            boundary = index;
            break;
        }
        if (ch == '.') {
            const bool decimalPoint =
                index > 0 && index + 1 < value.size() &&
                std::isdigit(static_cast<unsigned char>(value[index - 1])) &&
                std::isdigit(static_cast<unsigned char>(value[index + 1]));
            const bool sentencePeriod =
                index + 1 == value.size() ||
                std::isspace(static_cast<unsigned char>(value[index + 1]));
            if (!decimalPoint && sentencePeriod) {
                boundary = index;
                break;
            }
        }
    }
    if (boundary != std::string::npos) {
        value = value.substr(0, boundary);
    }
    const auto originalSnippet = trimText(value);
    value = cleanAtomValue(value);
    if (value.empty()) return std::nullopt;
    ExtractedValue extracted;
    extracted.value = std::move(value);
    extracted.sourceStart = static_cast<std::uint64_t>(start);
    extracted.sourceEnd = static_cast<std::uint64_t>(
        start + (boundary == std::string::npos
                     ? originalSnippet.size()
                     : boundary));
    extracted.originalSnippet = originalSnippet;
    return extracted;
}

bool isAtomSlugStopword(const std::string& token) {
    static const std::unordered_set<std::string> stopwords = {
        "a", "an", "and", "are", "as", "at", "be", "by", "for",
        "from", "in", "inside", "is", "it", "of", "on", "or",
        "should", "that", "the", "this", "to", "use", "using",
        "we", "with", "also", "says", "measured"};
    return stopwords.find(token) != stopwords.end();
}

std::string atomTopicSlug(const std::string& value) {
    std::string slug;
    std::size_t kept = 0;
    for (const auto& token : tokenizeLower(value)) {
        if (token.size() < 3 || isAtomSlugStopword(token)) continue;
        if (!slug.empty()) slug += "_";
        slug += token;
        if (++kept == 4) break;
    }
    return slug.empty() ? "general" : slug;
}

std::string specificAtomKey(const std::string& base,
                            const std::string& value) {
    const auto lower = lowerText(value);
    if (base == "user_constraint" &&
        containsAny(lower, {"password", "secret", "api key",
                            "api token", "access token",
                            "bearer token", "private key",
                            "credit card", "ssn"})) {
        return "user_constraint.security";
    }
    if (base == "decision" ||
        base == "open_question" ||
        base == "debug_followup" ||
        base == "current_task" ||
        base == "user_constraint") {
        return base + "." + atomTopicSlug(value);
    }
    return base;
}

std::string normalizedTab(const std::string& tab) {
    const auto cleaned = cleanAtomValue(tab);
    return cleaned.empty() ? "main" : cleaned;
}

std::string suggestContextTab(const std::string& text) {
    return inferContextTagSuggestions(text).front().tag;
}

std::vector<ContextAtomMeta> extractContextAtoms(
    const AppendMemoryStmt& statement) {
    std::vector<ContextAtomMeta> atoms;
    const std::string source =
        "message_" + std::to_string(statement.messageId);
    auto add = [&](std::string key,
                   const ExtractedValue& extracted,
                   std::string type,
                   std::string extractorRule,
                   std::string confidence = "0.92") {
        if (extracted.value.empty()) return;
        ContextAtomMeta atom;
        atom.key = specificAtomKey(key, extracted.value);
        atom.value = extracted.value;
        atom.type = std::move(type);
        atom.status = "active";
        atom.source = source;
        atom.tab = normalizedTab(statement.tab);
        atom.extractorRule = std::move(extractorRule);
        atom.extractorConfidence = std::move(confidence);
        atom.sourceStart = extracted.sourceStart;
        atom.sourceEnd = extracted.sourceEnd;
        atom.originalSnippet = extracted.originalSnippet;
        atoms.push_back(std::move(atom));
    };

    for (const char* marker : {
             "i live in ", "i moved to ", "moved to ",
             "i am in ", "i'm in ", "my location is ",
             "i live near "}) {
        if (auto extracted = extractAfterMarker(statement.text, marker)) {
            add("user_location", *extracted, "fact",
                std::string("marker:") + marker);
            break;
        }
    }
    for (const char* marker : {"i prefer ", "i like "}) {
        if (auto extracted = extractAfterMarker(statement.text, marker)) {
            add("user_preference", *extracted, "preference",
                std::string("marker:") + marker);
            break;
        }
    }
    for (const char* marker : {
             "my dog likes ", "my dog loves ", "my dog prefers "}) {
        if (auto extracted = extractAfterMarker(statement.text, marker)) {
            add("dog_preference", *extracted, "preference",
                std::string("marker:") + marker);
            break;
        }
    }
    for (const char* marker : {
             "my dog is named ", "my dog's name is "}) {
        if (auto extracted = extractAfterMarker(statement.text, marker)) {
            add("dog_name", *extracted, "fact",
                std::string("marker:") + marker);
            break;
        }
    }
    if (auto extracted = extractAfterMarker(statement.text,
                                           "remember that ")) {
        add("user_constraint", *extracted, "constraint",
            "marker:remember that");
    }
    for (const char* marker : {
             "always ", "never ", "do not ", "don't "}) {
        if (auto extracted = extractAfterMarker(statement.text, marker)) {
            ExtractedValue constraint = *extracted;
            constraint.value = cleanAtomValue(std::string(marker) +
                                              extracted->value);
            constraint.originalSnippet =
                cleanAtomValue(std::string(marker) +
                               extracted->originalSnippet);
            add("user_constraint", constraint, "constraint",
                std::string("marker:") + marker);
            break;
        }
    }
    for (const char* marker : {"i need ", "i want "}) {
        if (auto extracted = extractAfterMarker(statement.text, marker)) {
            add("current_task", *extracted, "task",
                std::string("marker:") + marker);
            break;
        }
    }
    for (const char* marker : {
             "todo: ", "todo ", "debug this later: ",
             "investigate ", "look into "}) {
        if (auto extracted = extractAfterMarker(statement.text, marker)) {
            add("debug_followup", *extracted, "debug",
                std::string("marker:") + marker);
            break;
        }
    }
    for (const char* marker : {
             "we decided ", "decision: ", "final call: "}) {
        if (auto extracted = extractAfterMarker(statement.text, marker)) {
            add("decision", *extracted, "decision",
                std::string("marker:") + marker);
            break;
        }
    }
    if (statement.text.find('?') != std::string::npos) {
        const auto question = cleanAtomValue(statement.text);
        if (!question.empty()) {
            ExtractedValue extracted;
            extracted.value = question;
            extracted.sourceStart = 0;
            extracted.sourceEnd =
                static_cast<std::uint64_t>(statement.text.size());
            extracted.originalSnippet = statement.text;
            add("open_question", extracted, "question",
                "punctuation:question_mark", "0.75");
        }
    }
    return atoms;
}

std::string contextCacheKey(std::uint64_t revision,
                            const std::vector<std::string>& parts) {
    std::string key = std::to_string(revision);
    for (const auto& part : parts) {
        key += '\x1f';
        key += part;
    }
    return key;
}

int contextAtomUtilityLower(const ContextAtomMeta& atom,
                            const std::string& lowerQuery) {
    const auto value = lowerText(atom.value);
    int score = 10;
    if (lowerQuery.find(value) != std::string::npos && !value.empty()) {
        score += 40;
    }
    score += queryValueOverlapScore(value, lowerQuery);
    if (atom.key == "user_location" &&
        (lowerQuery.find("near me") != std::string::npos ||
         lowerQuery.find("nearby") != std::string::npos ||
         lowerQuery.find("restaurant") != std::string::npos ||
         lowerQuery.find("weather") != std::string::npos ||
         lowerQuery.find("location") != std::string::npos ||
         lowerQuery.find("near ") != std::string::npos)) {
        score += 100;
    }
    if (atom.type == "constraint") score += 80;
    if (atom.type == "preference" &&
        (lowerQuery.find("recommend") != std::string::npos ||
         lowerQuery.find("choose") != std::string::npos ||
         lowerQuery.find("prefer") != std::string::npos)) {
        score += 60;
    }
    if (atom.type == "preference" &&
        containsAny(lowerQuery, {
            "restaurant", "cafe", "food", "dinner", "lunch"}) &&
        containsAny(value, {
            "restaurant", "cafe", "food", "quiet", "coffee"})) {
        score += 90;
    }
    if (atom.type == "task") score += 50;
    if (atom.type == "decision") score += 70;
    if (atom.type == "question" &&
        (lowerQuery.find("question") != std::string::npos ||
         lowerQuery.find("open") != std::string::npos ||
         lowerQuery.find("?") != std::string::npos)) {
        score += 70;
    }
    if (atom.type == "debug" &&
        (lowerQuery.find("debug") != std::string::npos ||
         lowerQuery.find("todo") != std::string::npos ||
         lowerQuery.find("investigate") != std::string::npos)) {
        score += 70;
    }
    return score;
}

std::size_t estimateContextTokens(const std::string& text) {
    return std::max<std::size_t>(1, (text.size() + 3) / 4);
}

std::string renderContextAtom(const ContextAtomMeta& atom,
                              bool receipts,
                              bool* redacted = nullptr) {
    std::string rendered = atom.type + " " + atom.key + "=" +
        redactIfNeeded(atom.value, atom.accessLabels, redacted);
    if (receipts) {
        rendered += " @" + atom.source;
        if (!atom.schemaName.empty()) {
            rendered += " schema=" + atom.schemaName + "." +
                        atom.schemaVersion;
        }
        if (!atom.accessLabels.empty()) {
            rendered += " labels=" + joinStrings(atom.accessLabels, ",");
        }
        if (!atom.tags.empty()) {
            rendered += " tags=" + joinStrings(atom.tags, ",");
        }
        if (!atom.extractorRule.empty()) {
            rendered += " rule=" + atom.extractorRule;
        }
        if (!atom.extractorConfidence.empty()) {
            rendered += " confidence=" + atom.extractorConfidence;
        }
        if (atom.sourceEnd > atom.sourceStart) {
            rendered += " span=" + std::to_string(atom.sourceStart) +
                        ":" + std::to_string(atom.sourceEnd);
        }
    }
    return rendered;
}

std::string contextAtomDiversityKey(const ContextAtomMeta& atom) {
    return atom.type + '\x1f' + atom.key + '\x1f' +
           lowerText(atom.value);
}

std::string resolveContextTab(const ConversationContextMeta& context,
                              const std::string& tab) {
    std::string current = normalizedTab(tab);
    std::unordered_set<std::string> seen;
    for (std::size_t hop = 0; hop < 32; ++hop) {
        if (!seen.insert(current).second) return current;
        bool changed = false;
        for (const auto& alias : context.tabAliases) {
            if (normalizedTab(alias.alias) == current) {
                current = normalizedTab(alias.target);
                changed = true;
                break;
            }
        }
        if (!changed) return current;
    }
    return current;
}

bool contextMatchesTab(const ConversationContextMeta& context,
                       const std::string& primaryTab,
                       const std::vector<std::string>& tags,
                       const std::string& tabFilter) {
    if (tabFilter.empty()) return true;
    if (resolveContextTab(context, primaryTab) == tabFilter) return true;
    for (const auto& tag : tags) {
        if (resolveContextTab(context, tag) == tabFilter) return true;
    }
    return false;
}

void setContextTabAlias(ConversationContextMeta& context,
                        const std::string& aliasValue,
                        const std::string& targetValue) {
    const std::string alias = normalizedTab(aliasValue);
    const std::string target = resolveContextTab(context, targetValue);
    for (auto& existing : context.tabAliases) {
        if (normalizedTab(existing.alias) == alias) {
            existing.alias = alias;
            existing.target = target;
            return;
        }
    }
    context.tabAliases.push_back(ContextTabAliasMeta{alias, target});
}

std::vector<std::string> contextAliasesForTab(
    const ConversationContextMeta& context,
    const std::string& tab) {
    std::vector<std::string> aliases;
    const std::string target = normalizedTab(tab);
    for (const auto& alias : context.tabAliases) {
        if (resolveContextTab(context, alias.alias) == target) {
            aliases.push_back(normalizedTab(alias.alias));
        }
    }
    std::sort(aliases.begin(), aliases.end());
    aliases.erase(std::unique(aliases.begin(), aliases.end()),
                  aliases.end());
    return aliases;
}

void recomputeContextAtomStatus(ConversationContextMeta& context) {
    for (auto& atom : context.atoms) {
        atom.tab = normalizedTab(atom.tab);
        for (auto& tag : atom.tags) tag = normalizedTab(tag);
        std::sort(atom.tags.begin(), atom.tags.end());
        atom.tags.erase(std::unique(atom.tags.begin(), atom.tags.end()),
                        atom.tags.end());
        atom.status = "active";
        atom.invalidatedBy.clear();
    }

    for (std::size_t index = 0; index < context.atoms.size(); ++index) {
        const auto& atom = context.atoms[index];
        for (std::size_t prior = 0; prior < index; ++prior) {
            auto& existing = context.atoms[prior];
            if (existing.status == "active" &&
                existing.key == atom.key &&
                existing.tab == atom.tab &&
                existing.value != atom.value) {
                existing.status = "invalidated";
                existing.invalidatedBy = atom.source;
            }
        }
    }
}

} // namespace

NativeQueryResult NativeEngine::executeCreateContext(
    const CreateContextStmt& statement) {
    if (catalog_.hasContext(statement.name)) {
        throw std::runtime_error(
            "Context already exists: " + statement.name);
    }
    ConversationContextMeta context;
    context.name = statement.name;
    catalog_.addContext(context);
    saveCatalog();
    contextResultCache_.clear();

    NativeQueryResult result;
    result.columns = {"field", "value"};
    addKeyValue(result, "context", statement.name);
    addKeyValue(result, "messages", "0");
    addKeyValue(result, "atoms", "0");
    result.message = "manifested context " + statement.name;
    return result;
}

NativeQueryResult NativeEngine::executeAppendMemory(
    const AppendMemoryStmt& statement) {
    const auto* existing = catalog_.getContext(statement.context);
    if (!existing) {
        throw std::runtime_error("Unknown context: " + statement.context);
    }
    ConversationContextMeta context = *existing;
    for (const auto& message : context.messages) {
        if (message.id == statement.messageId) {
            throw std::runtime_error(
                "Duplicate context message id: " +
                std::to_string(statement.messageId));
        }
    }

    ContextMessageMeta message;
    message.id = statement.messageId;
    message.speaker = statement.speaker;
    message.text = statement.text;
    const auto tagSuggestions = inferContextTagSuggestions(statement.text);
    const std::string suggestedTab = tagSuggestions.front().tag;
    const std::string requestedTab =
        statement.autoTab ? suggestedTab : statement.tab;
    message.tab = resolveContextTab(context, requestedTab);
    message.schemaName = "ConversationMessage";
    message.schemaVersion = "v1";
    message.storageRoute =
        "structured=catalog.contexts.messages; vector=ConversationMessage.content; blob=none";
    message.accessLabels = inferAccessLabels(statement.text);
    message.mentionedEntities = inferMentionedEntities(statement.text);
    message.tags = tagsFromSuggestions(tagSuggestions);
    addUnique(message.tags, message.tab);
    for (auto& tag : message.tags) {
        tag = resolveContextTab(context, tag);
    }
    std::sort(message.tags.begin(), message.tags.end());
    message.tags.erase(std::unique(message.tags.begin(), message.tags.end()),
                       message.tags.end());
    message.tagReasons = reasonsFromSuggestions(tagSuggestions);
    if (!statement.autoTab && !statement.tab.empty()) {
        addUnique(message.tagReasons,
                  message.tab + ":100:explicit vibe-tab");
    }
    context.messages.push_back(message);

    AppendMemoryStmt extractionStatement = statement;
    extractionStatement.tab = message.tab;
    auto atoms = extractContextAtoms(extractionStatement);
    const std::string invalidator =
        "message_" + std::to_string(statement.messageId);
    std::size_t invalidated = 0;
    for (const auto& atom : atoms) {
        for (auto& existingAtom : context.atoms) {
            if (existingAtom.status == "active" &&
                existingAtom.key == atom.key &&
                existingAtom.tab == atom.tab &&
                existingAtom.value != atom.value) {
                existingAtom.status = "invalidated";
                existingAtom.invalidatedBy = invalidator;
                ++invalidated;
            }
        }
        ContextAtomMeta storedAtom = atom;
        storedAtom.schemaName = "ContextAtom";
        storedAtom.schemaVersion = "v1";
        storedAtom.accessLabels = message.accessLabels;
        storedAtom.tags = message.tags;
        context.atoms.push_back(std::move(storedAtom));
    }

    catalog_.addContext(context);
    saveCatalog();
    contextResultCache_.clear();

    NativeQueryResult result;
    result.columns = {"field", "value"};
    addKeyValue(result, "context", statement.context);
    addKeyValue(result, "message", invalidator);
    addKeyValue(result, "speaker", statement.speaker);
    addKeyValue(result, "tab", message.tab);
    addKeyValue(result, "suggested_tab", suggestedTab);
    addKeyValue(result, "suggested_tab_reason",
                suggestionSummary(tagSuggestions.front()));
    addKeyValue(result, "auto_tab", statement.autoTab ? "true" : "false");
    addKeyValue(result, "tags", joinStrings(message.tags, ","));
    addKeyValue(result, "tag_reasons",
                joinStrings(message.tagReasons, " | "));
    addKeyValue(result, "message_schema",
                message.schemaName + "." + message.schemaVersion);
    addKeyValue(result, "access_labels",
                joinStrings(message.accessLabels, ","));
    addKeyValue(result, "mentioned_entities",
                joinStrings(message.mentionedEntities, ","));
    addKeyValue(result, "storage_route", message.storageRoute);
    addKeyValue(result, "dcf_object",
                "message_" + std::to_string(statement.messageId) +
                " -> " + message.schemaName + "." +
                message.schemaVersion);
    addKeyValue(result, "extracted_atoms", std::to_string(atoms.size()));
    addKeyValue(result, "invalidated_atoms", std::to_string(invalidated));
    for (const auto& atom : atoms) {
        addKeyValue(result, "atom",
                    atom.key + "=" + atom.value + " @" + atom.source);
        addKeyValue(result, "atom_provenance",
                    atom.key + " rule=" + atom.extractorRule +
                    " confidence=" + atom.extractorConfidence +
                    " span=" + std::to_string(atom.sourceStart) +
                    ":" + std::to_string(atom.sourceEnd));
    }
    result.message = "yeeted memory into " + statement.context;
    return result;
}

NativeQueryResult NativeEngine::executeTagMemory(
    const TagMemoryStmt& statement) {
    const auto* existing = catalog_.getContext(statement.context);
    if (!existing) {
        throw std::runtime_error("Unknown context: " + statement.context);
    }
    ConversationContextMeta context = *existing;
    const std::string source =
        "message_" + std::to_string(statement.messageId);
    const std::string tab = resolveContextTab(context, statement.tab);

    bool found = false;
    std::string previousTab;
    for (auto& message : context.messages) {
        if (message.id == statement.messageId) {
            previousTab = resolveContextTab(context, message.tab);
            message.tab = tab;
            removeValue(message.tags, previousTab);
            addUnique(message.tags, tab);
            addUnique(message.tagReasons,
                      tab + ":100:manual retag");
            found = true;
            break;
        }
    }
    if (!found) {
        throw std::runtime_error(
            "Unknown context message id: " +
            std::to_string(statement.messageId));
    }

    std::size_t retaggedAtoms = 0;
    for (auto& atom : context.atoms) {
        if (atom.source == source) {
            const std::string atomPreviousTab =
                resolveContextTab(context, atom.tab);
            atom.tab = tab;
            removeValue(atom.tags, atomPreviousTab);
            addUnique(atom.tags, tab);
            ++retaggedAtoms;
        }
    }
    recomputeContextAtomStatus(context);

    catalog_.addContext(context);
    saveCatalog();
    contextResultCache_.clear();

    NativeQueryResult result;
    result.columns = {"field", "value"};
    addKeyValue(result, "context", statement.context);
    addKeyValue(result, "message", source);
    addKeyValue(result, "tab", tab);
    addKeyValue(result, "retagged_atoms", std::to_string(retaggedAtoms));
    result.message = "vibe-tabbed memory in " + statement.context;
    return result;
}

NativeQueryResult NativeEngine::executeShowTabs(
    const ShowTabsStmt& statement) {
    const auto* context = catalog_.getContext(statement.context);
    if (!context) {
        throw std::runtime_error("Unknown context: " + statement.context);
    }

    struct TabStats {
        std::size_t messages = 0;
        std::size_t atoms = 0;
        std::size_t activeAtoms = 0;
        std::size_t invalidatedAtoms = 0;
        std::uint64_t lastMessage = 0;
        std::vector<std::string> accessLabels;
        std::vector<std::string> tags;
        std::vector<std::string> tagReasons;
    };

    std::map<std::string, TabStats> stats;
    for (const auto& message : context->messages) {
        const std::string tab = resolveContextTab(*context, message.tab);
        auto& entry = stats[tab];
        ++entry.messages;
        entry.lastMessage = std::max(entry.lastMessage, message.id);
        for (const auto& label : message.accessLabels) {
            addUnique(entry.accessLabels, label);
        }
        for (const auto& tag : message.tags) {
            addUnique(entry.tags, resolveContextTab(*context, tag));
        }
        for (const auto& reason : message.tagReasons) {
            addUnique(entry.tagReasons, reason);
        }
    }
    for (const auto& atom : context->atoms) {
        const std::string tab = resolveContextTab(*context, atom.tab);
        auto& entry = stats[tab];
        ++entry.atoms;
        if (atom.status == "active") ++entry.activeAtoms;
        else if (atom.status == "invalidated") ++entry.invalidatedAtoms;
        for (const auto& label : atom.accessLabels) {
            addUnique(entry.accessLabels, label);
        }
        for (const auto& tag : atom.tags) {
            addUnique(entry.tags, resolveContextTab(*context, tag));
        }
    }
    for (const auto& alias : context->tabAliases) {
        stats[resolveContextTab(*context, alias.target)];
    }
    if (stats.empty()) stats["main"] = TabStats{};

    NativeQueryResult result;
    result.columns = {"tab", "messages", "atoms", "active_atoms",
                      "invalidated_atoms", "last_message", "aliases",
                      "access_labels", "tags", "tag_reasons"};
    for (const auto& entry : stats) {
        const auto aliases =
            contextAliasesForTab(*context, entry.first);
        result.rows.push_back(Tuple{
            Value(entry.first),
            Value(std::to_string(entry.second.messages)),
            Value(std::to_string(entry.second.atoms)),
            Value(std::to_string(entry.second.activeAtoms)),
            Value(std::to_string(entry.second.invalidatedAtoms)),
            Value(entry.second.lastMessage == 0
                      ? std::string{}
                      : std::to_string(entry.second.lastMessage)),
            Value(joinStrings(aliases, ",")),
            Value(joinStrings(entry.second.accessLabels, ",")),
            Value(joinStrings(entry.second.tags, ",")),
            Value(joinStrings(entry.second.tagReasons, " | "))});
    }
    result.message = "showed tabs for " + context->name;
    return result;
}

NativeQueryResult NativeEngine::executeShowContextSchemas(
    const ShowContextSchemasStmt&) {
    ++stats_.contextSchemaQueries;
    const std::string key =
        contextCacheKey(catalog_.revision(), {"schemas"});
    auto cached = contextResultCache_.find(key);
    if (cached != contextResultCache_.end()) {
        ++stats_.contextCacheHits;
        return cached->second;
    }
    ++stats_.contextCacheMisses;

    NativeQueryResult result;
    result.columns = {"schema", "version", "owner_agent",
                      "sensitivity", "retention", "storage",
                      "vectorized_fields", "access_labels",
                      "indexed_fields", "related_schemas"};
    for (const auto& schema : catalog_.contextSchemas()) {
        result.rows.push_back(Tuple{
            Value(schema.name),
            Value(schema.version),
            Value(schema.ownerAgentId),
            Value(schema.sensitivityLevel),
            Value(schema.retentionPolicy),
            Value(schema.storageBackend),
            Value(joinStrings(schema.vectorizedFields, ",")),
            Value(joinStrings(schema.accessLabels, ",")),
            Value(joinStrings(schema.indexedFields, ",")),
            Value(joinStrings(schema.relatedSchemas, ","))});
    }
    result.message =
        "showed CSR schema registry fr fr";
    contextResultCache_[key] = result;
    return result;
}

NativeQueryResult NativeEngine::executeShowContextObjects(
    const ShowContextObjectsStmt& statement) {
    ++stats_.contextObjectQueries;
    const std::string key =
        contextCacheKey(catalog_.revision(),
                        {"objects", statement.context});
    auto cached = contextResultCache_.find(key);
    if (cached != contextResultCache_.end()) {
        ++stats_.contextCacheHits;
        return cached->second;
    }
    ++stats_.contextCacheMisses;

    const auto* context = catalog_.getContext(statement.context);
    if (!context) {
        throw std::runtime_error("Unknown context: " + statement.context);
    }

    auto fallbackLabels = [](std::vector<std::string> labels) {
        if (labels.empty()) labels.push_back("AGENT_INTERNAL");
        return labels;
    };

    NativeQueryResult result;
    result.columns = {"object_id", "schema", "version", "tab", "status",
                      "access_labels", "storage_route", "source",
                      "value", "tags", "extractor_rule",
                      "extractor_confidence", "source_span",
                      "original_snippet"};

    std::size_t redacted = 0;
    for (const auto& message : context->messages) {
        auto labels = fallbackLabels(message.accessLabels);
        bool wasRedacted = false;
        const std::string tab = resolveContextTab(*context, message.tab);
        result.rows.push_back(Tuple{
            Value("message_" + std::to_string(message.id)),
            Value(message.schemaName.empty()
                      ? std::string("ConversationMessage")
                      : message.schemaName),
            Value(message.schemaVersion.empty()
                      ? std::string("v1")
                      : message.schemaVersion),
            Value(tab),
            Value(std::string("active")),
            Value(joinStrings(labels, ",")),
            Value(message.storageRoute.empty()
                      ? std::string("structured=catalog.contexts.messages; vector=ConversationMessage.content; blob=none")
                      : message.storageRoute),
            Value(std::string("")),
            Value(redactIfNeeded(message.text, labels, &wasRedacted)),
            Value(joinStrings(message.tags, ",")),
            Value(std::string("message")),
            Value(std::string("1.00")),
            Value(std::string("0:") + std::to_string(message.text.size())),
            Value(redactIfNeeded(message.text, labels))});
        if (wasRedacted) ++redacted;
    }

    for (std::size_t index = 0; index < context->atoms.size(); ++index) {
        const auto& atom = context->atoms[index];
        auto labels = fallbackLabels(atom.accessLabels);
        bool wasRedacted = false;
        const std::string value =
            atom.type + " " + atom.key + "=" +
            redactIfNeeded(atom.value, labels, &wasRedacted);
        result.rows.push_back(Tuple{
            Value("atom_" + std::to_string(index + 1)),
            Value(atom.schemaName.empty()
                      ? std::string("ContextAtom")
                      : atom.schemaName),
            Value(atom.schemaVersion.empty()
                      ? std::string("v1")
                      : atom.schemaVersion),
            Value(resolveContextTab(*context, atom.tab)),
            Value(atom.status.empty() ? std::string("active")
                                      : atom.status),
            Value(joinStrings(labels, ",")),
            Value(std::string("structured=catalog.contexts.atoms")),
            Value(atom.source),
            Value(value),
            Value(joinStrings(atom.tags, ",")),
            Value(atom.extractorRule),
            Value(atom.extractorConfidence),
            Value(std::to_string(atom.sourceStart) + ":" +
                  std::to_string(atom.sourceEnd)),
            Value(redactIfNeeded(atom.originalSnippet.empty()
                                      ? atom.value
                                      : atom.originalSnippet,
                                  labels))});
        if (wasRedacted) ++redacted;
    }

    result.message =
        "showed context objects for " + context->name +
        " (redacted " + std::to_string(redacted) + ")";
    contextResultCache_[key] = result;
    return result;
}

NativeQueryResult NativeEngine::executeAliasTab(
    const AliasTabStmt& statement) {
    const auto* existing = catalog_.getContext(statement.context);
    if (!existing) {
        throw std::runtime_error("Unknown context: " + statement.context);
    }
    ConversationContextMeta context = *existing;
    const std::string alias = normalizedTab(statement.alias);
    const std::string target = resolveContextTab(context, statement.target);
    setContextTabAlias(context, alias, target);

    catalog_.addContext(context);
    saveCatalog();
    contextResultCache_.clear();

    NativeQueryResult result;
    result.columns = {"field", "value"};
    addKeyValue(result, "context", statement.context);
    addKeyValue(result, "alias", alias);
    addKeyValue(result, "target", target);
    result.message = "aliased tab in " + statement.context;
    return result;
}

NativeQueryResult NativeEngine::executeMergeTabs(
    const MergeTabsStmt& statement) {
    const auto* existing = catalog_.getContext(statement.context);
    if (!existing) {
        throw std::runtime_error("Unknown context: " + statement.context);
    }
    ConversationContextMeta context = *existing;
    const std::string source = resolveContextTab(context, statement.fromTab);
    const std::string target = resolveContextTab(context, statement.toTab);

    std::size_t movedMessages = 0;
    std::size_t movedAtoms = 0;
    if (source != target) {
        for (auto& message : context.messages) {
            if (resolveContextTab(context, message.tab) == source) {
                message.tab = target;
                ++movedMessages;
            }
            for (auto& tag : message.tags) {
                if (resolveContextTab(context, tag) == source) {
                    tag = target;
                }
            }
            std::sort(message.tags.begin(), message.tags.end());
            message.tags.erase(std::unique(message.tags.begin(),
                                           message.tags.end()),
                               message.tags.end());
        }
        for (auto& atom : context.atoms) {
            if (resolveContextTab(context, atom.tab) == source) {
                atom.tab = target;
                ++movedAtoms;
            }
            for (auto& tag : atom.tags) {
                if (resolveContextTab(context, tag) == source) {
                    tag = target;
                }
            }
            std::sort(atom.tags.begin(), atom.tags.end());
            atom.tags.erase(std::unique(atom.tags.begin(),
                                        atom.tags.end()),
                            atom.tags.end());
        }
        for (auto& alias : context.tabAliases) {
            if (resolveContextTab(context, alias.target) == source) {
                alias.target = target;
            }
        }
        setContextTabAlias(context, source, target);
        recomputeContextAtomStatus(context);
    }

    catalog_.addContext(context);
    saveCatalog();
    contextResultCache_.clear();

    NativeQueryResult result;
    result.columns = {"field", "value"};
    addKeyValue(result, "context", statement.context);
    addKeyValue(result, "from_tab", source);
    addKeyValue(result, "to_tab", target);
    addKeyValue(result, "moved_messages", std::to_string(movedMessages));
    addKeyValue(result, "moved_atoms", std::to_string(movedAtoms));
    result.message = "merged tabs in " + statement.context;
    return result;
}

NativeQueryResult NativeEngine::executeExplainContext(
    const ExplainContextStmt& statement) {
    const std::string key =
        contextCacheKey(catalog_.revision(),
                        {"explain", statement.context, statement.tab,
                         statement.query,
                         std::to_string(statement.tokenBudget),
                         statement.receipts ? "receipts:on"
                                            : "receipts:off"});
    auto cached = contextResultCache_.find(key);
    if (cached != contextResultCache_.end()) {
        ++stats_.contextCacheHits;
        return cached->second;
    }
    ++stats_.contextCacheMisses;

    const auto* context = catalog_.getContext(statement.context);
    if (!context) {
        throw std::runtime_error("Unknown context: " + statement.context);
    }

    struct PlanAtom {
        const ContextAtomMeta* atom = nullptr;
        int utility = 0;
        std::size_t ordinal = 0;
        std::size_t tokenCost = 0;
        bool redacted = false;
    };

    const std::string tabFilter = statement.tab.empty()
        ? std::string{}
        : resolveContextTab(*context, statement.tab);
    const std::string lowerQuery = lowerText(statement.query);

    std::size_t scannedMessages = 0;
    std::size_t scannedAtoms = 0;
    std::size_t prunedTabMessages = 0;
    std::size_t prunedTabAtoms = 0;
    std::size_t prunedInvalidatedAtoms = 0;
    std::size_t allActiveTokenCost = 0;
    std::vector<std::string> accessLabels;
    std::vector<std::string> mentionedEntities;
    std::vector<PlanAtom> active;
    std::unordered_map<std::string, std::size_t> activeByDiversityKey;
    active.reserve(context->atoms.size());
    activeByDiversityKey.reserve(context->atoms.size());

    for (const auto& message : context->messages) {
        if (!contextMatchesTab(*context, message.tab, message.tags,
                               tabFilter)) {
            ++prunedTabMessages;
            continue;
        }
        ++scannedMessages;
        for (const auto& label : message.accessLabels) {
            addUnique(accessLabels, label);
        }
        for (const auto& entity : message.mentionedEntities) {
            addUnique(mentionedEntities, entity);
        }
    }

    for (std::size_t index = 0; index < context->atoms.size(); ++index) {
        const auto& atom = context->atoms[index];
        if (!contextMatchesTab(*context, atom.tab, atom.tags, tabFilter)) {
            ++prunedTabAtoms;
            continue;
        }
        ++scannedAtoms;
        for (const auto& label : atom.accessLabels) {
            addUnique(accessLabels, label);
        }
        if (atom.status != "active") {
            ++prunedInvalidatedAtoms;
            continue;
        }
        bool redacted = false;
        const auto rendered =
            renderContextAtom(atom, statement.receipts, &redacted);
        const auto tokenCost = estimateContextTokens(rendered);
        PlanAtom candidate{&atom,
                           contextAtomUtilityLower(atom, lowerQuery),
                           index,
                           tokenCost,
                           redacted};
        auto diversityKey = contextAtomDiversityKey(atom);
        auto existing = activeByDiversityKey.find(diversityKey);
        if (existing == activeByDiversityKey.end()) {
            activeByDiversityKey.emplace(
                std::move(diversityKey), active.size());
            active.push_back(candidate);
            allActiveTokenCost += tokenCost;
        } else {
            auto& current = active[existing->second];
            if (candidate.utility > current.utility ||
                (candidate.utility == current.utility &&
                 candidate.ordinal > current.ordinal)) {
                allActiveTokenCost -= current.tokenCost;
                current = candidate;
                allActiveTokenCost += tokenCost;
            }
        }
    }

    std::stable_sort(active.begin(), active.end(),
        [](const PlanAtom& left, const PlanAtom& right) {
        if (left.utility != right.utility) {
            return left.utility > right.utility;
        }
        return left.ordinal > right.ordinal;
    });

    std::size_t renderedAtoms = 0;
    std::size_t tokenCost = 0;
    std::size_t redactedAtoms = 0;
    for (const auto& atom : active) {
        if (renderedAtoms > 0 &&
            tokenCost + atom.tokenCost > statement.tokenBudget) {
            continue;
        }
        if (renderedAtoms == 0 &&
            atom.tokenCost > statement.tokenBudget) {
            continue;
        }
        ++renderedAtoms;
        tokenCost += atom.tokenCost;
        if (atom.redacted) ++redactedAtoms;
    }

    const auto savedTokens =
        allActiveTokenCost > tokenCost ? allActiveTokenCost - tokenCost : 0;
    const auto budgetPrunedAtoms =
        active.size() > renderedAtoms ? active.size() - renderedAtoms : 0;

    NativeQueryResult result;
    result.columns = {"field", "value"};
    addKeyValue(result, "context", context->name);
    addKeyValue(result, "thesis",
                "SkibidiQL maintains the prompt as a relational view");
    addKeyValue(result, "materialized_view",
                "CURRENT_CONTEXT(context,tab,query,token_budget)");
    addKeyValue(result, "query", statement.query);
    addKeyValue(result, "tab",
                tabFilter.empty() ? "all" : tabFilter);
    addKeyValue(result, "token_budget",
                std::to_string(statement.tokenBudget));
    addKeyValue(result, "plan",
                "Scan messages -> Scan atoms -> Prune tab/status/ACL -> "
                "Rank utility+recency -> Budget render -> Emit receipts");
    addKeyValue(result, "scan_messages",
                std::to_string(scannedMessages) + "/" +
                std::to_string(context->messages.size()));
    addKeyValue(result, "scan_atoms",
                std::to_string(scannedAtoms) + "/" +
                std::to_string(context->atoms.size()));
    addKeyValue(result, "pruned_tab_messages",
                std::to_string(prunedTabMessages));
    addKeyValue(result, "pruned_tab_atoms",
                std::to_string(prunedTabAtoms));
    addKeyValue(result, "pruned_invalidated_atoms",
                std::to_string(prunedInvalidatedAtoms));
    addKeyValue(result, "rank_candidates",
                std::to_string(active.size()));
    addKeyValue(result, "rendered_if_spilled",
                std::to_string(renderedAtoms));
    addKeyValue(result, "budget_pruned_atoms",
                std::to_string(budgetPrunedAtoms));
    addKeyValue(result, "estimated_token_cost",
                std::to_string(tokenCost));
    addKeyValue(result, "estimated_full_token_cost",
                std::to_string(allActiveTokenCost));
    addKeyValue(result, "optimizer_saved_tokens",
                std::to_string(savedTokens));
    addKeyValue(result, "redacted_atoms",
                std::to_string(redactedAtoms));
    addKeyValue(result, "ranker",
                "query utility + atom type weights + recency tie-break");
    addKeyValue(result, "context_indexes",
                "tab,tags,status,key,type,source,access_labels,mentioned_entities");
    addKeyValue(result, "provenance_model",
                "git-blame-for-prompts: source message, schema, labels, "
                "invalidated_by");
    addKeyValue(result, "cache_policy",
                "revision-aware prompt view IR cache; invalidated on "
                "context/catalog mutation");
    addKeyValue(result, "schema_registry",
                "CSR: ConversationMessage.v1 + ContextAtom.v1");
    addKeyValue(result, "dcf_storage_route",
                "structured=catalog.contexts.messages+atoms; "
                "vector=ConversationMessage.content; blob=tool/object refs");
    addKeyValue(result, "access_policy",
                "schema=agent-internal; labels=" +
                (accessLabels.empty()
                     ? std::string("AGENT_INTERNAL")
                     : joinStrings(accessLabels, ",")) +
                "; redaction=on");
    addKeyValue(result, "mentioned_entities",
                joinStrings(mentionedEntities, ","));
    if (statement.receipts) {
        for (std::size_t index = 0;
             index < active.size() && index < 5;
             ++index) {
            const auto* atom = active[index].atom;
            addKeyValue(result, "ranked_atom",
                        atom->key + "=" +
                        redactIfNeeded(atom->value, atom->accessLabels) +
                        " utility=" +
                        std::to_string(active[index].utility) +
                        " source=" + atom->source);
        }
    }
    result.message = "explained context view plan for " + context->name;
    contextResultCache_[key] = result;
    return result;
}

NativeQueryResult NativeEngine::executeSpillContext(
    const SpillContextStmt& statement) {
    ++stats_.contextSpillQueries;
    const std::string key =
        contextCacheKey(catalog_.revision(),
                        {"spill", statement.context, statement.tab,
                         statement.query,
                         std::to_string(statement.tokenBudget),
                         statement.receipts ? "receipts:on"
                                            : "receipts:off"});
    auto cached = contextResultCache_.find(key);
    if (cached != contextResultCache_.end()) {
        ++stats_.contextCacheHits;
        return cached->second;
    }
    ++stats_.contextCacheMisses;

    const auto* context = catalog_.getContext(statement.context);
    if (!context) {
        throw std::runtime_error("Unknown context: " + statement.context);
    }

    struct RankedAtom {
        const ContextAtomMeta* atom = nullptr;
        int utility = 0;
        std::size_t ordinal = 0;
    };

    std::vector<RankedAtom> active;
    std::vector<const ContextAtomMeta*> invalidated;
    std::unordered_map<std::string, std::size_t> activeByDiversityKey;
    active.reserve(context->atoms.size());
    invalidated.reserve(context->atoms.size() / 4);
    activeByDiversityKey.reserve(context->atoms.size());
    const std::string tabFilter = statement.tab.empty()
        ? std::string{}
        : resolveContextTab(*context, statement.tab);
    const std::string lowerQuery = lowerText(statement.query);
    std::vector<std::string> accessLabels;
    std::vector<std::string> mentionedEntities;
    for (const auto& message : context->messages) {
        if (!contextMatchesTab(*context, message.tab, message.tags,
                               tabFilter)) {
            continue;
        }
        for (const auto& label : message.accessLabels) {
            addUnique(accessLabels, label);
        }
        for (const auto& entity : message.mentionedEntities) {
            addUnique(mentionedEntities, entity);
        }
    }
    for (std::size_t index = 0; index < context->atoms.size(); ++index) {
        const auto& atom = context->atoms[index];
        if (!contextMatchesTab(*context, atom.tab, atom.tags, tabFilter)) {
            continue;
        }
        for (const auto& label : atom.accessLabels) {
            addUnique(accessLabels, label);
        }
        if (atom.status == "active") {
            ++stats_.contextAtomsScored;
            RankedAtom candidate{&atom,
                                 contextAtomUtilityLower(atom, lowerQuery),
                                 index};
            auto diversityKey = contextAtomDiversityKey(atom);
            auto existing = activeByDiversityKey.find(diversityKey);
            if (existing == activeByDiversityKey.end()) {
                activeByDiversityKey.emplace(
                    std::move(diversityKey), active.size());
                active.push_back(candidate);
            } else {
                auto& current = active[existing->second];
                if (candidate.utility > current.utility ||
                    (candidate.utility == current.utility &&
                     candidate.ordinal > current.ordinal)) {
                    current = candidate;
                }
            }
        } else if (atom.status == "invalidated") {
            invalidated.push_back(&atom);
        }
    }
    std::stable_sort(active.begin(), active.end(),
        [](const RankedAtom& left, const RankedAtom& right) {
        if (left.utility != right.utility) {
            return left.utility > right.utility;
        }
        return left.ordinal > right.ordinal;
    });

    std::vector<std::string> renderedAtoms;
    renderedAtoms.reserve(active.size());
    std::size_t tokenCost = 0;
    std::size_t redactedAtoms = 0;
    for (const auto& ranked : active) {
        bool wasRedacted = false;
        std::string rendered =
            renderContextAtom(*ranked.atom, statement.receipts,
                              &wasRedacted);
        const auto cost = estimateContextTokens(rendered);
        if (!renderedAtoms.empty() &&
            tokenCost + cost > statement.tokenBudget) {
            continue;
        }
        if (renderedAtoms.empty() && cost > statement.tokenBudget) {
            continue;
        }
        renderedAtoms.push_back(std::move(rendered));
        tokenCost += cost;
        if (wasRedacted) ++redactedAtoms;
    }
    stats_.contextAtomsRendered += renderedAtoms.size();
    stats_.contextAtomsRedacted += redactedAtoms;

    NativeQueryResult result;
    result.columns = {"field", "value"};
    addKeyValue(result, "context", context->name);
    addKeyValue(result, "query", statement.query);
    addKeyValue(result, "tab",
                tabFilter.empty() ? "all" : tabFilter);
    addKeyValue(result, "token_budget",
                std::to_string(statement.tokenBudget));
    addKeyValue(result, "token_cost", std::to_string(tokenCost));
    addKeyValue(result, "messages",
                std::to_string(context->messages.size()));
    addKeyValue(result, "active_atoms", std::to_string(active.size()));
    addKeyValue(result, "rendered_atoms",
                std::to_string(renderedAtoms.size()));
    addKeyValue(result, "redacted_atoms", std::to_string(redactedAtoms));
    addKeyValue(result, "schema_registry",
                "CSR: ConversationMessage.v1 + ContextAtom.v1");
    addKeyValue(result, "dcf_storage_route",
                "structured=catalog.contexts.messages+atoms; vector=ConversationMessage.content; blob=tool/object refs");
    addKeyValue(result, "access_policy",
                "schema=agent-internal; labels=" +
                (accessLabels.empty()
                     ? std::string("AGENT_INTERNAL")
                     : joinStrings(accessLabels, ",")) +
                "; redaction=on");
    addKeyValue(result, "mentioned_entities",
                joinStrings(mentionedEntities, ","));
    addKeyValue(result, "indexed_fields",
                "ContextAtom.key,type,status,tab,tags,source,access_labels; ConversationMessage.tab,tags,mentioned_entities,access_labels");
    addKeyValue(result, "current_context",
                joinStrings(renderedAtoms, " | "));

    for (const auto& rendered : renderedAtoms) {
        addKeyValue(result, "view_atom", rendered);
    }
    if (statement.receipts) {
        for (const auto* atom : invalidated) {
            addKeyValue(result, "invalidated",
                atom->key + "=" +
                redactIfNeeded(atom->value, atom->accessLabels) +
                " @" + atom->source +
                " invalidated_by=" + atom->invalidatedBy);
        }
    }
    result.message = "spilled context view for " + context->name;
    contextResultCache_[key] = result;
    return result;
}
