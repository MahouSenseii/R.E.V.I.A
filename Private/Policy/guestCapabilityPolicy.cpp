#include "Policy/guestCapabilityPolicy.h"
#include "Core/utf8.h"

#include <algorithm>
#include <string_view>

namespace revia::policy
{
namespace
{
constexpr std::size_t MaxPathBytes = 2048;
constexpr std::size_t MaxComponentBytes = 255;
constexpr std::size_t MaxArgumentBytes = 8192;
constexpr std::size_t MaxArguments = 128;
constexpr std::size_t MaxInvocationBytes = 65536;

bool HasIdentity(const workspace::GuestIdentity& identity)
{
    return !identity.environmentId.empty() || !identity.bootId.empty();
}

bool SameIdentity(const workspace::GuestIdentity& left, const workspace::GuestIdentity& right)
{
    return left.environmentId == right.environmentId && left.bootId == right.bootId;
}

bool ValidPath(const std::string_view path, const bool absolute, const bool allowRoot)
{
    if (path.empty() || path.size() > MaxPathBytes || !utf8::IsValid(path)) return false;
    if (path == ".") return !absolute && allowRoot;
    if ((path.front() == '/') != absolute) return false;
    if (std::any_of(path.begin(), path.end(), [](const unsigned char c) { return c < 32 || c == 127 || c == '\\'; }))
        return false;
    if (!absolute && path.size() >= 2 && path[1] == ':' &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
        return false;
    std::size_t start = absolute ? 1 : 0;
    while (start < path.size())
    {
        const auto slash = path.find('/', start);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        const auto component = path.substr(start, end - start);
        if (component.empty() || component.size() > MaxComponentBytes || component == "." || component == "..")
            return false;
        if (slash == std::string_view::npos) return true;
        start = slash + 1;
    }
    return false;
}

bool ValidLimits(const workspace::GuestLimits& limits)
{
    return limits.maxReadBytes > 0 && limits.maxReadBytes <= 262144 &&
        limits.maxWriteBytes > 0 && limits.maxWriteBytes <= 262144 &&
        limits.maxOutputBytes > 0 && limits.maxOutputBytes <= 65536 &&
        limits.maxEntries > 0 && limits.maxEntries <= 512 &&
        limits.maxTimeoutMs > 0 && limits.maxTimeoutMs <= 120000;
}

bool ValidDigest(const std::string& value)
{
    return value == "missing" || (value.size() == 64 && std::all_of(value.begin(), value.end(),
        [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
}
}

GuestCapabilityDecision EvaluateGuestCapability(const workspace::GuestRequest& request, const GuestCapabilitySettings& settings)
{
    GuestCapabilityDecision decision;
    decision.limits = settings.limits;
    const auto refuse = [&](const char* code, const char* reason)
    {
        decision.code = code;
        decision.reason = reason;
        return decision;
    };
    if (!settings.enabled) return refuse("guest_disabled", "Guest actions are disabled.");
    if (!ValidLimits(settings.limits)) return refuse("guest_limits", "Guest capability limits are invalid.");
    if (HasIdentity(settings.expectedIdentity) &&
        (settings.expectedIdentity.environmentId.empty() || settings.expectedIdentity.bootId.empty() ||
            !SameIdentity(settings.expectedIdentity, request.expectedIdentity)))
        return refuse("guest_identity", "The request does not match this task's captured guest environment and boot.");

    using workspace::GuestOperation;
    switch (request.operation)
    {
    case GuestOperation::ListDirectory:
    case GuestOperation::ReadText:
        if (!settings.read) return refuse("guest_read_disabled", "Guest reading is disabled.");
        if (!ValidPath(request.path, false, request.operation == GuestOperation::ListDirectory))
            return refuse("guest_path", "The guest path must be a normalized relative POSIX workspace path.");
        break;
    case GuestOperation::WriteText:
        if (!settings.write) return refuse("guest_write_disabled", "Guest writing is disabled.");
        if (!ValidPath(request.path, false, false))
            return refuse("guest_path", "The guest file path must be a normalized relative POSIX workspace path.");
        if (!ValidDigest(request.expectedDigest))
            return refuse("guest_digest", "Guest writing requires missing or the existing file's lowercase SHA256 digest.");
        if (request.content.size() > settings.limits.maxWriteBytes || !utf8::IsValid(request.content))
            return refuse("guest_content", "Guest text must be valid UTF-8 within the configured write limit.");
        break;
    case GuestOperation::ExecuteProcess:
    {
        if (!settings.execute) return refuse("guest_execute_disabled", "Guest process execution is disabled.");
        if (!ValidPath(request.executable, true, false) ||
            std::find(settings.limits.approvedExecutables.begin(), settings.limits.approvedExecutables.end(), request.executable) ==
                settings.limits.approvedExecutables.end())
            return refuse("guest_executable", "The executable must exactly match an approved absolute guest path.");
        if (!ValidPath(request.workingDirectory, false, true))
            return refuse("guest_cwd", "The guest working directory must be a normalized relative POSIX workspace path.");
        if (request.timeoutMs <= 0 || request.timeoutMs > settings.limits.maxTimeoutMs)
            return refuse("guest_timeout", "The process timeout exceeds the configured guest limit.");
        if (request.arguments.size() > MaxArguments)
            return refuse("guest_arguments", "The guest argument count exceeds its limit.");
        std::size_t invocationBytes = request.executable.size() + request.workingDirectory.size();
        for (const auto& argument : request.arguments)
        {
            if (argument.size() > MaxArgumentBytes || argument.find('\0') != std::string::npos || !utf8::IsValid(argument) ||
                argument.size() > MaxInvocationBytes - invocationBytes)
                return refuse("guest_arguments", "Guest arguments must be valid bounded UTF-8 without embedded NUL bytes.");
            invocationBytes += argument.size();
        }
        break;
    }
    default:
        return refuse("guest_operation", "The guest operation is unsupported.");
    }
    decision.allowed = true;
    decision.code = "guest_allowed";
    decision.reason = "The request fits the configured guest capabilities.";
    return decision;
}

GuestCapabilitySettings IntersectGuestCapabilities(const GuestCapabilitySettings& captured, const GuestCapabilitySettings& current)
{
    auto result = captured;
    result.enabled &= current.enabled;
    result.read &= current.read;
    result.write &= current.write;
    result.execute &= current.execute;
    result.allowTaskExecution &= current.allowTaskExecution;
    if (HasIdentity(current.expectedIdentity) && !SameIdentity(captured.expectedIdentity, current.expectedIdentity))
        result.enabled = false;
    auto& limits = result.limits;
    limits.maxReadBytes = std::min(limits.maxReadBytes, current.limits.maxReadBytes);
    limits.maxWriteBytes = std::min(limits.maxWriteBytes, current.limits.maxWriteBytes);
    limits.maxOutputBytes = std::min(limits.maxOutputBytes, current.limits.maxOutputBytes);
    limits.maxEntries = std::min(limits.maxEntries, current.limits.maxEntries);
    limits.maxTimeoutMs = std::min(limits.maxTimeoutMs, current.limits.maxTimeoutMs);
    std::erase_if(limits.approvedExecutables, [&](const std::string& executable)
    {
        return std::find(current.limits.approvedExecutables.begin(), current.limits.approvedExecutables.end(), executable) ==
            current.limits.approvedExecutables.end();
    });
    return result;
}
}
