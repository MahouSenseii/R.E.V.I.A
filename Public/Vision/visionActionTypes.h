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
    std::string value;
    double modelConfidence = 0.0;
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
