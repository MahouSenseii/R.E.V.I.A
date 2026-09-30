#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <optional>
#include <vector>

struct IUIAutomation;
struct IUIAutomationElement;

namespace revia::actions { struct ActionRequest; }

namespace revia::actions::windows
{

// Runtime-minted binding to a specific window/control; model JSON supplies no fields.
// UIA runtime IDs are temporary observations, not durable identity or authority.
struct TargetBinding
{
    // Runtime-generated. Never parsed from anything.
    std::string id;
    std::chrono::steady_clock::time_point observedAt{};

    // Window identity. void* rather than HWND so the comparison logic below can be
    // tested without Windows.
    void* window = nullptr;
    std::uint32_t processId = 0;
    std::string windowTitle;
    std::string executable;

    // Control identity, in decreasing order of how much it can be trusted.
    std::string runtimeId;
    std::string automationId;
    std::string controlName;
    int controlType = 0;
    bool isPassword = false;
    bool wasFocused = false;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    // Window title plus focused-control identity, so a dialog opening over the target
    // reads as a different context even when the handle is unchanged.
    std::string contextFingerprint;

    // Which task this was observed for, and under which policy. A binding made for one
    // task does not carry over to another, and a policy change invalidates it.
    std::string taskOrigin;
    std::string policyVersion;

    bool valid = false;

    [[nodiscard]] bool Describes(const TargetBinding& other) const;
};

// Age backstop; live Revalidate still checks current target identity.
inline constexpr std::chrono::milliseconds BindingFreshnessLimit{2000};

[[nodiscard]] bool IsFresh(const TargetBinding& binding,
    std::chrono::steady_clock::time_point now, std::chrono::milliseconds limit = BindingFreshnessLimit);

// Why a binding stopped describing reality, in words that can go in a refusal.
[[nodiscard]] std::string CompareBindings(const TargetBinding& authorized, const TargetBinding& current);

// Private runtime evidence: these values never enter a model prompt or activity log.
struct DraftSnapshot
{
    TargetBinding control;
    std::string value;
    std::string context;
    bool valid = false;
};

// Facts from a bounded accessibility-tree read. Ancestor text aggregates can contain
// the draft itself; sibling labels and read-only fields can contain its recipient.
struct DraftContextNode
{
    std::string runtimeId;
    std::string automationId;
    std::string name;
    std::string value;
    int controlType = 0;
    bool ancestorOfDraft = false;
    bool insideDraft = false;
    bool isPassword = false;
    bool isStatus = false;
    bool hasValue = false;
    bool valueKnown = true;
};

[[nodiscard]] std::optional<std::string> BuildDraftContext(const std::vector<DraftContextNode>& nodes);
[[nodiscard]] std::string ValidateActionTarget(const ActionRequest& request, const TargetBinding& current);

[[nodiscard]] DraftSnapshot ObserveDraft(const ActionRequest& placement);
[[nodiscard]] std::string CompareDraftSnapshots(const DraftSnapshot& authorized,
    const DraftSnapshot& current, const std::string& expectedValue);

// Matching runtime IDs accept immediately. Recreated IDs require every stable property
// to match: automation ID, type, name, password status, and exact rectangle.
[[nodiscard]] bool SameControl(const TargetBinding& intended, const TargetBinding& current);

#ifdef _WIN32
[[nodiscard]] TargetBinding BindElement(IUIAutomationElement* element,
    IUIAutomationElement* window, const std::string& taskOrigin, const std::string& policyVersion);
// Observes the currently focused control and mints a binding for it.
[[nodiscard]] TargetBinding BindFocusedControl(IUIAutomation* automation, const std::string& taskOrigin, const std::string& policyVersion);

// Observes whatever is at a screen point.
[[nodiscard]] TargetBinding BindPoint(IUIAutomation* automation,
    int x, int y, const std::string& taskOrigin, const std::string& policyVersion);

// Re-observes and compares. Empty return means it still holds; otherwise the reason.
[[nodiscard]] std::string RevalidateFocusBinding(IUIAutomation* automation,
    const TargetBinding& authorized, std::chrono::steady_clock::time_point now);

[[nodiscard]] std::string RevalidatePointBinding(IUIAutomation* automation,
    const TargetBinding& authorized, int x, int y, std::chrono::steady_clock::time_point now);
#endif

} // namespace revia::actions::windows
