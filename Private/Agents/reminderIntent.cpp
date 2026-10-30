#include "Agents/reminderIntent.h"

#include "Agents/compositionRequest.h"
#include "Core/utf8.h"
#include "Planning/reminders.h"

#include <algorithm>
#include <cctype>

namespace revia::agents
{
namespace
{
bool WakeRequest(const std::string& input)
{
    if (!input.starts_with("wake me "))
        return false;
    auto when = input.substr(8);
    if (when.starts_with("up "))
        when.erase(0, 3);
    const auto end = when.find_last_not_of(" .!?");
    if (end == std::string::npos)
        return false;
    when.resize(end + 1);
    planning::ReminderRequest request;
    std::string error;
    return planning::ParseReminderRequest("remind me " + when + " to wake up", planning::WallClock::now(), request, error) ==
               planning::ReminderParse::Parsed &&
           request.text == "wake up";
}
}

bool RequestsReminderEffect(const std::string& input)
{
    if (input.empty() || input.size() > 4000 || input.find('\0') != std::string::npos || !utf8::IsValid(input))
        return false;
    planning::ReminderRequest request;
    std::string error;
    // Invalid is still an explicit request needing clarification. The parser is
    // anchored to the person's request; quoted/source text does not become a cue.
    if (planning::ParseReminderRequest(input, planning::WallClock::now(), request, error) != planning::ReminderParse::NotAReminder)
        return true;
    auto text = input;
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (auto at = text.find(" and "); at != std::string::npos; at = text.find(" and ", at + 5))
        if (IsSelfContainedCompositionRequest(text.substr(0, at)) && WakeRequest(text.substr(at + 5)))
            return true;
    return false;
}
}
