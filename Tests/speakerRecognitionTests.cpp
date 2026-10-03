#include "Speech/speakerRecognition.h"
#include "Identity/relationshipRegistry.h"
#include "speakerRecognitionTestAccess.h"
#include "testSupport.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <numbers>
#include <thread>

namespace
{
using namespace revia;
using tests::Check;
using namespace std::chrono_literals;

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

void TestLocalWaveFeaturesAndNoConsentRead()
{
    tests::ScopedTestDirectory directory;
    const auto first = directory.root / "first.wav", second = directory.root / "second.wav";
    WriteWave(first, 300);
    WriteWave(second, 1400);
    speech::WaveSpeakerRecognitionProvider provider;
    std::string error;
    const auto one = provider.Extract(first, {}, error), two = provider.Extract(second, {}, error);
    Check(one && two, "Actual bounded PCM feature extraction failed.");
    double dot = 0;
    for (std::size_t index = 0; index < speech::SpeakerFeatureCount; ++index)
        dot += one->values[index] * two->values[index];
    Check(dot < 0.9, "Controlled distinct tones did not produce distinct local features.");
    identity::RelationshipRegistry registry(directory.root / "identity.json");
    registry.Get("sam");
    speech::SpeakerRecognition recognition(registry);
    Check(
        recognition.Observe(directory.root / "not-readable.wav", {}).entityId.empty(), "No-consent recognition produced a guessed person.");
    Check(registry.GrantRecognitionConsent("sam", error) &&
              recognition.Enroll("sam", *registry.RecognitionConsentRevision("sam"), first, error),
        "Actual transient WAV enrollment failed.");
    Check(recognition.Observe(first, registry.RecognitionConsentRevisions()).entityId == "sam",
        "Actual consented transient WAV did not match its template.");
    std::stop_source cancelled;
    cancelled.request_stop();
    Check(!provider.Extract(first, cancelled.get_token(), error), "Cancelled extraction still returned features.");
    Check(registry.SpeakerTemplates().size() == 1, "Matching or cancellation refined an enrollment template.");
}

class RevokingProvider final : public speech::ISpeakerRecognitionProvider
{
  public:
    explicit RevokingProvider(identity::RelationshipRegistry& registry) : registry(registry)
    {
    }
    std::optional<speech::SpeakerFeatures> Extract(const std::filesystem::path&, std::stop_token, std::string&) const override
    {
        std::string error;
        registry.RevokeRecognitionConsent("sam", error);
        speech::SpeakerFeatures features;
        features.values[0] = 1;
        return features;
    }
    identity::RelationshipRegistry& registry;
};

void TestRevokeDuringProviderCannotEnrollOrMatch()
{
    tests::ScopedTestDirectory directory;
    identity::RelationshipRegistry registry(directory.root / "identity.json");
    registry.Get("sam");
    std::string error;
    registry.GrantRecognitionConsent("sam", error);
    const auto revision = *registry.RecognitionConsentRevision("sam");
    speech::SpeakerRecognition recognition(registry, std::make_shared<RevokingProvider>(registry));
    Check(!recognition.Enroll("sam", revision, directory.root / "transient.wav", error),
        "Revoked in-flight enrollment published a template.");
    Check(registry.SpeakerTemplates().empty(), "Failed enrollment retained voice features.");
}

void TestResolverOriginAndCancellationOnTransientSample()
{
    tests::ScopedTestDirectory directory;
    const auto wave = directory.root / "transient.wav";
    WriteWave(wave, 300);
    bool sawSample = false;
    speech::SpeechRecognitionService service;
    service.SetSpeakerResolver(
        [&](const auto& path, std::stop_token)
        {
            sawSample = std::filesystem::exists(path);
            // Reentry proves that the provider callback runs outside the service mutex.
            service.SetSpeakerResolver({});
            return identity::SpeakerObservation{"sam", identity::SpeakerSource::ConsentedVoice, "fixture-origin", 2, 0.99F};
        });
    const auto known = speech::SpeakerRecognitionTestAccess::Resolve(service, wave);
    Check(sawSample && known.entityId == "sam" && known.observationId == "fixture-origin",
        "Owner resolver lost transient sample or evidence origin.");
    std::stop_source cancelled;
    service.SetSpeakerResolver(
        [&](const auto&, std::stop_token)
        {
            cancelled.request_stop();
            return identity::SpeakerObservation{"sam"};
        });
    Check(speech::SpeakerRecognitionTestAccess::Resolve(service, wave, cancelled.get_token()).entityId.empty(),
        "Cancelled provider returned a guessed person.");
    service.SetSpeakerResolver(
        [](const auto&, std::stop_token) -> identity::SpeakerObservation { throw std::runtime_error("private fixture payload"); });
    Check(
        speech::SpeakerRecognitionTestAccess::Resolve(service, wave).entityId.empty(), "Provider exception escaped the unknown boundary.");
    std::filesystem::remove(wave);
    Check(!std::filesystem::exists(wave), "Disposable fixture audio removal failed.");
}

}

void RunSpeakerRecognitionTests()
{
    TestLocalWaveFeaturesAndNoConsentRead();
    TestRevokeDuringProviderCannotEnrollOrMatch();
    TestResolverOriginAndCancellationOnTransientSample();
    std::cout << "Speaker recognition tests passed (3 controlled audio/provider fixtures; no physical accuracy claim).\n";
}
