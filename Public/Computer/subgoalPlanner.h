#pragma once

#include "Computer/computerSubgoal.h"
#include "Computer/computerTypes.h"

#include <string>

namespace revia::computer
{

// Turning Main's answer into a proposal, and nothing more than a proposal.
//
// This is the one place free-form model output becomes a typed subgoal, which makes it
// the one place the contract can be broken. So it is deliberately narrow: it parses,
// it refuses, and it never fills anything in. A field the model did not supply stays
// empty and validation refuses the combination; a field the model supplied that this
// build does not understand is dropped rather than guessed at.
//
// What it produces is still only a claim. `ValidateSubgoal` is what attaches origin,
// scope and budget and decides whether the claim is inside the task the user actually
// authorized -- and nothing here can produce a subgoal that skips it, because the
// authority stamp is not reachable from this file.

// What Main is asked, in the two halves a chat model actually takes.
//
// The instruction is the system prompt and the situation is the message. That split is
// not cosmetic: routing the whole thing through the next-step planner as one user
// message meant the step grammar decided the answer's shape, the subgoal instructions
// were ignored, and the assisted path could never engage on a live backend -- while
// every scripted test passed, because a scripted test supplies the answer.
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
[[nodiscard]] SubgoalRequest FormatSubgoalRequest(
    const std::string& task,
    const ComputerTaskContext& context,
    const PayloadReference& availablePayload);

struct ParsedSubgoal
{
    bool succeeded = false;
    // Why not, for the activity feed and the record. Never parsed, never a label.
    std::string error;
    ComputerSubgoal proposed;
};

// Parse one constrained answer.
//
// Bounded before it is read: an answer larger than a subgoal can be is refused without
// being parsed, because the cheapest moment to refuse malformed output is before it is
// interpreted.
[[nodiscard]] ParsedSubgoal ParseSubgoal(const std::string& answer);

// The largest answer that could still be a subgoal. Comfortably above a well-formed one
// and far below a model that has started writing prose.
inline constexpr std::size_t MaximumSubgoalAnswerBytes = 4096;

} // namespace revia::computer
