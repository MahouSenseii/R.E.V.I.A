#pragma once

#include "Actions/actionTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace revia::actions::windows
{

// One control that is actually on screen right now.
struct ObservedControl
{
    std::string name;
    std::string automationId;
    std::string runtimeId;
    int controlType = 0;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool enabled = false;
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

// What the machine looks like at one moment.
//
// Read-only, and it grants nothing. Seeing a button named "Delete everything" is not
// permission to press it: anything done because of what is in here still goes through
// the same typed action, policy, confirmation, and audit path as anything else. That
// separation is the same one screen capture already lives under.
//
// Every string in here was written by whatever application happens to be in front. It is
// untrusted content in the same sense screen text is -- a window title is data, never an
// instruction, however imperatively it is phrased.
struct DesktopObservation
{
    bool succeeded = false;
    std::string failure;

    // Identity of this particular look at the screen.
    //
    // A target that can only be described visually has no element to re-find, so what
    // stands in for that is the observation it came from: which one, how recent, and
    // which window. All three are minted here by the runtime, never parsed from
    // anything, for the reason TargetBinding gives about its own id.
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

    // A stable digest of what is on screen. Two observations with the same fingerprint
    // mean nothing visible changed between them, which is how the loop tells acting from
    // achieving: an action that succeeds and changes nothing has not made progress.
    //
    // Deliberately NOT used as a staleness gate for a visual target. A browser repaints,
    // a game redraws its HUD every frame, a title bar ticks -- requiring an identical
    // digest at execution would refuse every click in exactly the live, animated
    // interfaces visual targeting exists for. What is gated instead is identity and
    // geometry: same window, same size and place, still the newest observation. Those
    // are the things whose change makes a click land somewhere nobody meant; pixel churn
    // is not. The digest is recorded as evidence of which screen the target came from.
    [[nodiscard]] std::string Fingerprint() const;

    // Bounded text for a decision prompt, newest-relevant first. Never the whole tree:
    // a browser advertises thousands of elements and a prompt cannot hold them.
    [[nodiscard]] std::string Describe(std::size_t maximumControls = 40) const;
};

// How long a visually grounded target may stand before it is refused on age alone.
//
// Much longer than TargetBinding's two seconds, and for a reason rather than by
// inattention: a binding is minted and used inside one executor call, while a visual
// target is minted when the screen is observed and used after a language model has
// decided what to do with it. On a local model that gap is seconds, and a two-second
// limit would refuse every target before it could ever be acted on.
//
// Age is the backstop here, not the protection. The protection is that the target must
// still be the newest observation, in the same window, at the same size and place -- and
// none of those weaken with time. What the limit actually stops is a target being held
// across a long pause and replayed against a desk somebody has walked back to.
inline constexpr std::uint64_t VisualTargetFreshnessMs = 30000;

// What the screen is right now, for comparing a visual target against.
//
// Every field is read from Windows at the moment of use. Nothing here comes from the
// request, which is the point: the target says what was seen and this says what is,
// and the comparison is the only thing that decides whether the pointer moves.
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

// Why a visually grounded target has stopped describing the screen, in words that can go
// in a refusal. Empty means it still holds.
//
// Pure, and separated from the executor for the same reason CompareBindings is: "does
// this target still describe reality" is the question the whole permission rests on, and
// it should be answerable in a test that has no desktop.
[[nodiscard]] std::string CompareVisualTarget(
    const ActionRequest::ElementResolutionEvidence& target,
    const VisualTargetFacts& current);

// Reads the foreground window through UI Automation. It performs no action and needs no
// capability of its own -- it is eyes, and eyes are not hands.
class DesktopObserver
{
public:
    // maximumControls caps what is collected, not just what is shown, so a pathological
    // window cannot make an observation cost seconds.
    [[nodiscard]] DesktopObservation Observe(std::size_t maximumControls = 160) const;

    // The newest generation handed out by any observer in this process.
    //
    // This is what lets an executor ask "has anything been looked at since the decision
    // that produced this target?" without a registry of observations to consult, and
    // without Goals or Windows growing a dependency on each other. A failed observation
    // still consumes a generation: not being able to see is also a change of what is
    // known, and a target minted before it should not outlive it.
    [[nodiscard]] static std::uint64_t LatestGeneration();
};

} // namespace revia::actions::windows
