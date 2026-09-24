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

// A runtime-created claim about what an action is aimed at.
//
// The window binding that came before this one stopped input from wandering into another
// top-level window, which is necessary and not sufficient: two controls inside one HWND
// can mean entirely different things. Typing into a document and then having focus move
// to a Send button is a change of consequence with no change of window, and the previous
// authorization was about the document.
//
// Every field here is filled in by the runtime from what is actually on screen. Nothing
// in model-authored JSON reaches any of it -- the model may say where to look, and the
// runtime decides what is there. That is why the id is generated here: an identifier
// that could be supplied from outside would be an authorization the model wrote itself.
//
// The UIA runtime id is useful for telling "the same control" from "a different control"
// over a second or two. It is deliberately not treated as durable identity: Windows
// reuses them, and a binding is a short-lived observation rather than a capability.
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

// How long an observation may stand before it has to be taken again.
//
// Two seconds is a compromise measured against what it is protecting: a UI Automation
// tree walk on a heavy application can take several hundred milliseconds, so anything
// much shorter would expire bindings faster than they can be made, and anything much
// longer starts describing a machine that has moved on. Age is only the backstop -- the
// real check is Revalidate, which compares against what is there now.
inline constexpr std::chrono::milliseconds BindingFreshnessLimit{2000};

[[nodiscard]] bool IsFresh(
    const TargetBinding& binding,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds limit = BindingFreshnessLimit);

// Why a binding stopped describing reality, in words that can go in a refusal.
[[nodiscard]] std::string CompareBindings(
    const TargetBinding& authorized,
    const TargetBinding& current);

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

[[nodiscard]] std::optional<std::string> BuildDraftContext(
    const std::vector<DraftContextNode>& nodes);
[[nodiscard]] std::string ValidateActionTarget(
    const ActionRequest& request, const TargetBinding& current);

[[nodiscard]] DraftSnapshot ObserveDraft(const ActionRequest& placement);
[[nodiscard]] std::string CompareDraftSnapshots(const DraftSnapshot& authorized,
    const DraftSnapshot& current, const std::string& expectedValue);

// Whether two bindings describe the same control, when one of them was minted a moment
// before the other for the same intended target.
//
// Separate from Describes, which is the right rule for "has the world drifted since I
// authorized this" and the wrong one here. Describes lets a runtime id decide whenever
// both sides have one, and a Chromium-based application -- Edge, Chrome, anything
// Electron -- rebuilds its accessibility nodes constantly. A regenerated id there means
// the node was recreated, not that the caret moved, and treating the two as the same
// thing makes typing into a browser impossible: the caret is provably on the target, the
// id no longer matches, and the text is refused.
//
// So a matching id still accepts immediately, and a mismatched one falls through to
// requiring every stable property to agree -- automation id, control type, name,
// password-ness, and the exact rectangle. That keeps the protection this check exists
// for. The incident behind it was two text fields in one window, and two distinct fields
// do not occupy the same rectangle.
[[nodiscard]] bool SameControl(
    const TargetBinding& intended, const TargetBinding& current);

#ifdef _WIN32
[[nodiscard]] TargetBinding BindElement(IUIAutomationElement* element,
    IUIAutomationElement* window, const std::string& taskOrigin,
    const std::string& policyVersion);
// Observes the currently focused control and mints a binding for it.
[[nodiscard]] TargetBinding BindFocusedControl(
    IUIAutomation* automation,
    const std::string& taskOrigin,
    const std::string& policyVersion);

// Observes whatever is at a screen point.
[[nodiscard]] TargetBinding BindPoint(
    IUIAutomation* automation,
    int x,
    int y,
    const std::string& taskOrigin,
    const std::string& policyVersion);

// Re-observes and compares. Empty return means it still holds; otherwise the reason.
[[nodiscard]] std::string RevalidateFocusBinding(
    IUIAutomation* automation,
    const TargetBinding& authorized,
    std::chrono::steady_clock::time_point now);

[[nodiscard]] std::string RevalidatePointBinding(
    IUIAutomation* automation,
    const TargetBinding& authorized,
    int x,
    int y,
    std::chrono::steady_clock::time_point now);
#endif

} // namespace revia::actions::windows
