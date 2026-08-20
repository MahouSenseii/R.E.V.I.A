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
        if (result.intent.action != actions::ActionType::InvokeControl &&
            result.intent.action != actions::ActionType::SetControlText)
        {
            result.reason = data.value(
                "reason",
                "Vision may resolve only invoke_control or set_control_text actions.");
            return result;
        }
        result.intent.targetName = Trim(data.value("target_name", ""));
        result.intent.targetDescription = Trim(data.value("target_description", ""));
        result.intent.value = data.value("value", "");
        result.intent.modelConfidence = data.value("confidence", -1.0);
        if (result.intent.targetName.empty())
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
        if (result.intent.modelConfidence < 0.0 || result.intent.modelConfidence > 1.0)
        {
            result.reason = "Vision target confidence must be between zero and one.";
            return result;
        }
        if (!data.contains("region") || !data["region"].is_object())
        {
            result.reason = "The vision model did not return a target region.";
            return result;
        }
        const nlohmann::json& region = data["region"];
        result.intent.region.left = region.value("left", -1);
        result.intent.region.top = region.value("top", -1);
        result.intent.region.right = region.value("right", -1);
        result.intent.region.bottom = region.value("bottom", -1);
        if (result.intent.region.left < 0 || result.intent.region.top < 0 ||
            !result.intent.region.IsValid())
        {
            result.reason = "The vision model returned an invalid target region.";
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
