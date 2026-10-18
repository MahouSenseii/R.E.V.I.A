#pragma once

#include "Actions/actionTypes.h"
#include "Agents/toolRegistry.h"
#include "Planning/reminders.h"

#include <functional>
#include <string>

namespace revia::agents
{

// The action boundary a chat tool reaches, supplied by the session from its own
// ActionRuntime and confirmation handler. A chat tool never holds the runtime itself, so
// it cannot reach an executor any other way.
struct ChatActionPort
{
    std::function<actions::PolicyDecision(const actions::ActionRequest&)> evaluate;
    // Runs the request through ActionRuntime with the turn's stamp and task contract.
    std::function<actions::ActionOutcome(const actions::ActionRequest&, bool confirmationGranted)> execute;
    // The session's confirmation handler. Empty when none is installed, and then nothing
    // that needs approval runs.
    std::function<actions::ConfirmationChoice(const actions::ActionRequest&, const actions::PolicyDecision&)> confirm;
};

// One typed action for a chat tool, through the same steps an interactive action takes:
// policy evaluation, confirmation when policy asks for it, then ActionRuntime with its
// audit. An action that needs approval with no handler to ask, or that the person
// declines, is never sent to the runtime; the tool returns an error saying so.
[[nodiscard]] ToolExecution RunChatAction(const ChatActionPort& port, actions::ActionRequest request, const ToolCallContext& context);

// A read_file or list_dir request for `path`, typed exactly as the structured parser
// would type it. The path is resolved and checked against approved roots by policy.
[[nodiscard]] actions::ActionRequest ChatFileRequest(actions::ActionType type, const std::string& path);

// Sets a reminder from a tool call through the same deterministic parser and book that
// "remind me in 10 minutes to stretch" uses: the model supplies words, never a time.
[[nodiscard]] ToolExecution AddChatReminder(
    planning::ReminderBook& book, const std::string& text, const std::string& when, planning::WallClock::time_point now);

} // namespace revia::agents
