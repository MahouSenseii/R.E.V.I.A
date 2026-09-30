#pragma once

#include "Actions/actionTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace revia::actions::windows
{

// Published name is observed evidence; inferred label and container are separate inferences.
// An empty name is valid and must not be replaced with inferred evidence.
struct ObservedControl
{
    std::string name;
    // What UIA's LabeledBy points at, when the element itself has no name. An
    // inference, and marked as one wherever it is used.
    std::string inferredLabel;
    // The nearest ancestor with a name. What distinguishes one nameless field from
    // another when nothing else does.
    std::string containerName;
    std::string automationId;
    std::string runtimeId;
    int controlType = 0;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool enabled = false;
    // From UI Automation's IsPassword, not from the name. The one signal here that is
    // not a guess, and the reason admitting unnamed controls is safe: a nameless
    // password box would otherwise become an ordinary typeable candidate.
    bool isPassword = false;
    // True when this element published no name of its own and was admitted on the
    // strength of what it can do and where it sits. Carried so that a decision about it
    // can be held to a higher bar than one about a control that said what it was.
    bool nameless = false;
    // What can be done with it, read from the patterns it advertises. Knowing a button
    // can be invoked is what lets a decision prefer InvokeControl over aiming a pointer
    // at it, which is the ordering the architecture asks for.
    bool invokable = false;
    bool editable = false;
    bool toggleable = false;
    bool selectable = false;

    [[nodiscard]] int CentreX() const { return left + (right - left) / 2; }
    [[nodiscard]] int CentreY() const { return top + (bottom - top) / 2; }
};

// Read-only observation grants no authority. Application strings are untrusted data;
// actions still require the normal policy, confirmation, and audit path.
struct DesktopObservation
{
    bool succeeded = false;
    std::string failure;

    // Runtime-minted observation identity, age, and window ground visual targets.
    // Never accept these fields from parsed input.
    std::string id;
    // Strictly increasing for the life of the process, across every observer. An action
    // holding an older generation is holding a description of a screen that has since
    // been looked at again and possibly replaced.
    std::uint64_t generation = 0;
    // Steady-clock milliseconds, so age is measurable without a wall clock that can
    // jump.
    std::uint64_t observedAtMs = 0;

    std::string foregroundApplication;
    std::string foregroundTitle;
    // Stable window identity. void* rather than HWND so this stays comparable in a test
    // that has no desktop, matching TargetBinding.
    void* foregroundWindow = nullptr;
    std::uint32_t foregroundProcessId = 0;
    int windowLeft = 0;
    int windowTop = 0;
    int windowRight = 0;
    int windowBottom = 0;

    std::vector<ObservedControl> controls;
    // How many were found but left out of `controls` by the cap. Reported rather than
    // dropped silently, because "there was more" changes what a decision should conclude
    // from not finding something.
    std::size_t omittedControls = 0;

    // Fingerprint detects progress and records screen evidence; it is not a visual freshness gate.
    // Freshness uses observation generation, window identity, and geometry despite pixel changes.
    [[nodiscard]] std::string Fingerprint() const;

    // Bounded text for a decision prompt, newest-relevant first. Never the whole tree:
    // a browser advertises thousands of elements and a prompt cannot hold them.
    [[nodiscard]] std::string Describe(std::size_t maximumControls = 40) const;
};

// Age backstop allows model-planning latency; targets must still have the newest
// observation and unchanged window geometry. Long-paused targets are refused.
inline constexpr std::uint64_t VisualTargetFreshnessMs = 30000;

// Read from Windows at execution time, never from the request; compared with prior evidence.
struct VisualTargetFacts
{
    std::uint64_t latestGeneration = 0;
    std::uint64_t nowMs = 0;
    void* foregroundWindow = nullptr;
    std::uint32_t foregroundProcessId = 0;
    int windowLeft = 0;
    int windowTop = 0;
    int windowRight = 0;
    int windowBottom = 0;
    bool windowBoundsKnown = false;
};

// Returns a refusal reason when visual evidence is stale; empty means still valid.
[[nodiscard]] std::string CompareVisualTarget(const ActionRequest::ElementResolutionEvidence& target, const VisualTargetFacts& current);

// Reads the foreground window through UI Automation. It performs no action and needs no
// capability of its own -- it is eyes, and eyes are not hands.
class DesktopObserver
{
public:
    // maximumControls caps what is collected, not just what is shown, so a pathological
    // window cannot make an observation cost seconds.
    [[nodiscard]] DesktopObservation Observe(std::size_t maximumControls = 160) const;

    // Process-wide latest observation generation. Failed observations also advance it
    // and invalidate targets created before them.
    [[nodiscard]] static std::uint64_t LatestGeneration();
};

} // namespace revia::actions::windows
