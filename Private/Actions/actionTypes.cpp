#include "Actions/actionTypes.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <vector>
#include <chrono>
#include <cctype>
#include <sstream>

namespace revia::actions
{

std::string ActionOutcome::Message() const
{
    return auditError.empty() ? result.message : result.message + "\nAudit error: " + auditError;
}

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
        case ActionType::LaunchApplication: return "launch_application";
        case ActionType::MoveCursor: return "move_cursor";
        case ActionType::ClickPointer: return "click_pointer";
        case ActionType::DragPointer: return "drag_pointer";
        case ActionType::ScrollPointer: return "scroll_pointer";
        case ActionType::PressKeys: return "press_keys";
        case ActionType::TypeText: return "type_text";
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
        case ExecutionMode::OwnerFullAccess: return "owner_full_access";
        default: return "disabled";
    }
}

std::string ToString(const CapabilitySettings::DesktopControl::InputScope value)
{
    return value == CapabilitySettings::DesktopControl::InputScope::WholeDesktop
        ? "whole_desktop" : "approved_applications";
}

CapabilitySettings::DesktopControl::InputScope InputScopeFromString(const std::string& value)
{
    // Anything unrecognized falls back to the narrow scope, because an unreadable
    // setting must not be the reason the pointer gets the whole machine.
    return NormalizeName(value) == "whole_desktop"
        ? CapabilitySettings::DesktopControl::InputScope::WholeDesktop
        : CapabilitySettings::DesktopControl::InputScope::ApprovedApplications;
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
    if (normalized == "launch_application" || normalized == "launch" ||
        normalized == "open_application") return ActionType::LaunchApplication;
    if (normalized == "move_cursor" || normalized == "move_pointer")
        return ActionType::MoveCursor;
    if (normalized == "click_pointer" || normalized == "click")
        return ActionType::ClickPointer;
    if (normalized == "drag_pointer" || normalized == "drag")
        return ActionType::DragPointer;
    if (normalized == "scroll_pointer" || normalized == "scroll")
        return ActionType::ScrollPointer;
    if (normalized == "press_keys" || normalized == "press") return ActionType::PressKeys;
    if (normalized == "type_text" || normalized == "type") return ActionType::TypeText;
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
    if (normalized == "owner_full_access") return ExecutionMode::OwnerFullAccess;
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
        case ActionType::LaunchApplication:
        case ActionType::MoveCursor:
        case ActionType::ClickPointer:
        case ActionType::DragPointer:
        case ActionType::ScrollPointer:
        case ActionType::PressKeys:
        case ActionType::TypeText:
            return RiskLevel::ReversibleWrite;
        case ActionType::Unknown:
        default:
            return RiskLevel::Destructive;
    }
}

bool IsUiAutomationAction(const ActionType value)
{
    return value == ActionType::InspectWindow || value == ActionType::FocusWindow ||
        value == ActionType::SetControlText || value == ActionType::InvokeControl;
}

bool IsSynthesizedInputAction(const ActionType value)
{
    return value == ActionType::MoveCursor || value == ActionType::ClickPointer ||
        value == ActionType::DragPointer || value == ActionType::ScrollPointer ||
        value == ActionType::PressKeys || value == ActionType::TypeText;
}

bool IsDesktopControlAction(const ActionType value)
{
    return value == ActionType::LaunchApplication || IsSynthesizedInputAction(value);
}

bool IsAutonomousRequest(const std::string& requestedBy)
{
    return requestedBy.rfind("autonomous", 0) == 0;
}

namespace
{

// Virtual-key numbers are written out rather than included from <windows.h> so the
// vocabulary stays buildable off Windows; the executor maps them straight through.
const std::map<std::string, int>& SupportedKeys()
{
    static const std::map<std::string, int> keys = []
    {
        std::map<std::string, int> table = {
            {"enter", 0x0D}, {"return", 0x0D}, {"tab", 0x09}, {"escape", 0x1B},
            {"esc", 0x1B}, {"space", 0x20}, {"backspace", 0x08}, {"delete", 0x2E},
            {"insert", 0x2D}, {"home", 0x24}, {"end", 0x23}, {"pageup", 0x21},
            {"pagedown", 0x22}, {"left", 0x25}, {"up", 0x26}, {"right", 0x27},
            {"down", 0x28}, {"minus", 0xBD}, {"plus", 0xBB}, {"comma", 0xBC},
            {"period", 0xBE}, {"slash", 0xBF}, {"backslash", 0xDC},
            {"semicolon", 0xBA}, {"quote", 0xDE}, {"backtick", 0xC0},
            {"leftbracket", 0xDB}, {"rightbracket", 0xDD}
        };
        for (char letter = 'a'; letter <= 'z'; ++letter)
        {
            table.emplace(std::string(1, letter), 0x41 + (letter - 'a'));
        }
        for (char digit = '0'; digit <= '9'; ++digit)
        {
            table.emplace(std::string(1, digit), 0x30 + (digit - '0'));
        }
        for (int index = 1; index <= 24; ++index)
        {
            table.emplace("f" + std::to_string(index), 0x70 + index - 1);
        }
        return table;
    }();
    return keys;
}

std::vector<std::string> SplitChord(const std::string& value)
{
    std::vector<std::string> parts;
    std::string current;
    for (const char character : value)
    {
        if (character == '+')
        {
            parts.push_back(current);
            current.clear();
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(character))) continue;
        current.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))));
    }
    parts.push_back(current);
    return parts;
}

} // namespace

bool ParseKeyChord(const std::string& value, KeyChord& outChord, std::string& outError)
{
    outChord = {};
    if (value.empty() || value.size() > 64)
    {
        outError = "A key chord must be between one and sixty-four characters.";
        return false;
    }

    bool windows = false;
    bool control = false;
    bool alt = false;
    bool shift = false;
    std::string keyName;
    for (const std::string& part : SplitChord(value))
    {
        if (part.empty())
        {
            outError = "A key chord may not contain an empty part.";
            return false;
        }
        if (part == "ctrl" || part == "control") { control = true; continue; }
        if (part == "alt") { alt = true; continue; }
        if (part == "shift") { shift = true; continue; }
        if (part == "win" || part == "meta" || part == "cmd" || part == "super")
        {
            windows = true;
            continue;
        }
        if (!keyName.empty())
        {
            outError = "A key chord may hold modifiers for exactly one key.";
            return false;
        }
        keyName = part;
    }
    if (keyName.empty())
    {
        outError = "A key chord requires one non-modifier key.";
        return false;
    }
    const auto found = SupportedKeys().find(keyName);
    if (found == SupportedKeys().end())
    {
        outError = "Unsupported key name: " + keyName;
        return false;
    }

    std::string normalized;
    if (windows) { normalized += "win+"; outChord.modifierVirtualKeys.push_back(0x5B); }
    if (control) { normalized += "ctrl+"; outChord.modifierVirtualKeys.push_back(0x11); }
    if (alt) { normalized += "alt+"; outChord.modifierVirtualKeys.push_back(0x12); }
    if (shift) { normalized += "shift+"; outChord.modifierVirtualKeys.push_back(0x10); }
    // Aliases collapse to one spelling so the blocklist and the audit trail cannot be
    // sidestepped by writing "esc" instead of "escape".
    std::string canonicalName = keyName;
    for (const auto& entry : SupportedKeys())
    {
        if (entry.second == found->second && entry.first.size() > canonicalName.size())
        {
            canonicalName = entry.first;
        }
    }
    normalized += canonicalName;
    outChord.normalized = normalized;
    outChord.virtualKey = found->second;
    outChord.usesWindowsKey = windows;
    outError.clear();
    return true;
}

bool IsApplicationSwitchingChord(const std::string& normalizedChord)
{
    static const std::vector<std::string> switching = {
        "alt+tab", "alt+shift+tab", "alt+escape", "ctrl+alt+escape", "ctrl+escape",
        "ctrl+shift+escape", "win+tab", "win+d", "win+m", "win+shift+m", "win+b",
        "win+comma", "win+l"};
    return std::find(switching.begin(), switching.end(), normalizedChord) != switching.end();
}

bool IsCommandSurfaceChord(const std::string& normalizedChord)
{
    // Each of these ends at a field that runs whatever is typed into it, which is the
    // shell boundary wearing a different hat.
    static const std::vector<std::string> commandSurfaces = {
        "win+r", "win+x", "win+s", "win+q", "win+i", "win+u"};
    return std::find(commandSurfaces.begin(), commandSurfaces.end(), normalizedChord) !=
        commandSurfaces.end();
}

bool IsAlwaysRefusedChord(const std::string& normalizedChord)
{
    // Windows will not let SendInput produce the secure attention sequence, so accepting
    // this would only be a lie about what happened.
    return normalizedChord == "ctrl+alt+delete";
}

bool IsCommandSurfaceExecutable(const std::string& executableName)
{
    static const std::vector<std::string> shells = {
        "cmd.exe", "powershell.exe", "pwsh.exe", "windowsterminal.exe", "wt.exe",
        "conhost.exe", "openconsole.exe", "bash.exe", "sh.exe", "zsh.exe", "wsl.exe",
        "wslhost.exe", "python.exe", "pythonw.exe", "wscript.exe", "cscript.exe",
        "mshta.exe", "regedit.exe", "reg.exe", "mmc.exe", "taskmgr.exe"};
    std::string lowered = executableName;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return std::find(shells.begin(), shells.end(), lowered) != shells.end();
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
