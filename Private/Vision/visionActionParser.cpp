#include "Vision/visionActionParser.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace revia::vision
{

namespace
{
std::string Trim(std::string value)
{
    const auto content = [](const unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), content));
    value.erase(std::find_if(value.rbegin(), value.rend(), content).base(), value.end());
    return value;
}

// What vision is allowed to propose.
//
// It was invoke_control and set_control_text, which assumed every interface exposes a UI
// Automation tree worth matching against. A game, an Unreal or UMG surface, a canvas and
// a bare Electron tree do not, and under that restriction vision could say nothing at all
// about them -- not even "press escape".
//
// Widening the list widens nothing else: each of these is an existing ActionType with an
// existing executor, and each still goes proposal -> typed action -> capability policy ->
// confirmation -> executor -> audit. What a proposal may aim at is decided by policy from
// the evidence behind it, not by this function.
bool IsVisionProposable(const actions::ActionType action)
{
    switch (action)
    {
        case actions::ActionType::InvokeControl:
        case actions::ActionType::SetControlText:
        case actions::ActionType::MoveCursor:
        case actions::ActionType::ClickPointer:
        case actions::ActionType::DragPointer:
        case actions::ActionType::ScrollPointer:
        case actions::ActionType::PressKeys:
        case actions::ActionType::TypeText:
            return true;
        default:
            return false;
    }
}

bool ReadRegion(
    const nlohmann::json& data,
    const char* field,
    ScreenRegion& region,
    std::string& reason)
{
    if (!data.contains(field) || !data[field].is_object())
    {
        reason = std::string("The vision model did not return a ") + field + ".";
        return false;
    }
    const nlohmann::json& source = data[field];
    region.left = source.value("left", -1);
    region.top = source.value("top", -1);
    region.right = source.value("right", -1);
    region.bottom = source.value("bottom", -1);
    if (region.left < 0 || region.top < 0 || !region.IsValid())
    {
        reason = std::string("The vision model returned an invalid ") + field + ".";
        return false;
    }
    return true;
}

std::string StripCodeFence(std::string value)
{
    value = Trim(std::move(value));
    if (!value.starts_with("```"))
    {
        return value;
    }
    const std::size_t firstLine = value.find('\n');
    const std::size_t finalFence = value.rfind("```");
    return firstLine == std::string::npos || finalFence <= firstLine
        ? value
        : Trim(value.substr(firstLine + 1, finalFence - firstLine - 1));
}
}

VisionActionParseResult VisionActionParser::Parse(const std::string& response) const
{
    VisionActionParseResult result;
    try
    {
        const nlohmann::json data = nlohmann::json::parse(StripCodeFence(response));
        if (!data.is_object())
        {
            result.reason = "The vision target response was not a JSON object.";
            return result;
        }

        result.intent.action = actions::ActionTypeFromString(data.value("action", ""));
        if (!IsVisionProposable(result.intent.action))
        {
            result.reason = data.value(
                "reason",
                "Vision may propose pointer, keyboard, and UI Automation actions only.");
            return result;
        }
        result.intent.targetName = Trim(data.value("target_name", ""));
        result.intent.targetDescription = Trim(data.value("target_description", ""));
        result.intent.value = data.value("value", "");
        result.intent.keys = Trim(data.value("keys", ""));
        result.intent.scrollClicks = data.value("scroll", 0);
        result.intent.horizontalScroll = data.value("horizontal", false);
        result.intent.clickCount = data.value("clicks", 1);
        result.intent.modelConfidence = data.value("confidence", -1.0);
        if (result.intent.modelConfidence < 0.0 || result.intent.modelConfidence > 1.0)
        {
            result.reason = "Vision target confidence must be between zero and one.";
            return result;
        }

        // The UI Automation family addresses a control by name, which is what makes it
        // the strongest route. Without a name there is nothing for the resolver to match.
        const bool uiaFamily = result.intent.action == actions::ActionType::InvokeControl ||
            result.intent.action == actions::ActionType::SetControlText;
        if (uiaFamily && result.intent.targetName.empty())
        {
            result.reason = "The vision model did not name the target control.";
            return result;
        }
        if (result.intent.action == actions::ActionType::SetControlText &&
            result.intent.value.empty())
        {
            result.reason = "A set-text screen action requires a non-empty value.";
            return result;
        }
        if (result.intent.action == actions::ActionType::TypeText &&
            result.intent.value.empty())
        {
            result.reason = "Typing requires non-empty text.";
            return result;
        }
        if (result.intent.action == actions::ActionType::PressKeys &&
            result.intent.keys.empty())
        {
            result.reason = "A key press requires a chord such as \"ctrl+l\".";
            return result;
        }
        // A pointer target is described, not named: a rendered game button has no
        // accessible name to give, and demanding one would exclude every interface this
        // route exists for.
        if (!uiaFamily && result.intent.NeedsRegion() &&
            result.intent.targetDescription.empty())
        {
            result.reason = "A visual pointer target needs a description of what it is.";
            return result;
        }

        // A coordinate the model wrote is not a visual target, and accepting one here
        // would make this parser a way around the raw-coordinate permission rather than
        // an alternative to it. The region is the proposal; the point is derived from it
        // at execution, against the window it was seen in.
        if (data.contains("x") || data.contains("y"))
        {
            result.reason = "Vision proposes the region a target occupies, never a point.";
            return result;
        }

        if (!result.intent.NeedsRegion())
        {
            // Keyboard goes wherever focus is. It needs keyboard permission like any
            // other keystroke and no visual authority at all.
            result.succeeded = true;
            return result;
        }
        if (!ReadRegion(data, "region", result.intent.region, result.reason))
        {
            return result;
        }
        if (result.intent.action == actions::ActionType::DragPointer &&
            !ReadRegion(data, "end_region", result.intent.endRegion, result.reason))
        {
            return result;
        }

        result.succeeded = true;
        return result;
    }
    catch (const std::exception& error)
    {
        result.reason = std::string("Invalid vision target JSON: ") + error.what();
        return result;
    }
}

} // namespace revia::vision
