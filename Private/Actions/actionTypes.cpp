#include "Actions/actionTypes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <sstream>

namespace revia::actions
{

namespace
{

std::string NormalizeName(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    {
        if (c == '-' || c == ' ')
        {
            return '_';
        }
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

std::string ToString(ActionType value)
{
    switch (value)
    {
        case ActionType::ListDirectory: return "list_directory";
        case ActionType::ReadTextFile: return "read_text_file";
        case ActionType::CreateDirectory: return "create_directory";
        case ActionType::CopyFile: return "copy_file";
        case ActionType::MoveFile: return "move_file";
        case ActionType::RenamePath: return "rename_path";
        case ActionType::MoveToRecycleBin: return "move_to_recycle_bin";
        case ActionType::InspectWindow: return "inspect_window";
        case ActionType::FocusWindow: return "focus_window";
        case ActionType::SetControlText: return "set_control_text";
        case ActionType::InvokeControl: return "invoke_control";
        case ActionType::WebSearch: return "web_search";
        case ActionType::Unknown:
        default: return "unknown";
    }
}

std::string ToString(RiskLevel value)
{
    switch (value)
    {
        case RiskLevel::ReadOnly: return "read_only";
        case RiskLevel::ReversibleWrite: return "reversible_write";
        case RiskLevel::Destructive: return "destructive";
        default: return "destructive";
    }
}

std::string ToString(PolicyVerdict value)
{
    switch (value)
    {
        case PolicyVerdict::Allowed: return "allowed";
        case PolicyVerdict::RequiresConfirmation: return "requires_confirmation";
        case PolicyVerdict::Blocked: return "blocked";
        default: return "blocked";
    }
}

std::string ToString(ExecutionMode value)
{
    switch (value)
    {
        case ExecutionMode::Disabled: return "disabled";
        case ExecutionMode::Supervised: return "supervised";
        case ExecutionMode::ApprovedScope: return "approved_scope";
        default: return "disabled";
    }
}

ActionType ActionTypeFromString(const std::string& value)
{
    const std::string normalized = NormalizeName(value);
    if (normalized == "list" || normalized == "list_directory") return ActionType::ListDirectory;
    if (normalized == "read" || normalized == "read_text_file") return ActionType::ReadTextFile;
    if (normalized == "mkdir" || normalized == "create_directory") return ActionType::CreateDirectory;
    if (normalized == "copy" || normalized == "copy_file") return ActionType::CopyFile;
    if (normalized == "move" || normalized == "move_file") return ActionType::MoveFile;
    if (normalized == "rename" || normalized == "rename_path") return ActionType::RenamePath;
    if (normalized == "trash" || normalized == "recycle" ||
        normalized == "move_to_recycle_bin") return ActionType::MoveToRecycleBin;
    if (normalized == "inspect_window") return ActionType::InspectWindow;
    if (normalized == "focus_window") return ActionType::FocusWindow;
    if (normalized == "set_control_text" || normalized == "set_text")
        return ActionType::SetControlText;
    if (normalized == "invoke_control" || normalized == "invoke")
        return ActionType::InvokeControl;
    if (normalized == "web_search" || normalized == "search_web")
        return ActionType::WebSearch;
    return ActionType::Unknown;
}

RiskLevel RiskLevelFromString(const std::string& value)
{
    const std::string normalized = NormalizeName(value);
    if (normalized == "read" || normalized == "read_only") return RiskLevel::ReadOnly;
    if (normalized == "write" || normalized == "reversible" ||
        normalized == "reversible_write") return RiskLevel::ReversibleWrite;
    return RiskLevel::Destructive;
}

ExecutionMode ExecutionModeFromString(const std::string& value)
{
    const std::string normalized = NormalizeName(value);
    if (normalized == "supervised") return ExecutionMode::Supervised;
    if (normalized == "approved_scope" || normalized == "autonomous")
    {
        return ExecutionMode::ApprovedScope;
    }
    return ExecutionMode::Disabled;
}

RiskLevel RiskForAction(ActionType value)
{
    switch (value)
    {
        case ActionType::ListDirectory:
        case ActionType::ReadTextFile:
        case ActionType::InspectWindow:
        case ActionType::WebSearch:
            return RiskLevel::ReadOnly;
        case ActionType::CreateDirectory:
        case ActionType::CopyFile:
        case ActionType::MoveFile:
        case ActionType::RenamePath:
        case ActionType::MoveToRecycleBin:
        case ActionType::FocusWindow:
        case ActionType::SetControlText:
        case ActionType::InvokeControl:
            return RiskLevel::ReversibleWrite;
        case ActionType::Unknown:
        default:
            return RiskLevel::Destructive;
    }
}

std::string NewActionId()
{
    static std::atomic<std::uint64_t> counter{1};
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream stream;
    stream << "action-" << ticks << '-' << counter.fetch_add(1, std::memory_order_relaxed);
    return stream.str();
}

std::filesystem::path Utf8ToPath(const std::string& value)
{
#if defined(__cpp_lib_char8_t)
    const std::u8string encoded(
        reinterpret_cast<const char8_t*>(value.data()),
        reinterpret_cast<const char8_t*>(value.data() + value.size()));
    return std::filesystem::path(encoded);
#else
    return std::filesystem::u8path(value);
#endif
}

std::string PathToUtf8(const std::filesystem::path& value)
{
#if defined(__cpp_lib_char8_t)
    const std::u8string encoded = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
#else
    return value.generic_u8string();
#endif
}

} // namespace revia::actions
