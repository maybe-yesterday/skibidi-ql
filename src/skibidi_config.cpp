#include "skibidi_config.h"

#include <cerrno>
#include <cstdlib>
#include <string>

namespace skibidi::config {

std::size_t readEnvSizeT(const char* name,
                         std::size_t fallback,
                         std::size_t minimum) {
#ifdef _WIN32
    char* rawBuffer = nullptr;
    std::size_t rawLength = 0;
    if (_dupenv_s(&rawBuffer, &rawLength, name) != 0 || !rawBuffer) {
        return fallback;
    }
    std::string rawValue(rawBuffer, rawLength > 0 ? rawLength - 1 : 0);
    std::free(rawBuffer);
#else
    const char* rawBuffer = std::getenv(name);
    if (!rawBuffer) return fallback;
    std::string rawValue(rawBuffer);
#endif
    const char* raw = rawValue.c_str();
    if (!*raw) return fallback;

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || !end || *end != '\0') {
        return fallback;
    }
    const auto value = static_cast<std::size_t>(parsed);
    return value < minimum ? minimum : value;
}

std::size_t defaultBufferPoolPages() {
    static const std::size_t value = readEnvSizeT(
        kBufferPoolPagesEnv, kDefaultBufferPoolPages);
    return value;
}

std::size_t defaultCompilationCacheEntries() {
    static const std::size_t value = readEnvSizeT(
        kCompilationCacheEntriesEnv, kDefaultCompilationCacheEntries);
    return value;
}

std::size_t defaultSqliteStatementCacheEntries() {
    static const std::size_t value = readEnvSizeT(
        kSqliteStatementCacheEntriesEnv,
        kDefaultSqliteStatementCacheEntries);
    return value;
}

std::size_t bloomMinimumBits() {
    static const std::size_t value = readEnvSizeT(
        kBloomMinimumBitsEnv, kDefaultBloomMinimumBits);
    return value;
}

std::size_t bloomBitsPerValue() {
    static const std::size_t value = readEnvSizeT(
        kBloomBitsPerValueEnv, kDefaultBloomBitsPerValue);
    return value;
}

std::size_t exactValueCountLimit() {
    static const std::size_t value = readEnvSizeT(
        kExactValueCountLimitEnv, kDefaultExactValueCountLimit, 0);
    return value;
}

std::size_t vectorBatchRows() {
    static const std::size_t value = readEnvSizeT(
        kVectorBatchRowsEnv, kDefaultVectorBatchRows);
    return value;
}

} // namespace skibidi::config
