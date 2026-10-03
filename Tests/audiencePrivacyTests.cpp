#include "Runtime/conversationRuntime.h"

#include "Emotion/emotionModel.h"
#include "Identity/developmentState.h"
#include "Identity/preferenceState.h"
#include "testSupport.h"

#include <chrono>
#include <atomic>
#include <filesystem>
#include <httplib.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using namespace revia;
using namespace revia::runtime;
using tests::Check;
using json = nlohmann::json;

constexpr const char* PrivatePreference = "private_preference_fixture_7ec2";
constexpr const char* PrivateCause = "private_cause_fixture_a532";
constexpr const char* AuthoredCharacter = "AUTHORED_CHARACTER_FIXTURE_84F0";
constexpr const char* AuthoredPreference = "authored astronomy";

class WorkingDirectory
{
  public:
    explicit WorkingDirectory(const std::filesystem::path& directory) : previous(std::filesystem::current_path())
    {
        std::filesystem::current_path(directory);
    }

    ~WorkingDirectory()
    {
        std::filesystem::current_path(previous);
    }

  private:
    std::filesystem::path previous;
};

class Backend
{
  public:
    Backend()
    {
        server.Get(
            "/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        server.Get("/v1/models",
            [](const auto&, auto& response) { response.set_content(R"({"data":[{"id":"audience-fixture-main"}]})", "application/json"); });
        server.Get("/props", [](const auto&, auto& response)
            { response.set_content(R"({"total_slots":1,"default_generation_settings":{"n_ctx":8192}})", "application/json"); });
        server.Post("/v1/chat/completions",
            [this](const auto& request, auto& response)
            {
                const json body = json::parse(request.body);
                {
                    std::lock_guard lock(mutex);
                    requests.push_back(body);
                }
                if (onRequest)
                    onRequest();
                const std::string answer = body.contains("response_format") ? R"({"verdict":"allow","reason":"Controlled fixture review."})"
                                                                            : "A static library contains object files. The linker includes "
                                                                              "referenced objects and resolves their external symbols.";
                const json choice = {{"message", {{"role", "assistant"}, {"content", answer}}}, {"finish_reason", "stop"}};
                if (body.value("stream", false))
                {
                    const json chunk = {{"choices", json::array({{{"delta", {{"content", answer}}}, {"finish_reason", "stop"}}})}};
                    response.set_content("data: " + chunk.dump() + "\n\ndata: [DONE]\n\n", "text/event-stream");
                }
                else
                {
                    response.set_content(json{{"choices", json::array({choice})}}.dump(), "application/json");
                }
            });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "The audience fixture could not bind its loopback backend.");
        listener = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
        if (!server.is_running())
        {
            server.stop();
            if (listener.joinable())
                listener.join();
            Check(false, "The audience fixture loopback backend did not start.");
        }
    }

    ~Backend()
    {
        server.stop();
        if (listener.joinable())
            listener.join();
    }

    std::vector<json> Requests() const
    {
        std::lock_guard lock(mutex);
        return requests;
    }

    int port = 0;
    std::function<void()> onRequest;

  private:
    httplib::Server server;
    mutable std::mutex mutex;
    std::vector<json> requests;
    std::jthread listener;
};

class FixtureEmotionModel final : public emotion::IEmotionModel
{
  public:
    emotion::EmotionVector Evaluate(const emotion::Stimulus& stimulus, const emotion::AppraisalContext&) const override
    {
        emotion::EmotionVector result;
        if (stimulus.eventType == "audience_privacy_fixture")
        {
            result[emotion::Emotion::Irritation] = 0.7F;
        }
        return result;
    }

    std::string Name() const override
    {
        return "audience-fixture-rule";
    }
};

class Fixture
{
  public:
    explicit Fixture(const bool reviewEnabled)
        : cwd(directory.root), router((directory.root / "router.db").string()), coordinator((directory.root / "agent.db").string()),
          emotions(std::make_unique<FixtureEmotionModel>()), log(directory.root / "Logs"),
          runtime(
              router, context, coordinator, speech, affect, emotions, events, log, [](RuntimeState, const std::string&) {},
              [](const AffectSnapshot&) {}, {}, {}, {},
              [reviewEnabled]
              {
                  responseFilterSettings filters;
                  filters.bAiReviewEnabled = reviewEnabled;
                  return filters;
              },
              [] { return "private_screen_fixture_781a"; },
              []
              {
                  identity::RelationshipState privatePerson;
                  privatePerson.entityId = "local:private-person";
                  privatePerson.displayName = "private_person_fixture_ae74";
                  privatePerson.interactionCount = 5;
                  return privatePerson;
              },
              [] { return identity::DevelopmentState{}; }, {}, {},
              []
              {
                  identity::Preference privatePreference;
                  privatePreference.subject = PrivatePreference;
                  privatePreference.strength = 0.8F;
                  privatePreference.confidence = 0.9F;
                  privatePreference.evidenceCount = 8;
                  privatePreference.source = identity::PreferenceSource::Observed;
                  return std::vector<identity::Preference>{privatePreference};
              })
    {
        profile.id = "audience-fixture";
        profile.displayName = "Fixture Revia";
        profile.systemPrompt = std::string("You are Revia. Preserve authored character: ") + AuthoredCharacter + ".";
        profile.bMemoryEnabled = false;
        profile.preferences.emplace_back(AuthoredPreference, 0.8F);
        llmSettings settings;
        settings.port = backend.port;
        settings.modelName = "audience-fixture-main";
        settings.bAutoStartServer = false;
        settings.bVisionEnabled = false;
        settings.bAutoMaxTokens = false;
        settings.maxTokens = 256;
        embeddingSettings embeddings;
        embeddings.bEnabled = false;
        embeddings.bAutoStartServer = false;
        router.ApplyLLMSettings(settings, embeddings, profile);
        emotion::Stimulus stimulus;
        stimulus.eventType = "audience_privacy_fixture";
        stimulus.description = PrivateCause;
        stimulus.importance = 0.9F;
        stimulus.valence = -0.8F;
        Check(emotions.Observe(stimulus, {}).has_value() && emotions.Current().cause == PrivateCause,
            "The synthetic private emotion cause was not established.");
    }

    SessionResult ReplyPublic()
    {
        identity::RelationshipState viewer;
        viewer.entityId = "adapter:fixture:viewer";
        return runtime.ReplyPublic("Explain how linking a static library resolves symbols.", {},
            "This is a public fixture conversation. Do not disclose private state.", viewer, profile, true, false);
    }

    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd;
    Backend backend;
    messageRouter router;
    conversationContext context;
    agents::TurnCoordinator coordinator;
    speech::SpeechService speech;
    AffectController affect;
    emotion::EmotionRuntime emotions;
    RuntimeEventBus events;
    logger log;
    ConversationRuntime runtime;
    aiProfile profile;
};

void TestPublicRequestMinimizesPrivateState()
{
    Fixture fixture(false);
    const auto reply = fixture.ReplyPublic();
    Check(reply.succeeded, "The actual public fixture reply failed: " + reply.reason);
    const auto requests = fixture.backend.Requests();
    Check(!requests.empty(), "The public fixture did not reach the real request transport.");
    for (const auto& request : requests)
    {
        const std::string wire = request.dump();
        Check(wire.find(PrivatePreference) == std::string::npos,
            "The actual public outbound request contains a companion-private earned preference sentinel.");
        Check(
            wire.find(PrivateCause) == std::string::npos, "The actual public outbound request contains a private emotion cause sentinel.");
        Check(
            wire.find("private_screen_fixture_781a") == std::string::npos && wire.find("private_person_fixture_ae74") == std::string::npos,
            "The public request inherited private screen or speaker context.");
    }
    const std::string wire = requests.front().dump();
    Check(wire.find(AuthoredCharacter) != std::string::npos && wire.find(AuthoredPreference) != std::string::npos,
        "Audience projection removed authored public character or declared preference.");
    Check(wire.find("voice") != std::string::npos, "Audience projection removed safe voice health facts.");
}

void TestPrivateRequestRetainsApprovedState()
{
    Fixture fixture(false);
    const auto reply = fixture.runtime.Reply("Explain how linking a static library resolves symbols.", fixture.profile, true, false);
    Check(reply.succeeded, "The private positive-control reply failed: " + reply.reason);
    const auto requests = fixture.backend.Requests();
    Check(!requests.empty(), "The private positive control did not reach transport.");
    const std::string wire = requests.front().dump();
    Check(wire.find(PrivatePreference) != std::string::npos && wire.find(PrivateCause) != std::string::npos,
        "Audience projection globally erased legitimate private state.");
    Check(wire.find(AuthoredCharacter) != std::string::npos, "The private reply lost authored character.");
}

void TestPublicReviewRequestsRemainMinimized()
{
    Fixture fixture(true);
    const auto reply = fixture.ReplyPublic();
    Check(reply.succeeded, "The public review fixture reply failed: " + reply.reason);
    const auto requests = fixture.backend.Requests();
    Check(requests.size() >= 2, "The optional review path was not exercised through the actual provider.");
    for (const auto& request : requests)
    {
        const std::string wire = request.dump();
        Check(wire.find(PrivatePreference) == std::string::npos && wire.find(PrivateCause) == std::string::npos,
            "A public final/meta/review request contains private state.");
    }
}

void TestAudienceRoutesMinimizeAndKeepTrustedPrivateReference()
{
    for (const auto kind : {identity::AudienceKind::Unknown, identity::AudienceKind::Shared, identity::AudienceKind::Public})
    {
        Fixture fixture(false);
        identity::AudienceContext audience{kind, "private_canonical_scope_fixture", 9, {"private_recipient_fixture"}};
        identity::RelationshipState speaker;
        speaker.entityId = "private_canonical_person_fixture";
        const auto reply = fixture.runtime.ReplyForAudience(
            "Explain static library linking.", {}, audience, speaker, fixture.profile, true, false, {}, [] { return true; });
        Check(reply.succeeded && !fixture.backend.Requests().empty(), "Actual scoped audience did not reach provider.");
        for (const auto& body : fixture.backend.Requests())
        {
            const auto wire = body.dump();
            Check(wire.find(PrivatePreference) == std::string::npos && wire.find(PrivateCause) == std::string::npos &&
                      wire.find("private_canonical_") == std::string::npos && wire.find("private_recipient_fixture") == std::string::npos,
                "Unknown/shared/public scoped route leaked private state or mapping identity.");
        }
    }
    Fixture fixture(false);
    identity::AudienceContext trusted{identity::AudienceKind::Private};
    const auto reply = fixture.runtime.ReplyForAudience(
        "Explain static library linking.", {}, trusted, {}, fixture.profile, true, false, {}, [] { return true; },
        "fixture_clipboard_reference");
    Check(reply.succeeded && fixture.backend.Requests().front().dump().find("fixture_clipboard_reference") != std::string::npos,
        "Trusted guarded private route lost approved turn reference.");
}

void TestRevokedContextDoesNotRequestOrDeliver()
{
    Fixture fixture(false);
    identity::AudienceContext trusted{identity::AudienceKind::Private};
    auto denied = fixture.runtime.ReplyForAudience(
        "Explain static library linking.", {}, trusted, {}, fixture.profile, true, false, {}, [] { return false; });
    Check(!denied.succeeded && denied.text.empty() && fixture.backend.Requests().empty() && fixture.context.GetRecentMessages().empty(),
        "Initially revoked context requested a model or entered history.");
    std::atomic<bool> admitted = true;
    int lateEvents = 0;
    const auto subscription = fixture.events.Subscribe(
        [&](const auto&)
        {
            if (!admitted.load())
                ++lateEvents;
        });
    fixture.backend.onRequest = [&] { admitted.store(false); };
    const auto reply = fixture.runtime.ReplyForAudience(
        "Explain static library linking.", {}, trusted, {}, fixture.profile, true, false, {}, [&] { return admitted.load(); });
    fixture.events.Unsubscribe(subscription);
    Check(!reply.succeeded && reply.text.empty() && lateEvents == 0, "Successful transport delivered a revoked reply or late event.");
    for (const auto& message : fixture.context.GetRecentMessages())
        Check(message.role != "assistant", "Revoked completion entered private assistant history.");
    Check(fixture.coordinator.DrainMemoryEvents().empty(), "Revoked completion queued memory work.");
}

void TestDefaultPrivateWorkCapturesAudienceAdmission()
{
    Fixture fixture(false);
    fixture.runtime.SetPrivateAdmissionFactory([] { return [] { return false; }; });
    const auto opening = fixture.runtime.StartConversation("A harmless fixture cue", "Fixture evidence", fixture.profile, true, false);
    Check(!opening.succeeded && fixture.backend.Requests().empty(), "Default private opening bypassed audience admission.");
    fixture.runtime.SetPrivateAdmissionFactory([]() -> std::function<bool()> { throw std::runtime_error("private fixture payload"); });
    const auto denied = fixture.runtime.StartConversation("A harmless fixture cue", "Fixture evidence", fixture.profile, true, false);
    Check(!denied.succeeded && fixture.backend.Requests().empty(), "Factory exception permitted private background work.");
}
} // namespace

void RunAudiencePrivacyTests()
{
    TestPublicRequestMinimizesPrivateState();
    TestPrivateRequestRetainsApprovedState();
    TestPublicReviewRequestsRemainMinimized();
    TestAudienceRoutesMinimizeAndKeepTrustedPrivateReference();
    TestRevokedContextDoesNotRequestOrDeliver();
    TestDefaultPrivateWorkCapturesAudienceAdmission();
    std::cout << "Audience privacy tests passed (6 real provider-request fixtures).\n";
}
