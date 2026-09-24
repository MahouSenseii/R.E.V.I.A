#pragma once

#include "Intelligence/intelligenceTypes.h"

#include <string>

namespace revia::intelligence
{

// A cheap, deterministic pre-generation decision. It deliberately uses semantic
// signals instead of message length: "why is this deadlocking?" is short and hard,
// while a request for a long silly story is not automatically an Expert problem.
class IntelligenceRouter
{
public:
    [[nodiscard]] IntelligenceDecision Route(
        const std::string& input,
        const RoutingContext& context = {}) const;
};

} // namespace revia::intelligence
