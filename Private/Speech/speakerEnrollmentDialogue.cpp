#include "Speech/speakerEnrollmentDialogue.h"

#include <cmath>
#include <utility>

namespace revia::speech
{
namespace
{
constexpr auto RequestLifetime = std::chrono::minutes(2);
constexpr unsigned MaximumSampleAttempts = 2;

enum class Command
{
    Other,
    Request,
    Consent,
    Confirm,
    Cancel
};

std::string Normalize(const std::string& text)
{
    if (text.size() > 128)
        return {};
    std::string result;
    bool spacing = false;
    for (const unsigned char value : text)
    {
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n')
        {
            spacing = !result.empty();
            continue;
        }
        if (spacing)
            result += ' ';
        spacing = false;
        result += static_cast<char>(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
    }
    if (!result.empty() && (result.back() == '.' || result.back() == '!' || result.back() == '?'))
        result.pop_back();
    return result;
}

Command Parse(const std::string& text)
{
    const auto normalized = Normalize(text);
    if (normalized == "remember my voice" || normalized == "please remember my voice" || normalized == "update my voice sample")
        return Command::Request;
    if (normalized == "i consent to local speaker recognition only")
        return Command::Consent;
    if (normalized == "confirm this voice sample" || normalized == "confirm my voice sample")
        return Command::Confirm;
    if (normalized == "cancel voice enrollment")
        return Command::Cancel;
    return Command::Other;
}

bool SamePurpose(const SpeakerEnrollmentContext& left, const SpeakerEnrollmentContext& right)
{
    return left.stamp.SameSession(right.stamp) && left.stamp.policyVersion == right.stamp.policyVersion &&
           left.enrollmentEpoch == right.enrollmentEpoch && left.entityId == right.entityId;
}

bool ValidFeatures(const SpeakerFeatures& features)
{
    double norm = 0;
    for (const auto value : features.values)
    {
        if (!std::isfinite(value) || std::abs(value) > 1)
            return false;
        norm += static_cast<double>(value) * value;
    }
    return features.version == SpeakerFeatureVersion && norm > 0;
}

std::string Instructions(const SpeakerEnrollmentPhase phase)
{
    switch (phase)
    {
    case SpeakerEnrollmentPhase::AwaitingConsent:
        return "For the explicitly selected person, type: I consent to local speaker recognition only. "
               "This does not authorize private access, voice cloning or recording.";
    case SpeakerEnrollmentPhase::AwaitingSample:
        return "The next independently supplied transient sample can be checked. Recording has not been started. "
               "You will confirm the sample separately before it is saved.";
    case SpeakerEnrollmentPhase::AwaitingConfirmation:
        return "A bounded voice feature sample is ready. If this independently supplied sample is yours, "
               "type: confirm this voice sample. Otherwise type: cancel voice enrollment.";
    case SpeakerEnrollmentPhase::Committing:
        return "The confirmed sample is being saved through the identity owner.";
    default:
        return "In a private typed conversation, explicitly select an existing person and type: remember my voice. "
               "Recognition does not authenticate anyone or authorize voice cloning.";
    }
}
} // namespace

SpeakerEnrollmentDialogue::SpeakerEnrollmentDialogue(SpeakerEnrollmentCallbacks injected) : callbacks(std::move(injected))
{
}

void SpeakerEnrollmentDialogue::ClearLocked()
{
    if (features)
        features->values.fill(0);
    features.reset();
    context.reset();
    phase = SpeakerEnrollmentPhase::Idle;
    expiresAt = {};
    sampleAttempts = 0;
    sampleInFlight = false;
    ++generation;
}

void SpeakerEnrollmentDialogue::ExpireLocked(const Clock::time_point now)
{
    if (phase != SpeakerEnrollmentPhase::Idle && now >= expiresAt)
        ClearLocked();
}

bool SpeakerEnrollmentDialogue::Admits(const SpeakerEnrollmentContext& captured) const
{
    if (!callbacks.admits || !callbacks.provider || !callbacks.commit || captured.entityId.empty() || captured.entityId.size() > 256 ||
        captured.stamp.companionId.empty() || captured.stamp.sessionId.empty() || !captured.stamp.generation ||
        !captured.stamp.policyVersion || !captured.enrollmentEpoch)
        return false;
    try
    {
        return callbacks.admits(captured);
    }
    catch (...)
    {
        return false;
    }
}

bool SpeakerEnrollmentDialogue::Current(
    const SpeakerEnrollmentContext& captured, const std::uint64_t capturedGeneration, const SpeakerEnrollmentPhase expected) const
{
    const auto matches = [&]
    {
        return generation == capturedGeneration && phase == expected && context && SamePurpose(*context, captured) &&
               Clock::now() < expiresAt;
    };
    {
        std::lock_guard lock(mutex);
        if (!matches())
            return false;
    }
    if (!Admits(captured))
        return false;
    std::lock_guard lock(mutex);
    return matches();
}

SpeakerEnrollmentResult SpeakerEnrollmentDialogue::HandleTyped(
    const std::string& text, const SpeakerEnrollmentContext& captured, const Clock::time_point now)
{
    const auto command = Parse(text);
    if (command == Command::Other)
    {
        const auto state = Snapshot(now);
        return {false, false, state.phase, {}};
    }
    if (command == Command::Cancel)
    {
        Cancel();
        return {true, false, SpeakerEnrollmentPhase::Idle, "Pending voice enrollment was cancelled; no new sample was saved."};
    }
    std::uint64_t observedGeneration;
    {
        std::lock_guard lock(mutex);
        ExpireLocked(now);
        observedGeneration = generation;
    }
    const bool admitted = Admits(captured);
    std::unique_lock lock(mutex);
    ExpireLocked(now);
    if (generation != observedGeneration)
        return {true, false, phase, "The enrollment request changed. Check the current request before continuing."};
    if (!admitted)
    {
        ClearLocked();
        return {true, false, phase, Instructions(phase)};
    }
    if (command == Command::Request)
    {
        if (context && SamePurpose(*context, captured))
            return {true, false, phase, Instructions(phase)};
        ClearLocked();
        context = captured;
        phase = SpeakerEnrollmentPhase::AwaitingConsent;
        expiresAt = now + RequestLifetime;
        return {true, false, phase, Instructions(phase)};
    }
    if (!context || !SamePurpose(*context, captured))
    {
        ClearLocked();
        return {true, false, phase, Instructions(phase)};
    }
    if (command == Command::Consent && phase == SpeakerEnrollmentPhase::AwaitingConsent)
    {
        phase = SpeakerEnrollmentPhase::AwaitingSample;
        return {true, false, phase, Instructions(phase)};
    }
    if (command == Command::Confirm && phase == SpeakerEnrollmentPhase::AwaitingConfirmation && features)
    {
        phase = SpeakerEnrollmentPhase::Committing;
        const auto capturedGeneration = generation;
        const auto original = *context;
        lock.unlock();
        return Finish(original, capturedGeneration);
    }
    return {true, false, phase, Instructions(phase)};
}

SpeakerEnrollmentResult SpeakerEnrollmentDialogue::HandleVoiceRequest(const std::string& text)
{
    const auto command = Parse(text);
    if (command == Command::Cancel)
    {
        Cancel();
        return {true, false, SpeakerEnrollmentPhase::Idle, "Pending voice enrollment was cancelled; no new sample was saved."};
    }
    if (command == Command::Other)
        return {};
    return {true, false, SpeakerEnrollmentPhase::Idle, Instructions(SpeakerEnrollmentPhase::Idle)};
}

SpeakerEnrollmentResult SpeakerEnrollmentDialogue::CaptureNextSample(
    const std::filesystem::path& transientWave, const std::stop_token stopToken, const Clock::time_point now)
{
    SpeakerEnrollmentContext captured;
    std::uint64_t capturedGeneration;
    {
        std::lock_guard lock(mutex);
        ExpireLocked(now);
        if (phase != SpeakerEnrollmentPhase::AwaitingSample || !context || sampleInFlight)
            return {false, false, phase, {}};
        if (stopToken.stop_requested())
        {
            ClearLocked();
            return {true, false, phase, "Pending voice enrollment was cancelled."};
        }
        captured = *context;
        capturedGeneration = generation;
        sampleInFlight = true;
        ++sampleAttempts;
    }
    std::optional<SpeakerFeatures> extracted;
    if (Current(captured, capturedGeneration, SpeakerEnrollmentPhase::AwaitingSample))
    {
        try
        {
            std::string ignored;
            extracted = callbacks.provider->Extract(transientWave, stopToken, ignored);
        }
        catch (...)
        {
        }
    }
    const bool admitted = !stopToken.stop_requested() && Current(captured, capturedGeneration, SpeakerEnrollmentPhase::AwaitingSample);
    std::lock_guard lock(mutex);
    ExpireLocked(Clock::now());
    if (generation != capturedGeneration)
        return {false, false, phase, {}};
    sampleInFlight = false;
    if (!admitted)
    {
        ClearLocked();
        return {true, false, phase, "The enrollment purpose is no longer current; the sample was discarded."};
    }
    if (!extracted || !ValidFeatures(*extracted))
    {
        if (sampleAttempts >= MaximumSampleAttempts)
        {
            ClearLocked();
            return {true, false, phase, "The bounded sample attempts were exhausted. Start a new request if you want to try again."};
        }
        return {
            true, false, phase, "The sample could not provide usable voice features. One independently supplied sample attempt remains."};
    }
    features = *extracted;
    extracted->values.fill(0);
    phase = SpeakerEnrollmentPhase::AwaitingConfirmation;
    return {true, false, phase, Instructions(phase)};
}

SpeakerEnrollmentResult SpeakerEnrollmentDialogue::Finish(const SpeakerEnrollmentContext& captured, const std::uint64_t capturedGeneration)
{
    SpeakerFeatures confirmed;
    {
        std::lock_guard lock(mutex);
        if (generation != capturedGeneration || phase != SpeakerEnrollmentPhase::Committing || !features)
            return {true, false, phase, "The pending enrollment changed; no success is confirmed."};
        confirmed = *features;
    }
    const auto admits = [this, captured, capturedGeneration]
    { return Current(captured, capturedGeneration, SpeakerEnrollmentPhase::Committing); };
    bool committed = false;
    if (admits())
    {
        try
        {
            std::string ignored;
            committed = callbacks.commit(captured, confirmed, admits, ignored);
        }
        catch (...)
        {
        }
    }
    confirmed.values.fill(0);
    const bool current = admits();
    std::lock_guard lock(mutex);
    if (generation != capturedGeneration)
        return {true, false, phase, "The enrollment purpose changed; no success is confirmed."};
    ClearLocked();
    if (!committed || !current)
        return {true, false, phase, "The identity owner could not confirm a current saved voice sample. No enrollment success is claimed."};
    return {true, true, phase,
        "The separately confirmed voice sample was saved for local recognition only. "
        "It does not authenticate anyone or authorize private access or voice cloning."};
}

SpeakerEnrollmentSnapshot SpeakerEnrollmentDialogue::Snapshot(const Clock::time_point now)
{
    std::lock_guard lock(mutex);
    ExpireLocked(now);
    return {phase, context, expiresAt};
}

void SpeakerEnrollmentDialogue::Cancel()
{
    std::lock_guard lock(mutex);
    ClearLocked();
}
} // namespace revia::speech
