#include "Agents/chatTools.h"

#include <utility>

namespace revia::agents
{

ToolExecution RunChatAction(const ChatActionPort& port, actions::ActionRequest request, const ToolCallContext& context)
{
    if (!port.evaluate || !port.execute)
        return ToolExecution::Fail("The action runtime is not available.");
    if (!context.Admitted())
        return ToolExecution::Fail("The turn ended before this tool ran.");
    request.requestedBy = "conversation_tool";
    // Rechecked at the executor's own effect boundary, after any approval wait.
    request.beforeEffect = [context, earlier = std::move(request.beforeEffect)](const std::string& target)
    {
        if (!context.Admitted())
            return std::string("The conversation turn that asked for this is no longer current.");
        return earlier ? earlier(target) : std::string{};
    };

    const actions::PolicyDecision decision = port.evaluate(request);
    bool confirmed = false;
    if (decision.verdict == actions::PolicyVerdict::RequiresConfirmation)
    {
        if (!port.confirm)
        {
            return ToolExecution::Fail("This needs your approval, and no approval prompt is available in this conversation, so nothing "
                                       "was done. Policy said: " +
                                       decision.reason);
        }
        if (!actions::Granted(port.confirm(request, decision)))
            return ToolExecution::Fail("The owner declined this action, so nothing was done.");
        if (!context.Admitted())
            return ToolExecution::Fail("The turn ended while approval was being asked for.");
        confirmed = true;
    }
    // A blocked request still goes to the runtime: it refuses before any executor and
    // records the refusal in the action audit, as every rejected action must be.
    const actions::ActionOutcome outcome = port.execute(request, confirmed);
    if (!outcome.Succeeded())
        return ToolExecution::Fail(outcome.Message().empty() ? decision.reason : outcome.Message());
    std::string content;
    for (const auto& entry : outcome.result.entries)
        content += entry + "\n";
    content += outcome.result.content;
    if (content.empty())
        content = outcome.result.message;
    return ToolExecution::Ok(std::move(content));
}

actions::ActionRequest ChatFileRequest(const actions::ActionType type, const std::string& path)
{
    actions::ActionRequest request;
    request.id = actions::NewActionId();
    request.type = type;
    request.source = actions::Utf8ToPath(path);
    return request;
}

ToolExecution AddChatReminder(
    planning::ReminderBook& book, const std::string& text, const std::string& when, const planning::WallClock::time_point now)
{
    planning::ReminderRequest request;
    std::string error;
    switch (planning::ParseReminderRequest("remind me " + when + " to " + text, now, request, error))
    {
    case planning::ReminderParse::NotAReminder:
        return ToolExecution::Fail("That time could not be read. Use words such as 'in 10 minutes', 'at 3pm' or 'tomorrow at 9am'.");
    case planning::ReminderParse::Invalid:
        return ToolExecution::Fail(error);
    case planning::ReminderParse::Parsed:
        break;
    }
    std::string saveError;
    const auto added = book.Add(request, saveError);
    if (!added)
        return ToolExecution::Fail(saveError.empty() ? "The reminder list is full." : saveError);
    std::string confirmation = "Reminder set: " + planning::Label(request) + ", due " + planning::DescribeWhen(request.due, now) + " (in " +
                               planning::DescribeSpan(request.due - now) + ").";
    if (!saveError.empty())
        confirmation += " It could not be saved to disk, so it lasts only until Revia closes: " + saveError;
    return ToolExecution::Ok(std::move(confirmation));
}

} // namespace revia::agents
