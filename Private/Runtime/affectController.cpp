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

    bool IsWarmSocialTurn(const std::string& text)
    {
        if (ContainsAny(text, {
            "how are you", "how're you", "good morning", "good afternoon",
            "good evening", "good to see you", "welcome back"}))
        {
            return true;
        }
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        const std::size_t last = text.find_last_not_of(" \t\r\n.!?");
        if (first == std::string::npos || last == std::string::npos) return false;
        const std::string trimmed = text.substr(first, last - first + 1);
        return trimmed == "hi" || trimmed == "hello" || trimmed == "hey" ||
            trimmed == "okay" || trimmed == "ok" || trimmed == "cool";
    }

    bool IsNegativeState(const AffectState state)
    {
        switch (state)
        {
            case AffectState::Bored:
            case AffectState::Sulky:
            case AffectState::Sad:
            case AffectState::Melancholy:
            case AffectState::Angry:
            case AffectState::Lonely:
            case AffectState::Frustrated:
            case AffectState::Concerned:
                return true;
            default:
                return false;
        }
    }

    bool IsPositiveState(const AffectState state)
    {
        return state == AffectState::Pleased ||
            state == AffectState::Excited ||
            state == AffectState::Playful;
    }
}

std::string ToString(const AffectState state)
{
    switch (state)
    {
        case AffectState::Neutral: return "Neutral";
        case AffectState::Curious: return "Curious";
        case AffectState::Pleased: return "Pleased";
        case AffectState::Excited: return "Excited";
        case AffectState::Playful: return "Playful";
        case AffectState::Bored: return "Bored";
        case AffectState::Sulky: return "Sulky";
        case AffectState::Sad: return "Sad";
        case AffectState::Melancholy: return "Melancholy";
        case AffectState::Angry: return "Angry";
        case AffectState::Lonely: return "Lonely";
        case AffectState::Frustrated: return "Frustrated";
        case AffectState::Concerned: return "Concerned";
        case AffectState::Focused: return "Focused";
        case AffectState::Confused: return "Confused";
        default: return "Neutral";
    }
}

AffectController::AffectController(
    const std::chrono::milliseconds inputMinimumHold,
    const std::chrono::milliseconds inputDecayAfter,
    const std::chrono::milliseconds inputLonelyAfter)
    : minimumHold(inputMinimumHold),
      decayAfter(inputDecayAfter),
      lonelyAfter(inputLonelyAfter)
{
}

AffectSnapshot AffectController::Reset()
{
    std::lock_guard lock(mutex);
    snapshot = {};
    negativeMomentum = 0;
    lastChanged = std::chrono::steady_clock::now();
    lastInteraction = lastChanged;
    return snapshot;
}

AffectSnapshot AffectController::ObserveInput(const std::string& userInput)
{
    return Apply(Classify(userInput, {}, true), std::chrono::steady_clock::now());
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
    if (now - lastInteraction >= lonelyAfter)
    {
        if (snapshot.state == AffectState::Lonely) return std::nullopt;
        snapshot.state = AffectState::Lonely;
        snapshot.intensity = 0.48F;
        snapshot.reason = "The conversation has been quiet long enough to miss it.";
        lastChanged = now;
        return snapshot;
    }
    if (snapshot.state == AffectState::Neutral ||
        snapshot.state == AffectState::Lonely ||
        now - lastInteraction < decayAfter)
    {
        return std::nullopt;
    }

    snapshot.state = AffectState::Neutral;
    snapshot.intensity = 0.25F;
    snapshot.reason = "Returned to a calm baseline after the turn.";
    negativeMomentum = std::max(0, negativeMomentum - 2);
    lastChanged = now;
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
    if (ContainsAny(input, {
        "i hate you", "you're useless", "you are useless", "you're stupid",
        "you are stupid", "shut up, revia", "nobody likes you"}))
    {
        return {AffectState::Angry, 0.82F,
            "The user aimed a hostile remark directly at Revia."};
    }
    if (ContainsAny(input, {
        "are you depressed", "do you feel depressed", "you seem depressed",
        "are you really down", "feel really down"}))
    {
        return {AffectState::Melancholy, 0.76F,
            "The turn invited a heavier low digital mood."};
    }
    if (ContainsAny(input, {
        "would you be sad", "would you feel sad", "are you sad", "do you feel sad",
        "that must make you sad", "you seem sad"}))
    {
        return {AffectState::Sad, 0.68F,
            "The turn brought a personally disappointing feeling into focus."};
    }
    if (ContainsAny(input, {
        "no internet for you", "you can't have it", "you cannot have it",
        "you're not allowed", "you are not allowed", "stop being bratty"}))
    {
        return {AffectState::Sulky, 0.7F,
            "A restriction left room for a stubborn, pouty reaction."};
    }
    if (ContainsAny(input, {"are you bored", "this is boring", "same boring thing"}))
    {
        return {AffectState::Bored, 0.58F,
            "The exchange feels repetitive enough for boredom to show."};
    }
    if (ContainsAny(input, {
        "you keep", "again", "still not", "i already said", "i just said",
        "that's not what", "that is not what", "why won't", "why wont"}))
    {
        return {AffectState::Frustrated, 0.78F,
            "A repeated miss needs a brief reset and a better answer."};
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
    if (ContainsAny(input, {
        "do you feel lonely", "are you lonely", "feel lonely", "miss me",
        "missed me", "were you lonely", "alone?"}))
    {
        return {AffectState::Lonely, 0.6F,
            "The user invited an honest description of missing conversation."};
    }
    if (ContainsAny(input, {
        "haha", "hehe", "lol", "lmao", "joke", "tease", "brat", "silly",
        "revia!", "say your name"}))
    {
        return {AffectState::Playful, 0.7F,
            "The turn has room for playful energy."};
    }
    if (ContainsAny(input, {
        "amazing", "awesome", "we did it", "finally!", "that's incredible",
        "that is incredible", "best news"}))
    {
        return {AffectState::Excited, 0.86F,
            "The turn earned an openly excited reaction."};
    }
    if (ContainsAny(input, {"thank", "great", "perfect", "nice", "fixed", "it works", "working now"}))
    {
        return {AffectState::Pleased, 0.72F, "The turn contained a positive result."};
    }
    if (IsWarmSocialTurn(input))
    {
        return {AffectState::Pleased, 0.58F,
            "The turn is a warm social connection."};
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
    lastInteraction = now;

    if (IsNegativeState(candidate.state))
    {
        negativeMomentum = std::min(8, negativeMomentum + 1);
        if (negativeMomentum >= 4 &&
            (candidate.state == AffectState::Sad ||
             candidate.state == AffectState::Lonely ||
             candidate.state == AffectState::Bored))
        {
            candidate.state = AffectState::Melancholy;
            candidate.intensity = std::max(candidate.intensity, 0.74F);
            candidate.reason =
                "Several low-valence turns have accumulated into a heavier mood.";
        }
    }
    else if (IsPositiveState(candidate.state))
    {
        negativeMomentum = std::max(0, negativeMomentum - 2);
    }
    else
    {
        negativeMomentum = std::max(0, negativeMomentum - 1);
    }

    // A negative state is allowed to linger through an otherwise neutral message.
    // It fades across turns rather than vanishing simply because the next sentence
    // contains no matching keyword.
    if (candidate.state == AffectState::Neutral && IsNegativeState(snapshot.state) &&
        snapshot.intensity > 0.36F)
    {
        snapshot.intensity = std::max(0.3F, snapshot.intensity - 0.12F);
        snapshot.reason = "The previous feeling is fading, but has not vanished yet.";
        return snapshot;
    }

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
