#include "Runtime/affectController.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <utility>

namespace revia::runtime
{

namespace
{
    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

    bool ContainsAny(const std::string& text, const std::initializer_list<const char*> markers)
    {
        return std::any_of(markers.begin(), markers.end(), [&](const char* marker)
        {
            return text.find(marker) != std::string::npos;
        });
    }
}

std::string ToString(const AffectState state)
{
    switch (state)
    {
        case AffectState::Neutral: return "Neutral";
        case AffectState::Curious: return "Curious";
        case AffectState::Pleased: return "Pleased";
        case AffectState::Concerned: return "Concerned";
        case AffectState::Focused: return "Focused";
        case AffectState::Confused: return "Confused";
        default: return "Neutral";
    }
}

AffectController::AffectController(
    const std::chrono::milliseconds inputMinimumHold,
    const std::chrono::milliseconds inputDecayAfter)
    : minimumHold(inputMinimumHold), decayAfter(inputDecayAfter)
{
}

AffectSnapshot AffectController::Reset()
{
    std::lock_guard lock(mutex);
    snapshot = {};
    lastChanged = std::chrono::steady_clock::now();
    lastObserved = lastChanged;
    return snapshot;
}

AffectSnapshot AffectController::ObserveTurn(
    const std::string& userInput,
    const std::string& response,
    const bool succeeded)
{
    return Apply(Classify(userInput, response, succeeded), std::chrono::steady_clock::now());
}

std::optional<AffectSnapshot> AffectController::Tick()
{
    std::lock_guard lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (snapshot.state == AffectState::Neutral || now - lastObserved < decayAfter)
    {
        return std::nullopt;
    }

    snapshot.state = AffectState::Neutral;
    snapshot.intensity = 0.25F;
    snapshot.reason = "Returned to a calm baseline after the turn.";
    lastChanged = now;
    lastObserved = now;
    return snapshot;
}

AffectSnapshot AffectController::Current() const
{
    std::lock_guard lock(mutex);
    return snapshot;
}

AffectController::Candidate AffectController::Classify(
    const std::string& userInput,
    const std::string& response,
    const bool succeeded)
{
    const std::string input = Lower(userInput);
    const std::string reply = Lower(response);
    if (!succeeded)
    {
        return {AffectState::Concerned, 0.9F, "The requested operation did not succeed."};
    }
    if (ContainsAny(reply, {"i don't know", "i do not know", "uncertain", "could not determine"}))
    {
        return {AffectState::Confused, 0.65F, "The available information was incomplete."};
    }
    if (ContainsAny(input, {
        "error", "crash", "failed", "failure", "broken", "not working", "doesn't work",
        "does not work", "issue", "problem", "bug"}))
    {
        return {AffectState::Focused, 0.82F, "A concrete problem needs careful attention."};
    }
    if (ContainsAny(input, {"thank", "great", "perfect", "nice", "fixed", "it works", "working now"}))
    {
        return {AffectState::Pleased, 0.72F, "The turn contained a positive result."};
    }
    if (input.find('?') != std::string::npos ||
        ContainsAny(input, {"why ", "how ", "what ", "where ", "when ", "which "}))
    {
        return {AffectState::Curious, 0.66F, "The turn invites investigation or explanation."};
    }
    if (ContainsAny(input, {"build", "implement", "change", "update", "create", "write", "test", "do it"}))
    {
        return {AffectState::Focused, 0.68F, "The turn contains a concrete task."};
    }
    return {AffectState::Neutral, 0.3F, "The conversation is calm and direct."};
}

AffectSnapshot AffectController::Apply(
    Candidate candidate,
    const std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock(mutex);
    candidate.intensity = std::clamp(candidate.intensity, 0.0F, 1.0F);
    lastObserved = now;

    if (candidate.state == snapshot.state)
    {
        snapshot.intensity = std::clamp(
            snapshot.intensity * 0.55F + candidate.intensity * 0.45F,
            0.0F,
            1.0F);
        snapshot.reason = std::move(candidate.reason);
        return snapshot;
    }

    if (now - lastChanged < minimumHold && candidate.intensity < snapshot.intensity + 0.15F)
    {
        return snapshot;
    }

    snapshot.state = candidate.state;
    snapshot.intensity = candidate.intensity;
    snapshot.reason = std::move(candidate.reason);
    lastChanged = now;
    return snapshot;
}

} // namespace revia::runtime
