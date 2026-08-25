#pragma once

#include <cstddef>
#include <string>

namespace revia::intelligence
{

enum class IntelligenceTier
{
    Reflex,
    Fast,
    Main,
    Expert,
    Vision,
    ExpertVision
};

enum class ReasoningMode
{
    Fast,
    Deep
};

struct RoutingContext
{
    bool visionRequired = false;
    bool expertVisionPreferred = false;
    bool explicitResearch = false;
    bool toolUseRequested = false;
    bool previousUncertainty = false;
    int suppliedFileCount = 0;
    std::size_t recentContextCharacters = 0;
};

struct IntelligenceDecision
{
    IntelligenceTier requestedTier = IntelligenceTier::Main;
    IntelligenceTier selectedTier = IntelligenceTier::Main;
    ReasoningMode mode = ReasoningMode::Fast;
    std::string selectedModel;
    std::string reason;
    float confidence = 0.0F;
    bool fallbackUsed = false;
    std::string fallbackReason;
};

[[nodiscard]] std::string ToString(IntelligenceTier tier);
[[nodiscard]] std::string ToString(ReasoningMode mode);

} // namespace revia::intelligence
