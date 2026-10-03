#pragma once

#include "Runtime/runtimeStamp.h"
#include "Speech/speakerRecognition.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

namespace revia::speech
{

enum class SpeakerEnrollmentPhase
{
    Idle,
    AwaitingConsent,
    AwaitingSample,
    AwaitingConfirmation,
    Committing
};

// Captured by the trusted typed/private adapter; matching evidence never selects this entity.
// enrollmentEpoch tracks purpose revocation, independently of inferred voice delivery audience changes.
struct SpeakerEnrollmentContext
{
    runtime::RuntimeStamp stamp;
    std::uint64_t enrollmentEpoch = 0;
    std::string entityId;
};

struct SpeakerEnrollmentResult
{
    bool handled = false;
    bool completed = false;
    SpeakerEnrollmentPhase phase = SpeakerEnrollmentPhase::Idle;
    std::string message;
};

struct SpeakerEnrollmentSnapshot
{
    SpeakerEnrollmentPhase phase = SpeakerEnrollmentPhase::Idle;
    std::optional<SpeakerEnrollmentContext> context;
    std::chrono::steady_clock::time_point expiresAt;
};

struct SpeakerEnrollmentCallbacks
{
    using Admission = std::function<bool()>;
    std::function<bool(const SpeakerEnrollmentContext&)> admits;
    std::shared_ptr<const ISpeakerRecognitionProvider> provider;
    // Synchronous owner commit checks admission before each identity mutation/save; failures must not claim enrollment.
    std::function<bool(const SpeakerEnrollmentContext&, const SpeakerFeatures&, const Admission&, std::string&)> commit;
};

// Holds only bounded derived features. The capture owner retains WAV cleanup and recording lifecycle.
class SpeakerEnrollmentDialogue
{
  public:
    using Clock = std::chrono::steady_clock;
    explicit SpeakerEnrollmentDialogue(SpeakerEnrollmentCallbacks callbacks = {});
    SpeakerEnrollmentResult HandleTyped(
        const std::string& text, const SpeakerEnrollmentContext& context, Clock::time_point now = Clock::now());
    SpeakerEnrollmentResult HandleVoiceRequest(const std::string& text);
    SpeakerEnrollmentResult CaptureNextSample(
        const std::filesystem::path& transientWave, std::stop_token stopToken = {}, Clock::time_point now = Clock::now());
    SpeakerEnrollmentSnapshot Snapshot(Clock::time_point now = Clock::now());
    void Cancel();

  private:
    SpeakerEnrollmentCallbacks callbacks;
    mutable std::mutex mutex;
    SpeakerEnrollmentPhase phase = SpeakerEnrollmentPhase::Idle;
    std::optional<SpeakerEnrollmentContext> context;
    std::optional<SpeakerFeatures> features;
    Clock::time_point expiresAt;
    std::uint64_t generation = 1;
    unsigned sampleAttempts = 0;
    bool sampleInFlight = false;

    void ClearLocked();
    void ExpireLocked(Clock::time_point now);
    bool Admits(const SpeakerEnrollmentContext& captured) const;
    bool Current(const SpeakerEnrollmentContext& captured, std::uint64_t capturedGeneration, SpeakerEnrollmentPhase expected) const;
    SpeakerEnrollmentResult Finish(const SpeakerEnrollmentContext& captured, std::uint64_t capturedGeneration);
};

} // namespace revia::speech
