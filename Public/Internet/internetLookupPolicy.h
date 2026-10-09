#pragma once

#include <string>

namespace revia::internet
{

struct LookupRequest
{
    std::string text;
    std::string reason;
    bool explicitRequest = false;
    bool prohibited = false;
};

// Selects a live authored request clause. Quoted data and later withdrawals cannot
// supply authority, and unrelated sentences never become part of its query.
[[nodiscard]] LookupRequest SelectLookupRequest(const std::string& input);

// Decides whether a conversation may spend one bounded web-search request. This is
// deterministic so enabling internet access does not add a hidden model call to every
// social turn. Only the admitted subject becomes the query; model output cannot choose a host.
class InternetLookupPolicy
{
public:
    [[nodiscard]] static bool ShouldLookup(const std::string& input, bool automaticLookup);
};

} // namespace revia::internet
