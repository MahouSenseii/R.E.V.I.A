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

// Runtime routing inputs are assembled only by ConversationRuntime::BuildRoutingContext.
// Every field must have a real producer; attachments and dispatched action turns supply no routing inputs.
struct RoutingContext
{
    bool visionRequired = false;
    bool expertVisionPreferred = false;
    bool explicitResearch = false;
    // Whether the previous delivered answer in this conversation is one the runtime has
    // its own reason to distrust: the generation failed, the deterministic filter had to
    // replace the reply, or the quality monitor found it ungrounded. Observed by the
    // runtime after delivery, never asked of the model -- a model's opinion of its own
    // last answer is not evidence about it.
    bool previousUncertainty = false;
    std::size_t recentContextCharacters = 0;
    // Transient prior successful answer tier lets meaningful short follow-ups inherit effort.
    // Missing, failed, cancelled, restored or evaluation history falls back conservatively, never implicitly Fast.
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
