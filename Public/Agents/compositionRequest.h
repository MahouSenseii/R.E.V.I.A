#pragma once

#include <string_view>

namespace revia::agents
{
// Bounded recognition of self-contained writing requests; all other inputs retain ordinary tool eligibility.
[[nodiscard]] bool IsSelfContainedCompositionRequest(std::string_view userAuthoredInput);
}
