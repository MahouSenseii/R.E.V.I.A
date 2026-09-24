#include "Intelligence/reflexRouter.h"

#include <algorithm>
#include <cctype>

namespace revia::intelligence
{
namespace
{
std::string Normalize(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n.!?");
    return value.substr(first, last - first + 1);
}
}

ReflexResult ReflexRouter::Route(
    const std::string& input,
    const ReflexContext& context) const
{
    const std::string text = Normalize(input);
    ReflexResult result;

    if (text == "stop" || text == "cancel" || text == "wait" || text == "pause" ||
        text == "quiet" || text == "shut up" || text == "never mind" ||
        text == "nevermind")
    {
        result.matched = true;
        result.requestsCancellation = true;
        result.response = context.interruptedGeneration ? "Stopped." : "Okay.";
        result.reason = "Immediate cancellation was handled without model inference.";
        return result;
    }

    if (text != "revia") return result;

    result.matched = true;
    result.reason = "Direct attention call was handled without model inference.";
    if (context.repeatedCalls > 0)
    {
        result.response = "I heard you the first time.";
    }
    else if (context.busy)
    {
        result.response = context.previousResponse == "Mm?" ? "Hold on—what?" : "Mm?";
    }
    else if (context.affect.state == runtime::AffectState::Angry ||
             context.affect.state == runtime::AffectState::Frustrated ||
             context.affect.state == runtime::AffectState::Sulky)
    {
        result.response = "What?";
    }
    else if (context.affect.state == runtime::AffectState::Playful ||
             context.affect.state == runtime::AffectState::Excited)
    {
        result.response = context.previousResponse == "Yeah?" ? "Hm?" : "Yeah?";
    }
    else
    {
        result.response = context.previousResponse == "I'm here." ? "Yeah?" : "I'm here.";
    }
    return result;
}

} // namespace revia::intelligence
