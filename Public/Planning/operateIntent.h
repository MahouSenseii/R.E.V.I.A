#pragma once

#include <string>

namespace revia::planning
{

// Whether an ordinary conversational turn is asking Revia to operate the machine.
//
// It exists so that "open Microsoft Edge and pull up Facebook" reaches the same typed
// action path "/operate open Microsoft Edge..." reaches. A slash prefix cannot be spoken,
// and speech is where this is going, so the prefix cannot be the only way in.
//
// This grants nothing. A match only decides which path the sentence travels; the request
// it produces is still parsed, still checked against CapabilityPolicy, still confirmed,
// still rate limited and still audited exactly as a slash command's would be. A false
// positive therefore costs an approval prompt the user declines, never an action they
// did not sanction -- which is why a deterministic gate is acceptable here rather than
// requiring a model to classify intent before anything may run.
//
// Deliberately conservative and deliberately pure: no model, no clock, no state. The
// cost of missing a request is that the user rephrases. The cost of inventing one is
// that Revia starts grabbing the mouse in the middle of a conversation.
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
