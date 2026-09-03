#pragma once

#include <cstddef>
#include <optional>
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
    // The tier that produced the previous successfully delivered conversational answer
    // in this conversation, so a short follow-up that only makes sense against that
    // answer ("Why?", "Explain that.") keeps the effort the answer was worth instead of
    // collapsing to the cheapest brain because it is three characters long.
    //
    // Empty whenever there is no trustworthy previous answer to inherit from: a first
    // turn, a cancelled or failed generation, a restored conversation with no recorded
    // tier, or an evaluation run that supplies its own corpus history. Empty means fall
    // back conservatively; it never means Fast.
    //
    // This is transient routing context. It is effort, not identity: it is never
    // persisted, never enters the identity packet, memory, relationships, or
    // development, and it does not change who Revia is.
    std::optional<IntelligenceTier> previousAssistantTier;
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
