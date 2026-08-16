#include "Planning/structuredActionParser.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <vector>

namespace revia::planning
{

namespace
{

std::string Trim(std::string value)
{
    const auto notWhitespace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notWhitespace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notWhitespace).base(), value.end());
    return value;
}

ParsedAction Error(bool recognized, std::string message)
{
    ParsedAction result;
    result.recognized = recognized;
    result.error = std::move(message);
    return result;
}

} // namespace

ParsedAction StructuredActionParser::ParseJson(const std::string& input) const
{
    ParsedAction result;
    result.recognized = true;
    try
    {
        const auto data = nlohmann::json::parse(StripCodeFence(input));
        if (!data.is_object())
        {
            return Error(true, "Action proposal must be a JSON object.");
        }

        const std::string actionName = data.value("action", "");
        result.request.type = actions::ActionTypeFromString(actionName);
        if (result.request.type == actions::ActionType::Unknown)
        {
            const std::string reason = data.value("reason", "unknown or missing action");
            return Error(true, "Planner did not return an executable action: " + reason);
        }

        const bool desktopAction =
            result.request.type == actions::ActionType::InspectWindow ||
            result.request.type == actions::ActionType::FocusWindow ||
            result.request.type == actions::ActionType::SetControlText ||
            result.request.type == actions::ActionType::InvokeControl;

        result.request.id = actions::NewActionId();
        result.request.dryRun = data.value("dry_run", false);
        result.request.requestedBy = "user";
        if (desktopAction)
        {
            result.request.application = data.value("application", "");
            result.request.windowTitle = data.value("window_title", "");
            result.request.control = data.value("control", "");
            result.request.value = data.value("value", "");
            if (result.request.application.empty())
            {
                return Error(true, "Desktop action proposal requires an application executable name.");
            }
            result.succeeded = true;
            return result;
        }

        std::string source;
        if (data.contains("source") && data["source"].is_string())
        {
            source = data["source"].get<std::string>();
        }
        else if (data.contains("path") && data["path"].is_string())
        {
            source = data["path"].get<std::string>();
        }
        if (source.empty())
        {
            return Error(true, "Action proposal requires a source or path string.");
        }

        result.request.source = actions::Utf8ToPath(source);
        if (data.contains("destination") && data["destination"].is_string())
        {
            result.request.destination = actions::Utf8ToPath(
                data["destination"].get<std::string>());
        }
        result.succeeded = true;
        return result;
    }
    catch (const std::exception& error)
    {
        return Error(true, std::string("Invalid action JSON: ") + error.what());
    }
}

ParsedAction StructuredActionParser::ParseCommand(const std::string& input) const
{
    const std::string trimmed = Trim(input);
    if (trimmed.rfind("/action ", 0) == 0)
    {
        return ParseJson(trimmed.substr(8));
    }

    const auto tokens = Tokenize(trimmed);
    if (tokens.empty())
    {
        return {};
    }

    const std::string& command = tokens[0];
    actions::ActionType type = actions::ActionType::Unknown;
    if (command == "/list") type = actions::ActionType::ListDirectory;
    else if (command == "/read") type = actions::ActionType::ReadTextFile;
    else if (command == "/mkdir") type = actions::ActionType::CreateDirectory;
    else if (command == "/copy") type = actions::ActionType::CopyFile;
    else if (command == "/move") type = actions::ActionType::MoveFile;
    else if (command == "/rename") type = actions::ActionType::RenamePath;
    else if (command == "/trash") type = actions::ActionType::MoveToRecycleBin;
    else if (command == "/inspect-window") type = actions::ActionType::InspectWindow;
    else if (command == "/focus-window") type = actions::ActionType::FocusWindow;
    else if (command == "/set-text") type = actions::ActionType::SetControlText;
    else if (command == "/invoke-control") type = actions::ActionType::InvokeControl;
    else return {};

    const bool desktopAction = type == actions::ActionType::InspectWindow ||
        type == actions::ActionType::FocusWindow || type == actions::ActionType::SetControlText ||
        type == actions::ActionType::InvokeControl;
    if (desktopAction)
    {
        const std::size_t expected = type == actions::ActionType::SetControlText ? 5U :
            type == actions::ActionType::InvokeControl ? 4U : 3U;
        if (tokens.size() != expected)
        {
            return Error(true,
                "Desktop command requires quoted application, window title, control, and value fields for its action type.");
        }
        ParsedAction result;
        result.recognized = true;
        result.succeeded = true;
        result.request.id = actions::NewActionId();
        result.request.type = type;
        result.request.application = tokens[1];
        result.request.windowTitle = tokens[2];
        if (type == actions::ActionType::SetControlText || type == actions::ActionType::InvokeControl)
        {
            result.request.control = tokens[3];
        }
        if (type == actions::ActionType::SetControlText)
        {
            result.request.value = tokens[4];
        }
        result.request.requestedBy = "user";
        return result;
    }

    const bool requiresDestination = type == actions::ActionType::CopyFile ||
        type == actions::ActionType::MoveFile || type == actions::ActionType::RenamePath;
    const std::size_t expected = requiresDestination ? 3U : 2U;
    if (tokens.size() != expected)
    {
        return Error(true, requiresDestination
            ? "Command requires quoted source and destination paths."
            : "Command requires one quoted path.");
    }

    ParsedAction result;
    result.recognized = true;
    result.succeeded = true;
    result.request.id = actions::NewActionId();
    result.request.type = type;
    result.request.source = actions::Utf8ToPath(tokens[1]);
    if (requiresDestination)
    {
        result.request.destination = actions::Utf8ToPath(tokens[2]);
    }
    result.request.requestedBy = "user";
    return result;
}

std::vector<std::string> StructuredActionParser::Tokenize(const std::string& input)
{
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;

    for (char value : input)
    {
        if (value == '"')
        {
            quoted = !quoted;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(value)) && !quoted)
        {
            if (!current.empty())
            {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(value);
    }
    if (quoted)
    {
        return {};
    }
    if (!current.empty())
    {
        tokens.push_back(current);
    }
    return tokens;
}

std::string StructuredActionParser::StripCodeFence(const std::string& input)
{
    std::string value = Trim(input);
    if (value.rfind("```", 0) != 0)
    {
        return value;
    }

    const std::size_t firstLine = value.find('\n');
    const std::size_t finalFence = value.rfind("```");
    if (firstLine == std::string::npos || finalFence <= firstLine)
    {
        return value;
    }
    return Trim(value.substr(firstLine + 1, finalFence - firstLine - 1));
}

} // namespace revia::planning
