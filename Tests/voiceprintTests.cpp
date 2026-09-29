#include "testSupport.h"
#include "reviaSessionTestAccess.h"

#include "Identity/relationshipRegistry.h"
#include "Identity/voiceprintRegistry.h"
#include "Speech/speakerEmbeddingClient.h"
#include "Speech/speechRecognitionService.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

// Whose voice this is, when they asked her to learn it.
//
// A voiceprint is kept only at its owner's request and dropped at their word; a match
// is a cosine score against the samples kept, above a threshold and clear of the next
// print; it moves conversation attribution exactly as a stated name does and nothing
// else. The worker's client takes one WAV and returns one unit vector.
namespace
{
using namespace std::chrono_literals;
using revia::identity::AsksToEnrollVoice;
using revia::identity::AsksToForgetVoice;
using revia::identity::LocalUserEntityId;
using revia::identity::ReadVoiceEnrollmentName;
using revia::identity::SpeakerMatch;
using revia::identity::VoiceprintRegistry;
using revia::runtime::ReviaSession;
using revia::speech::RecognitionEvent;
using revia::speech::SpeakerEmbeddingClient;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using Access = revia::runtime::ReviaSessionTestAccess;
using json = nlohmann::json;

// A voice as a direction in sixteen dimensions, with a little noise per utterance.
std::vector<float> Voice(const int axis, const float noise = 0.0F, const int seed = 1)
{
    std::vector<float> embedding(16, 0.0F);
    embedding[static_cast<std::size_t>(axis)] = 1.0F;
    for (std::size_t index = 0; index < embedding.size(); ++index)
    {
        embedding[index] += noise * static_cast<float>(std::sin(0.7 * static_cast<double>(index + 1) * seed));
    }
    return embedding;
}

void TestAPrintIsKeptOnRequestAndMatchedWithAMargin()
{
    VoiceprintRegistry registry(0.62F, 0.08F, 5);
    std::string error;
    Check(registry.Enroll("local:sam", "Sam", std::vector<float>{1.0F, 2.0F}, "2026-09-29", error) == 0 &&
        registry.Enroll("local:sam", "Sam", std::vector<float>(16, 0.0F), "2026-09-29", error) == 0 &&
        registry.Enroll("", "Sam", Voice(0), "2026-09-29", error) == 0,
        "A short, empty or ownerless embedding was kept.");
    Check(!registry.Match(Voice(0)).matched && registry.Match(Voice(0)).reason == "no voice is enrolled",
        "A match was found with nothing enrolled.");
    Check(registry.Enroll("local:sam", "Sam", Voice(0, 0.05F, 1), "2026-09-29", error) == 1 &&
        registry.Enroll("local:sam", "Sam", Voice(0, 0.05F, 2), "2026-09-29", error) == 2,
        "Enrolment did not count samples: " + error);
    const SpeakerMatch sam = registry.Match(Voice(0, 0.08F, 3));
    Check(sam.matched && sam.entityId == "local:sam" && sam.displayName == "Sam" && sam.score > 0.9F,
        "Sam's own voice was not matched: " + sam.reason);
    const SpeakerMatch stranger = registry.Match(Voice(1, 0.05F, 4));
    Check(!stranger.matched && stranger.nearest == "Sam" && stranger.score < 0.3F &&
        stranger.reason.find("below") != std::string::npos,
        "A stranger was matched, or the refusal did not say who was nearest: " + stranger.reason);
    Check(registry.Enroll("local:ana", "Ana", Voice(1, 0.05F, 5), "2026-09-29", error) == 1, "Ana was not kept.");
    Check(registry.Match(Voice(1, 0.05F, 6)).entityId == "local:ana", "Ana's voice went to Sam.");
    // Halfway between the two: above the threshold for neither, and even if it were,
    // not clear of the other.
    std::vector<float> between = Voice(0);
    between[1] = 1.0F;
    const SpeakerMatch ambiguous = registry.Match(between);
    Check(!ambiguous.matched, "A voice between two prints was attributed: " + ambiguous.reason);
    registry.Configure(0.5F, 0.3F);
    const SpeakerMatch close = registry.Match(between);
    Check(!close.matched && close.reason.find("ahead of the next") != std::string::npos,
        "A voice above the threshold but not clear of the next print was attributed: " + close.reason);

    for (int extra = 0; extra < 6; ++extra)
    {
        registry.Enroll("local:sam", "Sam", Voice(0, 0.05F, 10 + extra), "2026-09-29", error);
    }
    Check(registry.All()[0].samples.size() == 5, "Samples did not roll at five.");
    const std::string serialized = registry.Serialize();
    VoiceprintRegistry restored;
    Check(restored.Deserialize(serialized, error) && restored.Count() == 2 &&
        restored.Match(Voice(0, 0.05F, 20)).entityId == "local:sam" &&
        restored.All()[0].consentedAt == "2026-09-29",
        "The registry did not round-trip: " + error);
    Check(!restored.Deserialize("nope", error) && !restored.Deserialize(R"({"x":1})", error),
        "Broken voiceprints were accepted.");
    Check(registry.Forget("local:sam") && !registry.Forget("local:sam") && registry.Count() == 1 &&
        !registry.Match(Voice(0)).matched,
        "Forgetting did not remove the print.");
    Check(registry.Describe().find("Ana (local:ana): 1 sample") != std::string::npos,
        "The description did not list the kept voice.");
    Check(AsksToEnrollVoice("Revia, remember my voice, I'm Sam") && AsksToEnrollVoice("please learn my voice") &&
        !AsksToEnrollVoice("remember my birthday") && AsksToForgetVoice("forget my voice please") &&
        !AsksToForgetVoice("forget my name"),
        "The words that ask for a voice to be kept or forgotten were not recognised.");
    Check(ReadVoiceEnrollmentName("Revia, remember my voice, I'm Sam") == "Sam" &&
        ReadVoiceEnrollmentName("remember my voice, my name is Ana") == "Ana" &&
        ReadVoiceEnrollmentName("Learn my voice. This is Quentin.") == "Quentin" &&
        ReadVoiceEnrollmentName("remember my voice, I'm tired of typing").empty() &&
        ReadVoiceEnrollmentName("remember my voice").empty(),
        "The name in a request to keep a voice was not read as a transcript writes it.");
}

// The speaker worker as the client sees it.
class FakeWorker
{
public:
    FakeWorker()
    {
        server.Get("/health", [](const auto& request, auto& response)
        {
            if (request.get_header_value("Authorization") != "Bearer k")
            {
                response.status = 401;
                return;
            }
            response.set_content(R"({"status":"ok","backend":"sherpa-onnx"})", "application/json");
        });
        server.Post("/v1/speaker/embed", [this](const auto& request, auto& response)
        {
            const json body = json::parse(request.body);
            {
                std::lock_guard lock(mutex);
                lastPath = body.value("wav_path", "");
            }
            if (lastPath.find("short") != std::string::npos)
            {
                response.status = 400;
                response.set_content(R"({"succeeded":false,"message":"The utterance is shorter than half a second."})", "application/json");
                return;
            }
            response.set_content(json{{"succeeded", true}, {"embedding", Voice(0, 0.05F, 7)}, {"dimension", 16}}.dump(),
                "application/json");
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the speaker worker fixture.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        Check(server.is_running(), "The speaker worker fixture did not start.");
    }
    ~FakeWorker() { server.stop(); thread.join(); }
    std::string LastPath() { std::lock_guard lock(mutex); return lastPath; }
    int port = 0;
private:
    httplib::Server server;
    std::mutex mutex;
    std::string lastPath;
    std::jthread thread;
};

void TestTheClientTakesOneWavAndReturnsOneVector()
{
    FakeWorker worker;
    SpeakerEmbeddingClient client;
    std::string error;
    Check(!client.Embed("x.wav", error) && error == "Speaker identification is off.",
        "An unconfigured client tried to embed.");
    client.UseEndpoint("127.0.0.1", worker.port, "k");
    const auto embedding = client.Embed(std::filesystem::path("utterance.wav"), error);
    Check(embedding && embedding->size() == 16 && (*embedding)[0] > 0.9F, "The embedding did not come back: " + error);
    Check(worker.LastPath().find("utterance.wav") != std::string::npos &&
        std::filesystem::path(worker.LastPath()).is_absolute(),
        "The worker was not given the WAV's absolute path.");
    Check(!client.Embed(std::filesystem::path("short.wav"), error) && error.find("half a second") != std::string::npos,
        "The worker's refusal was not reported.");
    SpeakerEmbeddingClient wrongKey;
    wrongKey.UseEndpoint("127.0.0.1", worker.port, "wrong");
    Check(!wrongKey.Embed(std::filesystem::path("utterance.wav"), error) && error.find("did not answer") != std::string::npos,
        "A worker that refuses the key was used.");
    SpeakerEmbeddingClient nobody;
    nobody.UseEndpoint("127.0.0.1", 1, "k");
    Check(!nobody.Embed(std::filesystem::path("utterance.wav"), error), "An absent worker was used.");
}

RecognitionEvent HeardVoice(const std::string& transcript, std::vector<float> embedding)
{
    RecognitionEvent event{"Transcript", "", transcript};
    event.automatic = true;
    event.speakerEmbedding = std::move(embedding);
    return event;
}

void TestASessionKeepsAVoiceOnRequestAndFollowsIt()
{
    ScopedTestDirectory directory;
    ReviaSession session;
    Check(Access::CurrentSpeaker(session) == LocalUserEntityId(), "The session did not start with the local user.");
    Access::Hear(session, HeardVoice("Revia, what time is it?", Voice(0, 0.05F, 1)));
    Check(Access::Voiceprints(session).Count() == 0 && Access::CurrentSpeaker(session) == LocalUserEntityId(),
        "A voice was kept without anyone asking.");

    Access::Hear(session, HeardVoice("Revia, remember my voice, I'm Sam", Voice(0, 0.05F, 2)));
    Check(Access::Voiceprints(session).Count() == 1 && Access::CurrentSpeaker(session) == "local:sam",
        "Asking her to remember a voice with a name did not keep it under that name.");
    Access::Hear(session, HeardVoice("remember my voice", Voice(0, 0.05F, 3)));
    Check(Access::Voiceprints(session).All()[0].samples.size() == 2,
        "A second request from the same person did not add a sample to their print.");

    Access::Hear(session, HeardVoice("Revia, hello again", Voice(0, 0.08F, 4)));
    Check(Access::CurrentSpeaker(session) == "local:sam", "Sam's own voice moved attribution away from Sam.");
    Access::Hear(session, HeardVoice("Revia, who am I?", Voice(1, 0.05F, 5)));
    Check(Access::CurrentSpeaker(session) == LocalUserEntityId(),
        "A stranger's voice was still attributed to Sam.");
    Access::Hear(session, HeardVoice("Revia, it's me", Voice(0, 0.05F, 6)));
    Check(Access::CurrentSpeaker(session) == "local:sam", "Sam coming back was not recognised.");
    Access::Hear(session, HeardVoice("forget my voice", Voice(0, 0.05F, 7)));
    Check(Access::Voiceprints(session).Count() == 0, "Asking her to forget the voice did not drop it.");
}
} // namespace

void RunVoiceprintTests()
{
    TestAPrintIsKeptOnRequestAndMatchedWithAMargin();
    TestTheClientTakesOneWavAndReturnsOneVector();
    TestASessionKeepsAVoiceOnRequestAndFollowsIt();
    std::cout << "A voice is kept only at its owner's request, matched with a margin, follows them "
                 "at the microphone, and is dropped at their word.\n";
}
