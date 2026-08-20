#pragma once

#include "Vision/visionActionTypes.h"

#include <string>
#include <vector>

namespace revia::actions::windows
{

struct VisionResolverSettings
{
    double minimumConfidence = 0.72;
    double minimumNameAgreement = 0.35;
    double ambiguityMargin = 0.08;
    int maxCandidates = 500;
};

class VisionUiaResolver
{
public:
    [[nodiscard]] vision::UiaResolutionResult Resolve(
        const std::string& application,
        const std::string& windowTitle,
        const vision::VisionActionIntent& intent,
        const VisionResolverSettings& settings) const;

    [[nodiscard]] static vision::CandidateScore ScoreCandidate(
        const vision::VisionActionIntent& intent,
        const vision::UiaCandidate& candidate);

    [[nodiscard]] static vision::UiaResolutionResult SelectBest(
        const std::string& application,
        const std::string& windowTitle,
        const vision::VisionActionIntent& intent,
        const std::vector<vision::UiaCandidate>& candidates,
        const VisionResolverSettings& settings);
};

} // namespace revia::actions::windows
