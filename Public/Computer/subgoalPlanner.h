#pragma once

#include "Computer/computerSubgoal.h"
#include "Computer/computerTypes.h"

#include <string>

namespace revia::computer
{

// Parses model claims without inventing missing fields or granting authority.
// ValidateSubgoal attaches origin, scope, budget, and validation stamp.

// Instruction is the system prompt; situation is the user message.
// Keep them separate from next-step planning grammar.
struct SubgoalRequest
{
    std::string instruction;
    std::string situation;
    // The output shape, constrained to the intents this build implements and to names
    // actually on screen. A name the model invents is one nothing can match.
    std::string schema;
};

// Build the request. It has to name the payload references that exist right now: a
// model cannot reference content it was not told about, and telling it about content by
// *value* would defeat the whole arrangement.
[[nodiscard]] SubgoalRequest FormatSubgoalRequest(const std::string& task,
    const ComputerTaskContext& context, const PayloadReference& availablePayload);

struct ParsedSubgoal
{
    bool succeeded = false;
    // Why not, for the activity feed and the record. Never parsed, never a label.
    std::string error;
    ComputerSubgoal proposed;
};

// Rejects oversized output before parsing.
[[nodiscard]] ParsedSubgoal ParseSubgoal(const std::string& answer);

// The largest answer that could still be a subgoal. Comfortably above a well-formed one
// and far below a model that has started writing prose.
inline constexpr std::size_t MaximumSubgoalAnswerBytes = 4096;

} // namespace revia::computer
