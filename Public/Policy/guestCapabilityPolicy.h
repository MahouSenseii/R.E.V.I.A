#pragma once

#include "Workspace/guestTypes.h"

#include <string>
#include <string_view>

namespace revia::policy
{

struct GuestCapabilitySettings
{
    bool enabled = false;
    bool read = false;
    bool write = false;
    bool execute = false;
    bool allowTaskExecution = false;
    workspace::GuestLimits limits;
    // Captured by host admission for a goal, never accepted from a model proposal.
    workspace::GuestIdentity expectedIdentity;
};

struct GuestCapabilityDecision
{
    bool allowed = false;
    std::string code;
    std::string reason;
    workspace::GuestLimits limits;
};

// Pure guest validation. It never resolves a guest path against the host filesystem.
// Runtime admission separately checks the actual environment and boot identity.
[[nodiscard]] GuestCapabilityDecision EvaluateGuestCapability(
    const workspace::GuestRequest& request, const GuestCapabilitySettings& settings);

// The interpreter the sandboxed runs use; it must be on the approved executable list.
inline constexpr const char* GuestPython = "/usr/bin/python3";

// A normalized relative POSIX path: no empty, "." or ".." components, no backslash or drive.
[[nodiscard]] bool IsGuestRelativePath(std::string_view path);
// A shared project's folder name under projects/ in the guest.
[[nodiscard]] bool IsGuestProjectName(std::string_view name);

// Retains the captured task identity and can only reduce its authority and limits.
[[nodiscard]] GuestCapabilitySettings IntersectGuestCapabilities(
    const GuestCapabilitySettings& captured, const GuestCapabilitySettings& current);

}
