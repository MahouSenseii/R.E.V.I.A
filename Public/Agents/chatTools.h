#pragma once

#include "Actions/actionTypes.h"
#include "Agents/toolRegistry.h"
#include "LLM/toolCall.h"
#include "Planning/reminders.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// The same steps, returning the runtime's whole outcome for a tool that reads more than its
// text, such as a guest receipt. When nothing was sent to the runtime, it returns nothing
// and `refusal` says why.
[[nodiscard]] std::optional<actions::ActionOutcome> SubmitChatAction(
    const ChatActionPort& port, actions::ActionRequest request, const ToolCallContext& context, std::string& refusal);

// A read_file or list_dir request for `path`, typed exactly as the structured parser
// would type it. The path is resolved and checked against approved roots by policy.
[[nodiscard]] actions::ActionRequest ChatFileRequest(actions::ActionType type, const std::string& path);

// The absolute Windows paths written in `text`, in order: "C:\notes\todo.md", "D:/data".
// Inside quotes a path may hold spaces; outside them it ends at the first one.
[[nodiscard]] std::vector<std::string> AuthoredPaths(std::string_view text);

// The reads the person's own words call for, made before the model's first step: one call
// for each file or folder they named that exists inside an approved root, list_dir for a
// folder and read_file for a file, two at most. A full path is read as written. A file
// named on its own ("todo.md in that folder") is looked for in the folders they pointed
// at in `text` and in `earlier`, their recent messages newest first, and then in the
// approved roots.
//
// Left to choose, a live 9B asked to list a folder it could read answered without looking
// in about one try in eight and described files that were not there, and asked what
// notes.txt said, replied that it had looked and made the contents up. This only says
// what to read. Each call still goes through the tool loop's validation and the action
// policy, as a call the model makes does.
[[nodiscard]] std::vector<llm::ToolCall> OpeningFileCalls(
    std::string_view text, const std::vector<std::filesystem::path>& approvedRoots, const std::vector<std::string>& earlier = {});

// The hand-off the person asked for with "use your agents", for a reply that ends without
// making it: their own request is the task. Asked that way, a live 9B called delegate_task
// in 8 tries of 13; in the others it said it would, asked a question back, or answered
// itself. The call is still delegate_task's to accept or refuse.
[[nodiscard]] llm::ToolCall RequestedDelegationCall(std::string_view request);

// Sets a reminder from a tool call through the same deterministic parser and book that
// "remind me in 10 minutes to stretch" uses: the model supplies words, never a time.
[[nodiscard]] ToolExecution AddChatReminder(
    planning::ReminderBook& book, const std::string& text, const std::string& when, planning::WallClock::time_point now);

} // namespace revia::agents
