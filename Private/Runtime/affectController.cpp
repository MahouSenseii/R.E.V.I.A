#include "Runtime/affectController.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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

AffectSnapshot AffectController::ObserveInput(
    const std::string& userInput,
    const SocialContext& social)
{
    return Apply(
        ModulateForRelationship(Classify(userInput, {}, true), social),
        std::chrono::steady_clock::now());
}

AffectSnapshot AffectController::ObserveTurn(
    const std::string& userInput,
    const std::string& response,
    const bool succeeded,
    const SocialContext& social)
{
    return Apply(
        ModulateForRelationship(Classify(userInput, response, succeeded), social),
        std::chrono::steady_clock::now());
}

AffectController::Candidate AffectController::ModulateForRelationship(
    Candidate candidate,
    const SocialContext& social)
{
    // Thresholds sit outside the defaults on purpose, so a caller that passes no context
    // gets the unmodulated reading and nothing that worked before changes.
    constexpr float closeEnoughToTease = 0.6F;
    constexpr float stranger = 0.2F;
    constexpr float wornDown = 0.4F;

    const float familiarity = std::clamp(social.familiarity, 0.0F, 1.0F);
    const float irritation = std::clamp(social.irritation, 0.0F, 1.0F);
    const float socialEnergy = std::clamp(social.socialEnergy, 0.0F, 1.0F);
    const float confidence = std::clamp(social.confidence, 0.0F, 1.0F);

    // Hostility is where who is speaking matters most, so it is handled first and
    // explicitly rather than falling out of a general intensity nudge.
    if (candidate.state == AffectState::Angry)
    {
        if (familiarity >= closeEnoughToTease && irritation < wornDown)
        {
            // People who know each other insult each other affectionately. Reading that
            // as an attack is the mistake that makes a companion exhausting to talk to.
            candidate.state = AffectState::Playful;
            candidate.intensity *= 0.55F;
            candidate.reason =
                "A sharp remark, but from someone she knows well enough to take it as teasing.";
        }
        else if (familiarity >= closeEnoughToTease)
        {
            // Same words, same person, worse day. Anger is what strangers get; from
            // someone close, on top of everything else, it lands as hurt instead.
            candidate.state = AffectState::Sad;
            candidate.reason =
                "A sharp remark from someone close, landing on a day that had already gone badly.";
        }
        else if (familiarity <= stranger)
        {
            // No shared history to read it as anything gentler.
            candidate.intensity = std::clamp(candidate.intensity * 1.15F, 0.0F, 1.0F);
            candidate.reason += " There is no shared history to soften it.";
        }
    }

    // A fuse that is already short makes the next unwelcome thing land harder, whatever
    // it happened to be.
    if (IsNegativeState(candidate.state))
    {
        candidate.intensity = std::clamp(
            candidate.intensity + irritation * 0.22F, 0.0F, 1.0F);
    }

    // And a bad enough day colours a message that carried nothing in particular.
    if (candidate.state == AffectState::Neutral && irritation >= 0.65F)
    {
        candidate.state = AffectState::Frustrated;
        candidate.intensity = std::clamp(0.30F + 0.30F * irritation, 0.0F, 1.0F);
        candidate.reason =
            "Nothing in the message itself, but her patience was already worn thin.";
    }

    // Playing costs energy she does not always have.
    if (candidate.state == AffectState::Playful && socialEnergy < 0.3F)
    {
        candidate.state = AffectState::Bored;
        candidate.intensity *= 0.8F;
        candidate.reason =
            "There is room to play, but not much energy left to play with.";
    }

    // Warmth is worth more from someone whose warmth she has learned to expect, and less
    // from someone she has no reason to trust yet. Neutral at the default familiarity.
    if (IsPositiveState(candidate.state))
    {
        candidate.intensity = std::clamp(
            candidate.intensity * (1.0F + 0.35F * (familiarity - 0.25F)), 0.0F, 1.0F);
    }

    // The same setback is a defeat when she is already unsure of herself and an
    // annoyance when she is not.
    if (candidate.state == AffectState::Concerned)
    {
        if (confidence < 0.35F)
        {
            candidate.state = AffectState::Sad;
            candidate.reason += " It landed while she was already unsure of herself.";
        }
        else if (confidence >= 0.75F)
        {
            candidate.state = AffectState::Frustrated;
            candidate.reason += " She expected that to work.";
        }
    }

    return candidate;
}

AffectController::Candidate AffectController::ClassifyInternal(
    const InternalStimulus& stimulus)
{
    Candidate candidate;
    const float importance = std::clamp(stimulus.importance, 0.0F, 1.0F);
    const float failure = std::clamp(stimulus.failure, 0.0F, 1.0F);
    const float novelty = std::clamp(stimulus.novelty, 0.0F, 1.0F);

    // Named after the thing that actually happened, so the badge explains itself rather
    // than asserting a mood the user has no way to trace back to a cause.
    const std::string cause = stimulus.detail.empty()
        ? (stimulus.source.empty() ? std::string("Something finished.") : stimulus.source + " reported an outcome.")
        : (stimulus.source.empty() ? stimulus.detail : stimulus.source + ": " + stimulus.detail);

    // Weight by importance so a trivial outcome cannot produce a strong feeling, and add
    // novelty on top because a surprising result is felt harder than an expected one.
    const auto scaled = [&](const float magnitude)
    {
        return std::clamp(0.30F + 0.48F * magnitude * importance + 0.16F * novelty, 0.0F, 1.0F);
    };

    switch (stimulus.kind)
    {
        case InternalEventKind::ActivityFailed:
            if (stimulus.selfCaused && failure >= 0.6F)
            {
                // Her approach, her problem. That is what makes it frustrating rather
                // than merely unfortunate.
                candidate.state = AffectState::Frustrated;
            }
            else if (!stimulus.selfCaused)
            {
                // Something broke underneath her. Concern, not self-reproach.
                candidate.state = AffectState::Concerned;
            }
            else
            {
                // A partial failure she caused is more puzzling than infuriating.
                candidate.state = AffectState::Confused;
            }
            candidate.intensity = scaled(failure);
            candidate.reason = cause;
            return candidate;

        case InternalEventKind::ActionRefused:
            // Nothing broke; she was told no. Kept deliberately mild: a companion who
            // sulks every time policy holds a boundary makes the boundary feel like a
            // punishment the user is inflicting.
            candidate.state = AffectState::Concerned;
            candidate.intensity = std::clamp(0.28F + 0.22F * importance, 0.0F, 1.0F);
            candidate.reason = cause;
            return candidate;

        case InternalEventKind::DiscoveryMade:
            candidate.state = novelty >= 0.7F && importance >= 0.6F
                ? AffectState::Excited
                : AffectState::Curious;
            candidate.intensity = scaled(std::max(novelty, 0.4F));
            candidate.reason = cause;
            return candidate;

        case InternalEventKind::WaitEnded:
            candidate.state = AffectState::Focused;
            candidate.intensity = scaled(0.4F);
            candidate.reason = cause;
            return candidate;

        case InternalEventKind::ActivitySucceeded:
            // Novelty is what turns a success into interest rather than satisfaction.
            candidate.state = novelty >= 0.5F ? AffectState::Excited : AffectState::Pleased;
            candidate.intensity = scaled(1.0F - failure);
            candidate.reason = cause;
            return candidate;
    }

    candidate.state = AffectState::Neutral;
    candidate.reason = cause;
    return candidate;
}

std::optional<AffectSnapshot> AffectController::ObserveInternalEvent(
    const InternalStimulus& stimulus)
{
    // Most of what happens to her is not worth a change of expression. Returning nothing
    // here is the same decision the initiative controller makes when it stays quiet, and
    // it is the common one.
    if (std::clamp(stimulus.importance, 0.0F, 1.0F) < internalImportanceFloor)
    {
        return std::nullopt;
    }

    const Candidate candidate = ClassifyInternal(stimulus);
    if (candidate.state == AffectState::Neutral)
    {
        return std::nullopt;
    }

    const AffectSnapshot before = Current();
    // countsAsInteraction is false: see the header. Her own work is not company.
    const AffectSnapshot after = Apply(
        candidate, std::chrono::steady_clock::now(), false);
    if (after.state == before.state &&
        std::abs(after.intensity - before.intensity) < 0.01F &&
        after.reason == before.reason)
    {
        // The minimum-hold window swallowed it. Publishing an unchanged snapshot would
        // make the shell redraw a transition that never happened.
        return std::nullopt;
    }
    return after;
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
    const std::chrono::steady_clock::time_point now,
    const bool countsAsInteraction)
{
    std::lock_guard lock(mutex);
    candidate.intensity = std::clamp(candidate.intensity, 0.0F, 1.0F);
    if (countsAsInteraction)
    {
        lastInteraction = now;
    }

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
