#include "Runtime/publicContextCache.h"

#include <limits>

namespace revia::runtime
{

std::string SelectEvictableKey(const std::vector<std::string>& keys,
    const std::unordered_map<std::string, std::uint64_t>& lastUsed, const std::string& keepKey)
{
    std::string oldest;
    std::uint64_t oldestUse = std::numeric_limits<std::uint64_t>::max();
    for (const std::string& key : keys)
    {
        if (key == keepKey) continue;
        const auto found = lastUsed.find(key);
        const std::uint64_t when = found == lastUsed.end() ? 0 : found->second;
        if (when < oldestUse || (when == oldestUse && key < oldest))
        {
            oldestUse = when;
            oldest = key;
        }
    }
    return oldest;
}

} // namespace revia::runtime
