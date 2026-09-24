#pragma once

#include "Actions/actionTypes.h"

#include <string>

namespace revia::vision
{

struct ScreenRegion
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] bool IsValid() const
    {
        return right > left && bottom > top;
    }
};

struct VisionActionIntent
{
    actions::ActionType action = actions::ActionType::Unknown;
    std::string targetName;
    std::string targetDescription;
    ScreenRegion region;
    // Where a drag ends, for the pointer family. Regions at both ends, never points.
    ScreenRegion endRegion;
    std::string value;
    // A keyboard chord, for press_keys. Vision may notice that the transferable route to
    // something is a key rather than a click; that proposal follows the ordinary keyboard
    // path and needs no visual-coordinate authority to do it.
    std::string keys;
    int scrollClicks = 0;
    bool horizontalScroll = false;
    int clickCount = 1;
    double modelConfidence = 0.0;

    // Whether this intent needs a region at all.
    //
    // The pointer family aims somewhere, so it does. The keyboard family goes to whatever
    // has focus and the UI Automation family names a control, so neither does -- and
    // requiring a region from them would be asking for visual authority to do something
    // that never touches a coordinate.
    [[nodiscard]] bool NeedsRegion() const
    {
        return action == actions::ActionType::MoveCursor ||
            action == actions::ActionType::ClickPointer ||
            action == actions::ActionType::DragPointer ||
            action == actions::ActionType::ScrollPointer ||
            action == actions::ActionType::InvokeControl ||
            action == actions::ActionType::SetControlText;
    }
};

struct VisionActionParseResult
{
    bool succeeded = false;
    VisionActionIntent intent;
    std::string reason;
};

struct UiaCandidate
{
    std::string name;
    std::string automationId;
    std::string runtimeId;
    int controlType = 0;
    ScreenRegion bounds;
    bool enabled = false;
    bool offscreen = true;
    bool supportsInvoke = false;
    bool supportsValue = false;
};

struct CandidateScore
{
    double spatial = 0.0;
    double nameAgreement = 0.0;
    double total = 0.0;
};

struct UiaElementReference
{
    std::string application;
    std::string windowTitle;
    UiaCandidate element;
    std::string modelTarget;
    ScreenRegion modelRegion;
    double modelConfidence = 0.0;
    CandidateScore score;
};

struct UiaResolutionResult
{
    bool succeeded = false;
    UiaElementReference reference;
    std::string reason;
    int candidatesInspected = 0;
};

} // namespace revia::vision
