#include "Planning/structuredActionParser.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <string>
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

bool PointerButtonFromString(
    const std::string& value,
    actions::ActionRequest::DesktopInput::PointerButton& outButton)
{
    using Button = actions::ActionRequest::DesktopInput::PointerButton;
    if (value.empty() || value == "left") { outButton = Button::Left; return true; }
    if (value == "right") { outButton = Button::Right; return true; }
    if (value == "middle") { outButton = Button::Middle; return true; }
    return false;
}

// Accepts a whole signed integer and nothing else, so "12abc" is a rejection rather
// than a silent 12.
bool WholeNumber(const std::string& value, int& outNumber)
{
    if (value.empty() || value.size() > 11) return false;
    std::size_t consumed = 0;
    try
    {
        outNumber = std::stoi(value, &consumed);
    }
    catch (const std::exception&)
    {
        return false;
    }
    return consumed == value.size();
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
    try
    {
        return ParseObject(nlohmann::json::parse(StripCodeFence(input)));
    }
    catch (const std::exception& error)
    {
        return Error(true, std::string("Invalid action JSON: ") + error.what());
    }
}

ParsedAction StructuredActionParser::ParseObject(const nlohmann::json& data)
{
    ParsedAction result;
    result.recognized = true;
    try
    {
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

        const bool desktopAction = actions::IsUiAutomationAction(result.request.type);
        const bool desktopControl = actions::IsDesktopControlAction(result.request.type);

        result.request.id = actions::NewActionId();
        result.request.dryRun = data.value("dry_run", false);
        result.request.requestedBy = "user";
        if (desktopControl)
        {
            result.request.application = data.value("application", "");
            result.request.windowTitle = data.value("window_title", "");
            result.request.value = data.value("value", data.value("text", std::string{}));
            result.request.input.keys = data.value("keys", "");
            // Only a launch has to name an application. Everything else may leave it out
            // to mean the desktop itself; policy decides whether that scope exists.
            if (result.request.type == actions::ActionType::LaunchApplication &&
                result.request.application.empty())
            {
                return Error(true,
                    "Starting an application requires an executable name.");
            }
            if (data.contains("x") && data.contains("y") &&
                data["x"].is_number_integer() && data["y"].is_number_integer())
            {
                result.request.input.x = data["x"].get<int>();
                result.request.input.y = data["y"].get<int>();
                result.request.input.hasPoint = true;
            }
            if (data.contains("end_x") && data.contains("end_y") &&
                data["end_x"].is_number_integer() && data["end_y"].is_number_integer())
            {
                result.request.input.endX = data["end_x"].get<int>();
                result.request.input.endY = data["end_y"].get<int>();
                result.request.input.hasEndPoint = true;
            }
            result.request.input.clickCount = data.value("clicks", 1);
            result.request.input.scrollClicks = data.value("scroll", 0);
            result.request.input.horizontalScroll = data.value("horizontal", false);
            if (!PointerButtonFromString(
                    data.value("button", std::string{}), result.request.input.button))
            {
                return Error(true, "A pointer button must be left, right, or middle.");
            }
            // A launch may name one file to open. It is a path like any other and is
            // checked against the approved roots, never treated as an argument string.
            if (result.request.type == actions::ActionType::LaunchApplication &&
                data.contains("source") && data["source"].is_string())
            {
                result.request.source =
                    actions::Utf8ToPath(data["source"].get<std::string>());
            }
            result.succeeded = true;
            return result;
        }
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
        else if (data.contains("target") && data["target"].is_string())
        {
            // Local planners also use target for the same explicit filesystem path.
            // It still passes through the normal capability and path checks.
            source = data["target"].get<std::string>();
        }
        if (source.empty())
        {
            return Error(true, "Action proposal requires a source, path, or target string.");
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
    else if (command == "/launch") type = actions::ActionType::LaunchApplication;
    else if (command == "/move-cursor") type = actions::ActionType::MoveCursor;
    else if (command == "/click") type = actions::ActionType::ClickPointer;
    else if (command == "/drag") type = actions::ActionType::DragPointer;
    else if (command == "/scroll") type = actions::ActionType::ScrollPointer;
    else if (command == "/press") type = actions::ActionType::PressKeys;
    else if (command == "/type") type = actions::ActionType::TypeText;
    else if (command == "/web") type = actions::ActionType::WebSearch;
    else return {};

    if (type == actions::ActionType::WebSearch)
    {
        if (tokens.size() != 2)
        {
            return Error(true, "Web search requires one quoted query.");
        }
        ParsedAction result;
        result.recognized = true;
        result.succeeded = true;
        result.request.id = actions::NewActionId();
        result.request.type = type;
        result.request.value = tokens[1];
        result.request.requestedBy = "user";
        return result;
    }

    if (actions::IsDesktopControlAction(type))
    {
        ParsedAction result;
        result.recognized = true;
        result.request.id = actions::NewActionId();
        result.request.type = type;
        result.request.requestedBy = "user";
        if (type == actions::ActionType::LaunchApplication)
        {
            if (tokens.size() < 2 || tokens.size() > 3)
            {
                return Error(true,
                    "Usage: /launch \"application.exe\" [\"file to open\"]");
            }
            result.request.application = tokens[1];
            if (tokens.size() == 3)
            {
                result.request.source = actions::Utf8ToPath(tokens[2]);
            }
            result.succeeded = true;
            return result;
        }

        // Two forms per command. Naming an application and window asks for the confined
        // form; starting straight at the arguments asks for the desktop itself. An
        // executable name never parses as a whole number, so the two never collide, and
        // policy -- not the parser -- decides whether the second form is permitted.
        int leadingNumber = 0;
        const bool keyboardCommand = type == actions::ActionType::PressKeys ||
            type == actions::ActionType::TypeText;
        // A chord or a run of text can look like anything, including a number, so the
        // keyboard commands are told apart by how many fields they carry instead.
        const bool screenSpace = keyboardCommand
            ? tokens.size() == 2
            : tokens.size() > 1 && WholeNumber(tokens[1], leadingNumber);
        std::size_t next = 1;
        if (!screenSpace)
        {
            if (tokens.size() < 3)
            {
                return Error(true, "Desktop operation needs either an application and "
                    "window title, or screen coordinates.");
            }
            result.request.application = tokens[1];
            result.request.windowTitle = tokens[2];
            next = 3;
        }
        const std::size_t arguments = tokens.size() - next;

        const auto usage = [&](const char* screenForm, const char* windowForm)
        {
            return Error(true, std::string("Usage: ") + screenForm + "  or  " + windowForm);
        };

        if (type == actions::ActionType::MoveCursor || type == actions::ActionType::ClickPointer)
        {
            const std::size_t maximum = type == actions::ActionType::ClickPointer ? 4U : 2U;
            if (arguments < 2 || arguments > maximum)
            {
                return type == actions::ActionType::ClickPointer
                    ? usage("/click \"x\" \"y\" [\"button\"] [\"clicks\"]",
                        "/click \"application.exe\" \"window\" \"x\" \"y\" [\"button\"] [\"clicks\"]")
                    : usage("/move-cursor \"x\" \"y\"",
                        "/move-cursor \"application.exe\" \"window\" \"x\" \"y\"");
            }
            if (!WholeNumber(tokens[next], result.request.input.x) ||
                !WholeNumber(tokens[next + 1], result.request.input.y))
            {
                return Error(true, "A pointer position needs whole-number x and y values.");
            }
            result.request.input.hasPoint = true;
            if (arguments >= 3 &&
                !PointerButtonFromString(tokens[next + 2], result.request.input.button))
            {
                return Error(true, "A pointer button must be left, right, or middle.");
            }
            if (arguments == 4 &&
                !WholeNumber(tokens[next + 3], result.request.input.clickCount))
            {
                return Error(true, "A click count must be a whole number.");
            }
        }
        else if (type == actions::ActionType::DragPointer)
        {
            if (arguments < 4 || arguments > 5)
            {
                return usage("/drag \"x\" \"y\" \"toX\" \"toY\" [\"button\"]",
                    "/drag \"application.exe\" \"window\" \"x\" \"y\" \"toX\" \"toY\" [\"button\"]");
            }
            if (!WholeNumber(tokens[next], result.request.input.x) ||
                !WholeNumber(tokens[next + 1], result.request.input.y) ||
                !WholeNumber(tokens[next + 2], result.request.input.endX) ||
                !WholeNumber(tokens[next + 3], result.request.input.endY))
            {
                return Error(true, "A drag needs whole-number start and end coordinates.");
            }
            result.request.input.hasPoint = true;
            result.request.input.hasEndPoint = true;
            if (arguments == 5 &&
                !PointerButtonFromString(tokens[next + 4], result.request.input.button))
            {
                return Error(true, "A pointer button must be left, right, or middle.");
            }
        }
        else if (type == actions::ActionType::ScrollPointer)
        {
            if (arguments != 1 ||
                !WholeNumber(tokens[next], result.request.input.scrollClicks))
            {
                return usage("/scroll \"detents\"",
                    "/scroll \"application.exe\" \"window\" \"detents\"");
            }
        }
        else if (type == actions::ActionType::PressKeys)
        {
            if (arguments != 1)
            {
                return usage("/press \"ctrl+s\"",
                    "/press \"application.exe\" \"window\" \"ctrl+s\"");
            }
            result.request.input.keys = tokens[next];
        }
        else
        {
            if (arguments != 1)
            {
                return usage("/type \"text\"",
                    "/type \"application.exe\" \"window\" \"text\"");
            }
            result.request.value = tokens[next];
        }
        result.succeeded = true;
        return result;
    }

    const bool desktopAction = actions::IsUiAutomationAction(type);
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
