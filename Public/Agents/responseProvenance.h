#pragma once

#include <string>

namespace revia::agents
{

// Separates Revia's own opinions from requested repetition, roleplay, and runtime replies.
enum class ResponseProvenance
{
    // Only this provenance can express Revia's own opinion.
    NormalGeneration,
    // The user chose these words.
    RequestedRepetition,
    // Speaking as a requested character.
    Roleplay,
    // Deterministic runtime output.
    RuntimeReflex
};

[[nodiscard]] std::string ToString(ResponseProvenance value);

[[nodiscard]] bool MayExpressOwnOpinion(ResponseProvenance value);

// Classifies the instruction before generation; uncertain cases suppress opinion learning.
[[nodiscard]] ResponseProvenance ClassifyRequestedProvenance(const std::string& userInput);

} // namespace revia::agents
