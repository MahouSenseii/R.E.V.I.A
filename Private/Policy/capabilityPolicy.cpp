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

bool IsDesktopAction(const actions::ActionType type)
{
    return type == actions::ActionType::InspectWindow ||
        type == actions::ActionType::FocusWindow ||
        type == actions::ActionType::SetControlText ||
        type == actions::ActionType::InvokeControl;
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
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
    if (IsDesktopAction(request.type))
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
            static_cast<int>(decision.risk) <= static_cast<int>(settings.autoApproveRiskThrough))
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

    if (static_cast<int>(decision.risk) <= static_cast<int>(settings.autoApproveRiskThrough))
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
