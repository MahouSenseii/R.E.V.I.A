#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace revia::runtime
{

// How many warnings and errors a bounded log currently holds.
//
// The counts describe the retained model, not the session. Incrementing on append and
// never decrementing on trim is what let a shell report errors that had already been
// dropped from the log they claimed to be counting. Recomputing is O(retained), which
// at a two-thousand-entry cap is nothing, and it cannot drift the way paired
// increments and decrements can.
struct RetainedSeverityCounts
{
    int warnings = 0;
    int errors = 0;
};

// Severity as a small integer so this stays independent of any UI enum: 1 warning,
// 2 error, anything else informational.
[[nodiscard]] RetainedSeverityCounts CountRetainedSeverities(
    const std::vector<int>& severities);

// The least recently used key that may be evicted, or empty when there is nothing to
// evict.
//
// `keepKey` is the context currently being processed and is never returned, however old
// it looks. A map entry with no recorded use is treated as the oldest, so a key that
// somehow escaped bookkeeping is evicted before one that is genuinely in use.
[[nodiscard]] std::string SelectEvictableKey(
    const std::vector<std::string>& keys,
    const std::unordered_map<std::string, std::uint64_t>& lastUsed,
    const std::string& keepKey);

} // namespace revia::runtime
