#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace revia::runtime
{

// Returns the least recently used key except keepKey, or empty if none is eligible.
// Unrecorded keys are oldest; lexical order breaks ties.
[[nodiscard]] std::string SelectEvictableKey(const std::vector<std::string>& keys,
    const std::unordered_map<std::string, std::uint64_t>& lastUsed, const std::string& keepKey);

} // namespace revia::runtime
