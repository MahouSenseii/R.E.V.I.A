#pragma once

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

    std::string foregroundApplication;
    std::string foregroundTitle;
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
    [[nodiscard]] std::string Fingerprint() const;

    // Bounded text for a decision prompt, newest-relevant first. Never the whole tree:
    // a browser advertises thousands of elements and a prompt cannot hold them.
    [[nodiscard]] std::string Describe(std::size_t maximumControls = 40) const;
};

// Reads the foreground window through UI Automation. It performs no action and needs no
// capability of its own -- it is eyes, and eyes are not hands.
class DesktopObserver
{
public:
    // maximumControls caps what is collected, not just what is shown, so a pathological
    // window cannot make an observation cost seconds.
    [[nodiscard]] DesktopObservation Observe(std::size_t maximumControls = 160) const;
};

} // namespace revia::actions::windows
