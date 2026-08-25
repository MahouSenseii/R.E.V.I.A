#include "Intelligence/humanizationState.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace revia::intelligence
{
namespace
{
float Clamp(const float value)
{
    return std::clamp(value, 0.0F, 1.0F);
}

bool Contains(const std::string& input, const std::string& signal)
{
    std::string lowered = input;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return lowered.find(signal) != std::string::npos;
}
}

void HumanizationController::ObserveInput(
    const std::string& input,
    const runtime::AffectSnapshot& affect)
{
    std::lock_guard lock(mutex);
    state.familiarity = Clamp(state.familiarity + 0.004F);
    state.socialEnergy = Clamp(state.socialEnergy * 0.98F + 0.012F);
    state.irritation = Clamp(state.irritation * state.moodPersistence);

    if (affect.state == runtime::AffectState::Curious)
        state.curiosity = Clamp(state.curiosity + 0.05F * affect.intensity);
    if (affect.state == runtime::AffectState::Playful)
        state.playfulness = Clamp(state.playfulness + 0.05F * affect.intensity);
    if (affect.state == runtime::AffectState::Angry ||
        affect.state == runtime::AffectState::Frustrated ||
        affect.state == runtime::AffectState::Sulky)
        state.irritation = Clamp(state.irritation + 0.12F * affect.intensity);

    if (Contains(input, "i already said") || Contains(input, "you keep repeating"))
        state.irritation = Clamp(state.irritation + 0.08F);
}

void HumanizationController::ObserveOutcome(
    const bool succeeded,
    const runtime::AffectSnapshot& affect)
{
    std::lock_guard lock(mutex);
    state.confidence = Clamp(state.confidence * 0.9F + (succeeded ? 0.075F : 0.02F));
    if (!succeeded) state.unresolvedThought = "A recent request did not complete cleanly.";
    else if (affect.state != runtime::AffectState::Concerned) state.unresolvedThought.clear();
}

HumanizationState HumanizationController::Current() const
{
    std::lock_guard lock(mutex);
    return state;
}

std::string HumanizationController::BuildPromptBlock() const
{
    const HumanizationState snapshot = Current();
    std::ostringstream output;
    output << std::fixed << std::setprecision(2)
        << "Shared Revia state (the same state is supplied to every intelligence tier): "
        << "curiosity=" << snapshot.curiosity
        << ", social energy=" << snapshot.socialEnergy
        << ", irritation=" << snapshot.irritation
        << ", confidence=" << snapshot.confidence
        << ", familiarity=" << snapshot.familiarity
        << ", talkativeness=" << snapshot.talkativeness
        << ", playfulness=" << snapshot.playfulness
        << ". These are behavioral leanings, not labels to recite or a recipe for canned phrases.";
    if (!snapshot.currentInterest.empty())
        output << " Current interest: " << snapshot.currentInterest << '.';
    if (!snapshot.unresolvedThought.empty())
        output << " Unresolved thought: " << snapshot.unresolvedThought;
    return output.str();
}

} // namespace revia::intelligence
