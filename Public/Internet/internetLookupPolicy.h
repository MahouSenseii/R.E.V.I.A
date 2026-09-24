#pragma once

#include <string>

namespace revia::internet
{

// Decides whether a conversation may spend one bounded web-search request. This is
// deterministic so enabling internet access does not add a hidden model call to every
// social turn. The exact user text becomes the query; model output cannot choose a host.
class InternetLookupPolicy
{
public:
    [[nodiscard]] static bool ShouldLookup(
        const std::string& input,
        bool automaticLookup);
};

} // namespace revia::internet
