#include "Policy/capabilityPolicy.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <system_error>

#ifdef _WIN32
#include <windows.h>

// Win32's generic-name macros collide with Revia's typed action names.
#undef CopyFile
#undef CreateDirectory
#undef MoveFile
#endif

namespace revia::policy
{

namespace
{

std::filesystem::path AbsoluteLexical(const std::filesystem::path& value)
{
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(value, error);
    if (error)
    {
        return {};
    }
    return absolute.lexically_normal();
}

std::wstring Comparable(const std::filesystem::path& value)
{
    std::wstring comparable = value.lexically_normal().generic_wstring();
    while (comparable.size() > 1 && comparable.back() == L'/')
    {
        comparable.pop_back();
    }
#ifdef _WIN32
    std::transform(comparable.begin(), comparable.end(), comparable.begin(), [](wchar_t c)
    {
        return static_cast<wchar_t>(std::towlower(c));
    });
#endif
    return comparable;
}

bool ContainsPath(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
    const std::wstring rootValue = Comparable(root);
    const std::wstring candidateValue = Comparable(candidate);
    if (candidateValue == rootValue)
    {
        return true;
    }
    if (rootValue.empty() || candidateValue.size() <= rootValue.size())
    {
        return false;
    }
    return candidateValue.compare(0, rootValue.size(), rootValue) == 0 &&
        candidateValue[rootValue.size()] == L'/';
}

bool PathComponentIsReparsePoint(const std::filesystem::path& value)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(value.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    const auto status = std::filesystem::symlink_status(value, error);
    return !error && std::filesystem::is_symlink(status);
#endif
}

bool RequiresDestination(actions::ActionType type)
{
    return type == actions::ActionType::CopyFile ||
        type == actions::ActionType::MoveFile ||
        type == actions::ActionType::RenamePath;
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// Approved-application matching is shared by UI Automation and desktop operation:
// both are "drive this executable", and neither may reach an unapproved one.
bool ApplicationIsApproved(
    const actions::CapabilitySettings& settings,
    const std::string& application)
{
    const std::string wanted = Lower(application);
    return std::any_of(
        settings.approvedApplications.begin(), settings.approvedApplications.end(),
        [&wanted](const std::string& allowed) { return Lower(allowed) == wanted; });
}

bool TypedTextIsAcceptable(const std::string& value)
{
    return std::none_of(value.begin(), value.end(), [](const unsigned char character)
    {
        return character < 0x20 && character != 0x09 && character != 0x0A;
    });
}

} // namespace

CapabilityPolicy::CapabilityPolicy(actions::CapabilitySettings inputSettings)
    : settings(std::move(inputSettings))
{
    for (const auto& root : settings.approvedRoots)
    {
        const auto lexical = AbsoluteLexical(root);
        if (lexical.empty())
        {
            continue;
        }
        lexicalRoots.push_back(lexical);
        canonicalRoots.push_back(ResolveForPolicy(lexical));
    }
}

actions::PolicyDecision CapabilityPolicy::Evaluate(
    const actions::ActionRequest& request) const
{
    actions::PolicyDecision decision;
    decision.risk = actions::RiskForAction(request.type);
    // OwnerFullAccess is the owner delegating broad operation, so reversible work stops
    // asking. It changes only this ceiling: approved roots, approved applications,
    // approved controls, the desktop-control switches, and destructive confirmation are
    // all still evaluated exactly as they are in Supervised mode.
    const actions::RiskLevel automaticCeiling =
        settings.mode == actions::ExecutionMode::OwnerFullAccess
            ? std::max(settings.autoApproveRiskThrough, actions::RiskLevel::ReversibleWrite)
            : settings.autoApproveRiskThrough;

    if (settings.mode == actions::ExecutionMode::Disabled)
    {
        decision.reason = "Capability execution is disabled.";
        return decision;
    }
    if (request.type == actions::ActionType::Unknown)
    {
        decision.reason = "Unknown actions are never executable.";
        return decision;
    }
    if (request.type == actions::ActionType::WebSearch)
    {
        if (!settings.internet.enabled)
        {
            decision.reason = "Internet access is disabled in capability settings.";
            return decision;
        }
        if (request.value.empty() || request.value.size() > 1024)
        {
            decision.reason = "Internet search requires a query no longer than 1024 bytes.";
            return decision;
        }
        decision.verdict = actions::PolicyVerdict::Allowed;
        decision.reason = "Read-only search approved through the configured bounded provider.";
        return decision;
    }
    if (actions::IsDesktopControlAction(request.type))
    {
        if (request.application.empty() ||
            request.application.find_first_of("/\\") != std::string::npos)
        {
            decision.reason = "Desktop operation requires an executable name without a path.";
            return decision;
        }
        if (!ApplicationIsApproved(settings, request.application))
        {
            decision.reason = "The target application is outside the approved application list.";
            return decision;
        }
        // Delegating a task is not standing consent to drive the machine unprompted.
        if (actions::IsAutonomousRequest(request.requestedBy) &&
            !settings.desktopControl.autonomous)
        {
            decision.reason = "Autonomous desktop operation is a separate permission and is off.";
            return decision;
        }
        if (request.type == actions::ActionType::LaunchApplication &&
            !settings.desktopControl.applicationLaunch)
        {
            decision.reason = "Starting applications is disabled in desktop control settings.";
            return decision;
        }
        const bool pointerAction = request.type == actions::ActionType::MoveCursor ||
            request.type == actions::ActionType::ClickPointer ||
            request.type == actions::ActionType::ScrollPointer;
        if (pointerAction && !settings.desktopControl.pointer)
        {
            decision.reason = "Pointer control is disabled in desktop control settings.";
            return decision;
        }
        const bool keyboardAction = request.type == actions::ActionType::PressKeys ||
            request.type == actions::ActionType::TypeText;
        if (keyboardAction && !settings.desktopControl.keyboard)
        {
            decision.reason = "Keyboard control is disabled in desktop control settings.";
            return decision;
        }

        if (request.type == actions::ActionType::LaunchApplication)
        {
            // An optional file to open. It is checked against the same approved roots as
            // every other filesystem action rather than trusted as a process argument,
            // because a process argument is the shortest path back to a shell.
            if (!request.source.empty())
            {
                const std::filesystem::path lexical = AbsoluteLexical(request.source);
                decision.canonicalSource = ResolveForPolicy(lexical);
                if (lexical.empty() || decision.canonicalSource.empty() ||
                    !IsWithinApprovedRoot(lexical, decision.canonicalSource))
                {
                    decision.reason = "The file to open is outside every approved root.";
                    return decision;
                }
                if (HasReparsePointBelowApprovedRoot(lexical))
                {
                    decision.reason = "The file to open crosses a symbolic link or reparse point.";
                    return decision;
                }
            }
        }
        else if (pointerAction)
        {
            // Aiming is either an element the vision-to-UIA resolver re-verified or a
            // point the owner separately allowed. There is no third option, and the
            // executor still confines the point to the target window.
            if (!request.resolution.visionResolved)
            {
                if (!request.input.hasPoint)
                {
                    decision.reason = "A pointer action needs a resolved element or an explicit point.";
                    return decision;
                }
                if (!settings.desktopControl.rawCoordinates)
                {
                    decision.reason = "Pointing at raw coordinates is disabled; resolve the element first.";
                    return decision;
                }
            }
            if (request.type == actions::ActionType::ClickPointer &&
                (request.input.clickCount < 1 || request.input.clickCount > 3))
            {
                decision.reason = "A click may repeat between one and three times.";
                return decision;
            }
            if (request.type == actions::ActionType::ScrollPointer &&
                (request.input.scrollClicks == 0 ||
                    request.input.scrollClicks < -10 || request.input.scrollClicks > 10))
            {
                decision.reason = "A scroll needs between one and ten wheel detents.";
                return decision;
            }
        }
        else if (request.type == actions::ActionType::PressKeys)
        {
            actions::KeyChord chord;
            std::string chordError;
            if (!actions::ParseKeyChord(request.input.keys, chord, chordError))
            {
                decision.reason = chordError;
                return decision;
            }
        }
        else
        {
            if (request.value.empty())
            {
                decision.reason = "Typing requires non-empty text.";
                return decision;
            }
            if (request.value.size() > settings.desktopControl.maxTypedCharacters)
            {
                decision.reason = "The text exceeds the configured typing length limit.";
                return decision;
            }
            if (!TypedTextIsAcceptable(request.value))
            {
                decision.reason = "Typed text may not contain control characters.";
                return decision;
            }
        }

        if (request.dryRun)
        {
            decision.verdict = actions::PolicyVerdict::Allowed;
            decision.reason = "Desktop-operation dry-run approved; no input was synthesized.";
            return decision;
        }
        if (static_cast<int>(decision.risk) <= static_cast<int>(automaticCeiling))
        {
            decision.verdict = actions::PolicyVerdict::Allowed;
            decision.reason = "Desktop operation is inside the automatic approval ceiling.";
            return decision;
        }
        if (settings.mode == actions::ExecutionMode::ApprovedScope)
        {
            decision.reason = "Desktop operation exceeds the unattended risk ceiling.";
            return decision;
        }
        decision.verdict = actions::PolicyVerdict::RequiresConfirmation;
        decision.reason = "Desktop operation requires confirmation for the allowed application.";
        return decision;
    }
    if (actions::IsUiAutomationAction(request.type))
    {
        if (request.application.empty() || request.application.find_first_of("/\\") != std::string::npos)
        {
            decision.reason = "Desktop actions require an executable name without a path.";
            return decision;
        }
        const std::string requestedApplication = Lower(request.application);
        const bool applicationAllowed = std::any_of(
            settings.approvedApplications.begin(), settings.approvedApplications.end(),
            [&](const std::string& allowed) { return Lower(allowed) == requestedApplication; });
        if (!applicationAllowed)
        {
            decision.reason = "The target application is outside the approved application list.";
            return decision;
        }
        if ((request.type == actions::ActionType::SetControlText ||
                request.type == actions::ActionType::InvokeControl) && request.control.empty())
        {
            decision.reason = "The desktop action requires a control name or automation id.";
            return decision;
        }
        if (request.type == actions::ActionType::SetControlText ||
            request.type == actions::ActionType::InvokeControl)
        {
            const auto controlScope = std::find_if(
                settings.approvedControls.begin(),
                settings.approvedControls.end(),
                [&requestedApplication](const auto& entry)
                {
                    return Lower(entry.first) == requestedApplication;
                });
            if (controlScope == settings.approvedControls.end())
            {
                decision.reason = "The application has no approved control scope.";
                return decision;
            }
            const auto matchesApprovedControl = [&](const std::string& candidate)
            {
                if (candidate.empty())
                {
                    return false;
                }
                const std::string loweredCandidate = Lower(candidate);
                return std::any_of(
                    controlScope->second.begin(),
                    controlScope->second.end(),
                    [&loweredCandidate](const std::string& allowed)
                    {
                        return allowed == "*" || Lower(allowed) == loweredCandidate;
                    });
            };
            if (!matchesApprovedControl(request.control) &&
                !matchesApprovedControl(request.resolution.resolvedName) &&
                !matchesApprovedControl(request.resolution.resolvedAutomationId))
            {
                decision.reason = "The target control is outside the application's approved control list.";
                return decision;
            }
        }
        if (request.type == actions::ActionType::SetControlText && request.value.empty())
        {
            decision.reason = "Setting control text requires a non-empty value.";
            return decision;
        }

        if (request.dryRun ||
            static_cast<int>(decision.risk) <= static_cast<int>(automaticCeiling))
        {
            decision.verdict = actions::PolicyVerdict::Allowed;
            decision.reason = request.dryRun
                ? "Desktop dry-run approved for an allowed application."
                : "Read-only desktop inspection approved for an allowed application.";
            return decision;
        }
        if (settings.mode == actions::ExecutionMode::ApprovedScope)
        {
            decision.reason = "Desktop interaction exceeds the unattended risk ceiling.";
            return decision;
        }
        decision.verdict = actions::PolicyVerdict::RequiresConfirmation;
        decision.reason = "Desktop interaction requires confirmation for the allowed application.";
        return decision;
    }
    if (request.source.empty())
    {
        decision.reason = "The action requires a source path.";
        return decision;
    }
    if (RequiresDestination(request.type) && request.destination.empty())
    {
        decision.reason = "The action requires a destination path.";
        return decision;
    }

    const std::filesystem::path lexicalSource = AbsoluteLexical(request.source);
    if (lexicalSource.empty())
    {
        decision.reason = "The source path could not be normalized.";
        return decision;
    }
    decision.canonicalSource = ResolveForPolicy(lexicalSource);
    if (decision.canonicalSource.empty() ||
        !IsWithinApprovedRoot(lexicalSource, decision.canonicalSource))
    {
        decision.reason = "The source path is outside every approved root.";
        return decision;
    }
    if (HasReparsePointBelowApprovedRoot(lexicalSource))
    {
        decision.reason = "The source path crosses a symbolic link or reparse point.";
        return decision;
    }

    if (RequiresDestination(request.type))
    {
        const std::filesystem::path lexicalDestination = AbsoluteLexical(request.destination);
        decision.canonicalDestination = ResolveForPolicy(lexicalDestination);
        if (lexicalDestination.empty() || decision.canonicalDestination.empty() ||
            !IsWithinApprovedRoot(lexicalDestination, decision.canonicalDestination))
        {
            decision.reason = "The destination path is outside every approved root.";
            return decision;
        }
        if (HasReparsePointBelowApprovedRoot(lexicalDestination))
        {
            decision.reason = "The destination path crosses a symbolic link or reparse point.";
            return decision;
        }
    }

    if (request.dryRun)
    {
        decision.verdict = actions::PolicyVerdict::Allowed;
        decision.reason = "Dry-run approved inside the configured capability scope.";
        return decision;
    }

    if (static_cast<int>(decision.risk) <= static_cast<int>(automaticCeiling))
    {
        decision.verdict = actions::PolicyVerdict::Allowed;
        decision.reason = "Action is inside an approved root and below the automatic risk ceiling.";
        return decision;
    }

    if (settings.mode == actions::ExecutionMode::ApprovedScope)
    {
        decision.verdict = actions::PolicyVerdict::Blocked;
        decision.reason = "Action risk exceeds the unattended approved-scope ceiling.";
        return decision;
    }

    decision.verdict = actions::PolicyVerdict::RequiresConfirmation;
    decision.reason = "Action is in scope but exceeds the supervised automatic risk ceiling.";
    return decision;
}

const actions::CapabilitySettings& CapabilityPolicy::Settings() const
{
    return settings;
}

std::filesystem::path CapabilityPolicy::ResolveForPolicy(
    const std::filesystem::path& value) const
{
    if (value.empty())
    {
        return {};
    }

    std::error_code error;
    std::filesystem::path cursor = value;
    std::vector<std::filesystem::path> missing;
    while (!cursor.empty() && !std::filesystem::exists(cursor, error))
    {
        error.clear();
        missing.push_back(cursor.filename());
        const auto parent = cursor.parent_path();
        if (parent == cursor)
        {
            break;
        }
        cursor = parent;
    }

    std::filesystem::path resolved = std::filesystem::weakly_canonical(cursor, error);
    if (error)
    {
        return {};
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it)
    {
        resolved /= *it;
    }
    return resolved.lexically_normal();
}

bool CapabilityPolicy::IsWithinApprovedRoot(
    const std::filesystem::path& lexicalPath,
    const std::filesystem::path& canonicalPath) const
{
    for (std::size_t index = 0; index < lexicalRoots.size(); ++index)
    {
        if (ContainsPath(lexicalRoots[index], lexicalPath) &&
            ContainsPath(canonicalRoots[index], canonicalPath))
        {
            return true;
        }
    }
    return false;
}

std::optional<std::filesystem::path> CapabilityPolicy::FindLexicalRoot(
    const std::filesystem::path& lexicalPath) const
{
    for (const auto& root : lexicalRoots)
    {
        if (ContainsPath(root, lexicalPath))
        {
            return root;
        }
    }
    return std::nullopt;
}

bool CapabilityPolicy::HasReparsePointBelowApprovedRoot(
    const std::filesystem::path& lexicalPath) const
{
    const auto root = FindLexicalRoot(lexicalPath);
    if (!root.has_value())
    {
        return true;
    }

    std::filesystem::path cursor = *root;
    std::error_code error;
    const auto relative = lexicalPath.lexically_relative(*root);
    for (const auto& component : relative)
    {
        cursor /= component;
        if (!std::filesystem::exists(cursor, error))
        {
            error.clear();
            break;
        }
        if (PathComponentIsReparsePoint(cursor))
        {
            return true;
        }
    }
    return false;
}

} // namespace revia::policy
