#pragma once

#include <string>

namespace revia::planning
{

// Conservative local intent detection routes spoken/plain requests to the typed action path.
// A match grants no authority; normal validation, policy, confirmation, rate, and audit apply.
struct OperateIntent
{
    bool matched = false;
    // The verb that matched, for the audit reason and the diagnostic line. Never the
    // rest of the sentence: this is routing evidence, not content.
    std::string verb;
};

[[nodiscard]] OperateIntent DetectOperateRequest(const std::string& input);
// Routing evidence for a user-requested message, never a permission from model output.
[[nodiscard]] bool RequestsExternalMessage(const std::string& input);

} // namespace revia::planning
