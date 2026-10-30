#pragma once

#include <string_view>

namespace revia::agents
{
// Sampling policy only; source availability and permission remain with the caller.
[[nodiscard]] bool IsSourceExplanationRequest(std::string_view input, std::string_view previousAssistant);
}
