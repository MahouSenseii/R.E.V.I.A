#include "Speech/speakerEnrollmentDialogue.h"

#include "Identity/relationshipRegistry.h"
#include "testSupport.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <thread>

namespace
{
using namespace revia;
using namespace std::chrono_literals;
using tests::Check;
using speech::SpeakerEnrollmentPhase;

speech::SpeakerEnrollmentContext Context()
{
    return {{"fixture-companion", "fixture-session", 1, "turn-one", "attempt-one", 7}, 1, "selected-person"};
}

void WriteWave(const std::filesystem::path& path, const double frequency)
{
    constexpr unsigned rate = 16000, frames = 8000, dataBytes = frames * 2;
    std::ofstream output(path, std::ios::binary);
    const auto little = [&](unsigned value, unsigned count)
    {
        for (unsigned index = 0; index < count; ++index)
            output.put(static_cast<char>(value >> (8 * index)));
    };
    output.write("RIFF", 4);
    little(dataBytes + 36, 4);
    output.write("WAVEfmt ", 8);
    little(16, 4);
    little(1, 2);
    little(1, 2);
    little(rate, 4);
    little(rate * 2, 4);
    little(2, 2);
    little(16, 2);
    output.write("data", 4);
    little(dataBytes, 4);
    for (unsigned index = 0; index < frames; ++index)
        little(static_cast<std::uint16_t>(static_cast<std::int16_t>(10000 * std::sin(2 * std::numbers::pi * frequency * index / rate))), 2);
}

class ControlledProvider final : public speech::ISpeakerRecognitionProvider
{
  public:
    std::function<void()> duringExtract;
    std::optional<speech::SpeakerFeatures> result = []
    {
        speech::SpeakerFeatures features;
        features.values[0] = 1;
        return features;
    }();
    bool throws = false;
    mutable std::atomic<unsigned> calls = 0;

    std::optional<speech::SpeakerFeatures> Extract(const std::filesystem::path&, std::stop_token, std::string& error) const override
    {
        ++calls;
        if (duringExtract)
            duringExtract();
        error = "PRIVATE_FEATURE_ERROR_SENTINEL";
        if (throws)
            throw std::runtime_error(error);
        return result;
    }
};

struct Fixture
{
    tests::ScopedTestDirectory directory;
    identity::RelationshipRegistry identities{directory.root / "identity.json"};
    speech::SpeakerRecognition recognition{identities};
    speech::SpeakerEnrollmentContext current = Context();
    std::atomic<bool> allowed = true;
    std::atomic<unsigned> commits = 0;
    std::shared_ptr<const speech::ISpeakerRecognitionProvider> provider = std::make_shared<speech::WaveSpeakerRecognitionProvider>();
    std::function<void()> beforeCommit;

    Fixture()
    {
        (void)identities.Get(current.entityId);
    }

    speech::SpeakerEnrollmentCallbacks Callbacks()
    {
        return {[&](const auto& captured)
            {
                return allowed && captured.stamp.SameSession(current.stamp) &&
                       captured.stamp.policyVersion == current.stamp.policyVersion && captured.enrollmentEpoch == current.enrollmentEpoch &&
                       captured.entityId == current.entityId && identities.Find(captured.entityId).has_value();
            },
            provider,
            [&](const auto& captured, const auto& features, const auto& admits, std::string& error)
            {
                if (beforeCommit)
                    beforeCommit();
                if (!admits())
                    return false;
                ++commits;
                if (!identities.GrantRecognitionConsent(captured.entityId, error) || !admits())
                    return false;
                const auto revision = identities.RecognitionConsentRevision(captured.entityId);
                if (!revision || !recognition.Enroll(captured.entityId, *revision, features, error) || !admits())
                    return false;
                return identities.Save(error);
            }};
    }
};

void Arm(speech::SpeakerEnrollmentDialogue& dialogue, const speech::SpeakerEnrollmentContext& context)
{
    Check(dialogue.HandleTyped("remember my voice", context).phase == SpeakerEnrollmentPhase::AwaitingConsent,
        "Direct typed request did not create a consent request.");
    Check(dialogue.HandleTyped("I consent to local speaker recognition only", context).phase == SpeakerEnrollmentPhase::AwaitingSample,
        "Explicit recognition-only consent did not arm the next transient sample.");
}

void TestVoiceAndUntrustedRequestsNeverArm()
{
    speech::SpeakerEnrollmentDialogue unconfigured;
    const auto voice = unconfigured.HandleVoiceRequest("Remember my voice.");
    Check(voice.handled && !voice.completed && unconfigured.Snapshot().phase == SpeakerEnrollmentPhase::Idle,
        "Voice request bypassed the typed/private consent boundary.");
    Check(!unconfigured.HandleVoiceRequest("She said remember my voice").handled, "Reported speech was treated as direct enrollment.");
    Check(!unconfigured.HandleTyped("Someone said 'remember my voice'", Context()).handled,
        "Quoted request was treated as direct enrollment.");
    Check(unconfigured.HandleTyped("remember my voice", Context()).phase == SpeakerEnrollmentPhase::Idle,
        "Absent trusted collaborators admitted enrollment.");
    Fixture fixture;
    speech::SpeakerEnrollmentDialogue dialogue(fixture.Callbacks());
    auto invalid = fixture.current;
    invalid.entityId = "unknown-person";
    Check(dialogue.HandleTyped("remember my voice", invalid).phase == SpeakerEnrollmentPhase::Idle,
        "A nonexistent person was implicitly selected or created.");
    invalid = fixture.current;
    invalid.enrollmentEpoch = 0;
    Check(dialogue.HandleTyped("remember my voice", invalid).phase == SpeakerEnrollmentPhase::Idle, "Missing purpose epoch was accepted.");
    fixture.allowed = false;
    Check(dialogue.HandleTyped("remember my voice", fixture.current).phase == SpeakerEnrollmentPhase::Idle,
        "Denied typed/private adapter armed enrollment.");
    auto throwingCallbacks = fixture.Callbacks();
    throwingCallbacks.admits = [](const auto&) -> bool { throw std::runtime_error("PRIVATE_ADMISSION_SENTINEL"); };
    speech::SpeakerEnrollmentDialogue throwing(throwingCallbacks);
    const auto denied = throwing.HandleTyped("remember my voice", fixture.current);
    Check(denied.phase == SpeakerEnrollmentPhase::Idle && denied.message.find("SENTINEL") == std::string::npos,
        "Admission exception leaked content or allowed a request.");
    Fixture withdrawal;
    auto provider = std::make_shared<ControlledProvider>();
    withdrawal.provider = provider;
    speech::SpeakerEnrollmentDialogue pending(withdrawal.Callbacks());
    Arm(pending, withdrawal.current);
    Check(!pending.HandleVoiceRequest("I consent to local speaker recognition only").completed &&
              pending.Snapshot().phase == SpeakerEnrollmentPhase::AwaitingSample,
        "Voice consent changed an already typed/private purpose.");
    const auto cancelled = pending.HandleVoiceRequest("cancel voice enrollment");
    Check(cancelled.handled && !cancelled.completed && pending.Snapshot().phase == SpeakerEnrollmentPhase::Idle,
        "An explicit voice cancellation left pending feature capture armed.");
    pending.CaptureNextSample("not-readable.wav");
    Check(provider->calls == 0 && withdrawal.commits == 0, "Voice withdrawal still read or saved a subsequent sample.");
}

void TestRealSampleNeedsSeparateConfirmationAndPersists()
{
    Fixture fixture;
    const auto wave = fixture.directory.root / "transient.wav";
    WriteWave(wave, 300);
    speech::SpeakerEnrollmentDialogue dialogue(fixture.Callbacks());
    Check(dialogue.CaptureNextSample(wave).phase == SpeakerEnrollmentPhase::Idle, "Unsolicited sample armed enrollment.");
    Check(dialogue.HandleTyped("remember my voice", fixture.current).phase == SpeakerEnrollmentPhase::AwaitingConsent,
        "Typed request failed.");
    Check(dialogue.HandleTyped("yes", fixture.current).phase == SpeakerEnrollmentPhase::AwaitingConsent,
        "Generic yes substituted for recognition-only consent.");
    Check(dialogue.CaptureNextSample(wave).phase == SpeakerEnrollmentPhase::AwaitingConsent, "Pre-consent sample was captured.");
    Check(dialogue.HandleTyped("I consent to local speaker recognition only", fixture.current).phase ==
              SpeakerEnrollmentPhase::AwaitingSample,
        "Recognition-only consent failed.");
    auto laterTurn = fixture.current;
    laterTurn.stamp.taskId = "turn-two";
    laterTurn.stamp.attemptId = "attempt-two";
    const auto sample = dialogue.CaptureNextSample(wave);
    Check(sample.handled && sample.phase == SpeakerEnrollmentPhase::AwaitingConfirmation && fixture.commits == 0 &&
              fixture.identities.SpeakerTemplates().empty(),
        "Extraction committed a template without a separate typed confirmation.");
    Check(!dialogue.HandleVoiceRequest("confirm this voice sample").completed && fixture.commits == 0,
        "A recognition guess or voice transcript confirmed enrollment.");
    Check(dialogue.HandleTyped("confirm this voice sample", laterTurn).completed && fixture.commits == 1,
        "Separately confirmed same-session sample did not commit through the identity owner.");
    Check(dialogue.Snapshot().phase == SpeakerEnrollmentPhase::Idle, "Completed enrollment retained pending features.");
    std::string error;
    identity::RelationshipRegistry restored(fixture.directory.root / "identity.json");
    Check(restored.Load(error) && restored.SpeakerTemplates().size() == 1, "Confirmed enrollment did not survive identity restart.");
    speech::SpeakerRecognition recognition(restored);
    Check(recognition.Observe(wave, restored.RecognitionConsentRevisions()).entityId == fixture.current.entityId,
        "Real consented WAV did not match its persisted template.");
    std::filesystem::remove(wave);
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fixture.directory.root))
        files += entry.is_regular_file();
    Check(files == 1, "Enrollment helper retained a raw WAV or second identity store.");
}

void TestExpiryIsBoundedAndDuplicateDoesNotExtendIt()
{
    Fixture fixture;
    auto provider = std::make_shared<ControlledProvider>();
    fixture.provider = provider;
    speech::SpeakerEnrollmentDialogue dialogue(fixture.Callbacks());
    const auto start = speech::SpeakerEnrollmentDialogue::Clock::now();
    dialogue.HandleTyped("remember my voice", fixture.current, start);
    const auto deadline = dialogue.Snapshot(start).expiresAt;
    dialogue.HandleTyped("remember my voice", fixture.current, start + 30s);
    Check(dialogue.Snapshot(start + 30s).expiresAt == deadline && deadline <= start + 2min,
        "Duplicate request extended or unbounded the pending consent lifetime.");
    dialogue.HandleTyped("I consent to local speaker recognition only", fixture.current, start + 31s);
    Check(dialogue.CaptureNextSample("not-readable.wav", {}, start + 2min).phase == SpeakerEnrollmentPhase::Idle && provider->calls == 0,
        "Expired purpose still read the transient sample.");
    Check(!dialogue.HandleTyped("confirm this voice sample", fixture.current, start + 2min).completed, "Expired sample was confirmed.");
}

void TestCancellationDuringExtractionDoesNotRestamp()
{
    Fixture fixture;
    auto provider = std::make_shared<ControlledProvider>();
    fixture.provider = provider;
    speech::SpeakerEnrollmentDialogue dialogue(fixture.Callbacks());
    Arm(dialogue, fixture.current);
    provider->duringExtract = [&]
    {
        dialogue.Cancel();
        ++fixture.current.enrollmentEpoch;
        Arm(dialogue, fixture.current);
    };
    const auto result = dialogue.CaptureNextSample("transient-owned-by-caller.wav");
    Check(!result.completed && dialogue.Snapshot().phase == SpeakerEnrollmentPhase::AwaitingSample && fixture.commits == 0,
        "Cancelled extraction populated a newly armed purpose with an old sample.");
    provider->duringExtract = [&] { fixture.allowed = false; };
    dialogue.CaptureNextSample("transient-owned-by-caller.wav");
    Check(dialogue.Snapshot().phase == SpeakerEnrollmentPhase::Idle, "Revocation during extraction retained pending features.");
    fixture.allowed = true;
    std::stop_source stop;
    provider->duringExtract = [&] { stop.request_stop(); };
    Arm(dialogue, fixture.current);
    dialogue.CaptureNextSample("transient-owned-by-caller.wav", stop.get_token());
    Check(dialogue.Snapshot().phase == SpeakerEnrollmentPhase::Idle && fixture.commits == 0,
        "Cancellation during extraction retained features or committed a template.");
}

void TestProviderFailuresAreSafeAndAttemptsBounded()
{
    Fixture fixture;
    auto provider = std::make_shared<ControlledProvider>();
    fixture.provider = provider;
    speech::SpeakerEnrollmentDialogue dialogue(fixture.Callbacks());
    Arm(dialogue, fixture.current);
    provider->throws = true;
    const auto first = dialogue.CaptureNextSample("PRIVATE_PATH_SENTINEL.wav");
    Check(first.phase == SpeakerEnrollmentPhase::AwaitingSample && first.message.find("SENTINEL") == std::string::npos,
        "Provider exception leaked diagnostic content or prevented bounded retry.");
    provider->throws = false;
    provider->result->values[0] = std::numeric_limits<float>::quiet_NaN();
    const auto second = dialogue.CaptureNextSample("PRIVATE_PATH_SENTINEL.wav");
    Check(second.phase == SpeakerEnrollmentPhase::Idle && second.message.find("SENTINEL") == std::string::npos,
        "Invalid features escaped or more than two sample attempts were retained.");
    dialogue.CaptureNextSample("PRIVATE_PATH_SENTINEL.wav");
    Check(provider->calls == 2 && fixture.commits == 0, "Exhausted enrollment started a retry storm or committed invalid features.");
    Arm(dialogue, fixture.current);
    provider->result->values[0] = 1;
    provider->result->version = 99;
    Check(dialogue.CaptureNextSample("transient.wav").phase == SpeakerEnrollmentPhase::AwaitingSample,
        "Unknown feature version was accepted.");
    dialogue.Cancel();
}

void TestDuplicateSampleAndConfirmationCommitOnce()
{
    Fixture fixture;
    auto provider = std::make_shared<ControlledProvider>();
    fixture.provider = provider;
    std::mutex gateMutex;
    std::condition_variable gate;
    bool entered = false, release = false;
    provider->duringExtract = [&]
    {
        std::unique_lock lock(gateMutex);
        entered = true;
        gate.notify_all();
        gate.wait(lock, [&] { return release; });
    };
    speech::SpeakerEnrollmentDialogue dialogue(fixture.Callbacks());
    Arm(dialogue, fixture.current);
    std::jthread capture([&] { dialogue.CaptureNextSample("transient.wav"); });
    {
        std::unique_lock lock(gateMutex);
        Check(gate.wait_for(lock, 5s, [&] { return entered; }), "Controlled extraction did not enter.");
    }
    dialogue.CaptureNextSample("second-transient.wav");
    {
        std::lock_guard lock(gateMutex);
        release = true;
    }
    gate.notify_all();
    capture.join();
    Check(provider->calls == 1, "Concurrent sample duplicated extraction.");
    fixture.beforeCommit = [&]
    {
        Check(dialogue.Snapshot().phase == SpeakerEnrollmentPhase::Committing,
            "Commit callback ran under the helper mutex or before commit ownership.");
        Check(!dialogue.HandleTyped("confirm this voice sample", fixture.current).completed,
            "Reentrant confirmation committed a second template.");
    };
    Check(dialogue.HandleTyped("confirm this voice sample", fixture.current).completed && fixture.commits == 1,
        "Actual owned confirmation did not commit exactly once.");
    Check(!dialogue.HandleTyped("confirm this voice sample", fixture.current).completed && fixture.commits == 1,
        "Duplicate completed confirmation replayed persistence.");
}

void TestContextChangesCannotConfirmOldFeatures()
{
    for (unsigned change = 0; change < 4; ++change)
    {
        Fixture fixture;
        fixture.provider = std::make_shared<ControlledProvider>();
        speech::SpeakerEnrollmentDialogue dialogue(fixture.Callbacks());
        Arm(dialogue, fixture.current);
        dialogue.CaptureNextSample("transient.wav");
        if (change == 0)
            ++fixture.current.enrollmentEpoch;
        else if (change == 1)
            ++fixture.current.stamp.policyVersion;
        else if (change == 2)
            ++fixture.current.stamp.generation;
        else
        {
            fixture.current.entityId = "other-selected-person";
            (void)fixture.identities.Get(fixture.current.entityId);
        }
        Check(!dialogue.HandleTyped("confirm this voice sample", fixture.current).completed && fixture.commits == 0 &&
                  dialogue.Snapshot().phase == SpeakerEnrollmentPhase::Idle,
            "Changed audience purpose, authority, session or selected entity confirmed old features.");
    }
}

void TestCommitAdmissionAndErrorsDoNotClaimSuccess()
{
    Fixture fixture;
    fixture.provider = std::make_shared<ControlledProvider>();
    speech::SpeakerEnrollmentDialogue dialogue(fixture.Callbacks());
    Arm(dialogue, fixture.current);
    dialogue.CaptureNextSample("transient.wav");
    fixture.beforeCommit = [&] { dialogue.Cancel(); };
    Check(!dialogue.HandleTyped("confirm this voice sample", fixture.current).completed && fixture.commits == 0 &&
              fixture.identities.SpeakerTemplates().empty(),
        "Cancellation in owner callback passed live admission or claimed persistence.");
    auto callbacks = fixture.Callbacks();
    callbacks.commit = [](const auto&, const auto&, const auto&, std::string&) -> bool
    { throw std::runtime_error("PRIVATE_COMMIT_ERROR_SENTINEL"); };
    speech::SpeakerEnrollmentDialogue throwing(callbacks);
    Arm(throwing, fixture.current);
    throwing.CaptureNextSample("transient.wav");
    const auto failure = throwing.HandleTyped("confirm this voice sample", fixture.current);
    Check(!failure.completed && failure.phase == SpeakerEnrollmentPhase::Idle && failure.message.find("SENTINEL") == std::string::npos,
        "Commit exception leaked content, retained features or claimed enrollment.");
}

void TestExplicitReplacementUsesIndependentlyConfirmedSample()
{
    Fixture fixture;
    const auto first = fixture.directory.root / "first.wav", second = fixture.directory.root / "second.wav";
    WriteWave(first, 300);
    WriteWave(second, 1400);
    speech::SpeakerEnrollmentDialogue dialogue(fixture.Callbacks());
    Arm(dialogue, fixture.current);
    dialogue.CaptureNextSample(first);
    Check(dialogue.HandleTyped("confirm this voice sample", fixture.current).completed, "Initial enrollment failed.");
    const auto original = fixture.identities.SpeakerTemplates().front().features.values;
    Check(fixture.recognition.Observe(first, fixture.identities.RecognitionConsentRevisions()).entityId == fixture.current.entityId,
        "The controlled consented sample did not produce an observation.");
    Check(fixture.identities.SpeakerTemplates().front().features.values == original, "Recognition reinforced its own guessed label.");
    Check(dialogue.HandleTyped("update my voice sample", fixture.current).phase == SpeakerEnrollmentPhase::AwaitingConsent,
        "Explicit replacement did not require fresh consent.");
    dialogue.HandleTyped("I consent to local speaker recognition only", fixture.current);
    dialogue.CaptureNextSample(second);
    Check(fixture.identities.SpeakerTemplates().front().features.values == original,
        "Unconfirmed replacement changed the persisted template.");
    Check(dialogue.HandleTyped("confirm this voice sample", fixture.current).completed && fixture.commits == 2,
        "Independent explicitly confirmed replacement failed.");
    Check(fixture.identities.SpeakerTemplates().front().features.values != original,
        "Replacement retained the original derived features instead of the independently sampled features.");
    std::string error;
    fixture.identities.RevokeRecognitionConsent(fixture.current.entityId, error);
    dialogue.Cancel();
    Check(fixture.recognition.Observe(second, fixture.identities.RecognitionConsentRevisions()).entityId.empty(),
        "Revoked recognition consent kept the replacement usable.");
}
} // namespace

void RunSpeakerEnrollmentDialogueTests()
{
    TestVoiceAndUntrustedRequestsNeverArm();
    TestRealSampleNeedsSeparateConfirmationAndPersists();
    TestExpiryIsBoundedAndDuplicateDoesNotExtendIt();
    TestCancellationDuringExtractionDoesNotRestamp();
    TestProviderFailuresAreSafeAndAttemptsBounded();
    TestDuplicateSampleAndConfirmationCommitOnce();
    TestContextChangesCannotConfirmOldFeatures();
    TestCommitAdmissionAndErrorsDoNotClaimSuccess();
    TestExplicitReplacementUsesIndependentlyConfirmedSample();
    std::cout << "Speaker enrollment dialogue tests passed (9 fixtures; controlled WAVs, no physical accuracy/authentication claim).\n";
}
