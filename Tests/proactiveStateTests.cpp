#include "Agents/answerObligation.h"
#include "Agents/responseFilterSettings.h"
#include "Core/conversationMessage.h"
#include "Core/profile.h"
#include "LLM/endpointSettings.h"
#include "LLM/responseTypes.h"
#include "Memory/memoryTypes.h"
#include "Memory/memoryScope.h"
#include "testSupport.h"
#include "promptLayoutTestSupport.h"
#include "Runtime/conversationRuntime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using namespace revia;
using namespace revia::runtime;
using tests::Check;
using json = nlohmann::json;

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path& root)
        : previous(std::filesystem::current_path()) { std::filesystem::current_path(root); }
    ~WorkingDirectory() { std::filesystem::current_path(previous); }
private:
    std::filesystem::path previous;
};

class Backend
{
public:
    explicit Backend(std::vector<std::string> models = {"proactive-fixture"})
    {
        server.Get("/health", [](const auto&, auto& response)
        { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        json inventory = {{"data", json::array()}};
        for (const auto& model : models) inventory["data"].push_back({{"id", model}});
        server.Get("/v1/models", [body = inventory.dump()](const auto&, auto& response)
        { response.set_content(body, "application/json"); });
        server.Post("/v1/chat/completions", [this](const auto& request, auto& response)
        {
            const auto body = json::parse(request.body);
            {
                std::lock_guard lock(mutex);
                requests.push_back(body);
            }
            if (holdBackgroundVision.load())
            {
                if (body.contains("response_format") &&
                    body["response_format"].value("type", "") == "json_schema" &&
                    body["response_format"]["json_schema"].value("name", "") == "screen_awareness")
                {
                    backgroundVisionStarted = true;
                    const auto deadline = std::chrono::steady_clock::now() + 2s;
                    while (!foregroundVisionStarted && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(5ms);
                }
                else
                    foregroundVisionStarted = true;
                response.set_content(R"({"choices":[{"message":{"content":"A test screen summary."},"finish_reason":"stop"}]})", "application/json");
                return;
            }
            if (rejectNext.exchange(false))
            {
                response.status = 400;
                response.set_content(R"({"error":"fixture context rejection"})", "application/json");
                return;
            }
            if (!body.value("stream", false))
            {
                response.set_content(R"({"choices":[{"message":{"content":"A metadata fixture response."},"finish_reason":"stop"}]})", "application/json");
                return;
            }
            const json chunk = {{"choices", json::array({{
                {"delta", {{"content", "That leaf pattern has a surprisingly orderly structure."}}},
                {"finish_reason", "stop"}}})}};
            response.set_content("data: " + chunk.dump() + "\n\ndata: [DONE]\n\n", "text/event-stream");
        });
        server.Post("/v1/embeddings", [this](const auto&, auto& response)
        {
            ++embeddingRequests;
            response.set_content(R"({"data":[{"embedding":[1.0,0.0]}]})", "application/json");
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the proactive fixture.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        if (!server.is_running())
        {
            server.stop(); thread.join();
            Check(false, "Proactive fixture server did not start.");
        }
    }
    ~Backend() { server.stop(); thread.join(); }
    json Last()
    {
        std::lock_guard lock(mutex);
        Check(!requests.empty(), "The proactive entry never reached generation.");
        return requests.back();
    }
    std::size_t Count() { std::lock_guard lock(mutex); return requests.size(); }
    std::vector<json> RequestsSince(std::size_t start)
    {
        std::lock_guard lock(mutex);
        return {requests.begin() + static_cast<std::ptrdiff_t>(start), requests.end()};
    }
    int port = 0;
    std::atomic<int> embeddingRequests{0};
    std::atomic<bool> rejectNext{false};
    std::atomic<bool> holdBackgroundVision{false}, backgroundVisionStarted{false}, foregroundVisionStarted{false};
private:
    httplib::Server server;
    std::mutex mutex;
    std::vector<json> requests;
    std::jthread thread;
};

llmSettings MetadataSettings(const int port, std::string modelName)
{
    llmSettings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.modelName = std::move(modelName);
    settings.bAutoStartServer = false;
    settings.bVisionEnabled = true;
    settings.bAutoMaxTokens = false;
    settings.maxTokens = 256;
    return settings;
}

embeddingSettings MetadataEmbeddings(const int port, const bool enabled = false)
{
    embeddingSettings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.modelName = "fixture-embedding";
    settings.bEnabled = enabled;
    settings.bAutoStartServer = false;
    return settings;
}

std::string SystemText(const json& request)
{
    return revia::tests::RuntimeAuthoredText(request);
}

std::size_t Occurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0, position = 0;
    while ((position = text.find(needle, position)) != std::string::npos)
    { ++count; position += needle.size(); }
    return count;
}

json History(const conversationContext& context)
{
    json result = json::array();
    for (const auto& message : context.GetRecentMessages()) result.push_back({message.role, message.content});
    return result;
}

struct Fixture
{
    explicit Fixture(int port)
        : runtime(
              router, context, coordinator, speech, affect, emotions, events, log,
              [this](RuntimeState state, const std::string&)
              {
                  if (cancelAtDelivery && state == RuntimeState::Responding)
                      cancellation.request_stop();
              },
              [](const AffectSnapshot&) {},
              []
              {
                  actions::CapabilitySettings::InternetAccess access;
                  access.enabled = access.automaticLookup = access.autonomousResearch = true;
                  return access;
              },
              // Hands on, so this fixture keeps measuring what it was written to measure
              // and does not start tripping the no-permission grounding rule.
              []
              {
                  actions::CapabilitySettings::DesktopControl hands;
                  hands.pointer = hands.keyboard = hands.applicationLaunch = true;
                  return hands;
              },
              [this](const std::string&, const std::string&)
              {
                  ++lookups;
                  return actions::ActionOutcome{};
              },
              []
              {
                  responseFilterSettings filters;
                  filters.bAiReviewEnabled = false;
                  return filters;
              },
              [this]
              {
                  ++cachedReads;
                  return std::string("PRIVATE_SCREEN_SENTINEL: cached local observation.");
              },
              [this] { return person; }, [this] { return development; },
              [this](const emotion::Stimulus& stimulus)
              {
                  if (stimulus.eventType != "reply_delivered" && stimulus.eventType != "reply_failed")
                      ++inputAppraisals;
              },
              [this]
              {
                  ++captures;
                  return std::string("UNEXPECTED_CAPTURE");
              },
              [this] { return preferences; },
              [this]
              {
                  ++inquiries;
                  return agents::SelfInquiryLimits{};
              },
              [this](const memory::RecallRequest&, const std::string&, const memory::MemoryScope&)
              {
                  ++recalls;
                  return std::string("UNEXPECTED_ARCHIVE_READ");
              })
    {
        profile.id = "proactive-fixture";
        profile.displayName = "Revia";
        profile.systemPrompt = "You are Revia. PROFILE_SENTINEL.";
        profile.bMemoryEnabled = true;
        profile.answerObligation = AnswerObligationMode::Reliable;
        llmSettings llm;
        llm.host = "127.0.0.1"; llm.port = port; llm.modelName = "proactive-fixture";
        llm.bAutoStartServer = false; llm.bVisionEnabled = false;
        llm.bAutoMaxTokens = false; llm.maxTokens = 256;
        embeddingSettings embedding;
        embedding.bEnabled = true;
        embedding.host = "127.0.0.1"; embedding.port = port;
        embedding.modelName = "fixture-embedding"; embedding.bAutoStartServer = false;
        router.ApplyLLMSettings(llm, llm, llm, embedding, profile, true, true);
        person.entityId = "local:quentin"; person.displayName = "Quentin";
        person.interactionCount = 12; person.familiarity = 0.8F;
        person.affinity = 0.7F; person.trust = 0.8F;
        development.delta[identity::Trait::Patience] = 0.1F;
        identity::Preference opinion;
        opinion.subject = "fixture leaf geometry"; opinion.strength = 0.7F;
        opinion.confidence = 0.8F; opinion.evidenceCount = 8;
        preferences.push_back(opinion);
        emotion::MoodState mood;
        mood.irritability = 0.7F;
        emotions.SetMood(mood);
        events.Subscribe([this](const RuntimeEvent& event)
        {
            if (event.component == "Memory" && event.phase == "Queued") ++memorySubmissions;
        });
    }
    ~Fixture() { coordinator.Stop(); }
    messageRouter router;
    conversationContext context;
    agents::TurnCoordinator coordinator;
    speech::SpeechService speech;
    AffectController affect;
    emotion::EmotionRuntime emotions;
    RuntimeEventBus events;
    logger log;
    identity::RelationshipState person;
    identity::DevelopmentState development;
    std::vector<identity::Preference> preferences;
    aiProfile profile;
    int lookups = 0, captures = 0, cachedReads = 0, recalls = 0, inquiries = 0;
    int inputAppraisals = 0, memorySubmissions = 0;
    bool cancelAtDelivery = false;
    std::stop_source cancellation;
    ConversationRuntime runtime;
};

void CheckCanonicalState(const json& request)
{
    const auto system = SystemText(request);
    for (const std::string marker : {
        "PROFILE_SENTINEL", "How you have changed through experience: you are now more patient",
        "Your current response posture is", "your patience has been worn thin today",
        "About the person you are speaking with: you know them well", "you trust them", "fixture leaf geometry",
        "Runtime self-knowledge (ground truth", "Answer posture: reliable."})
        Check(system.find(marker) != std::string::npos, "Proactive generation omitted canonical state: " + marker);
    for (const std::string marker : {"How you have changed through experience:",
        "Your current response posture is", "About the person you are speaking with:",
        "What you like and dislike."})
        Check(Occurrences(system, marker) == 1, "Proactive generation duplicated a canonical state section.");
}

void TestProactiveGenerationAndPublicBoundary()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend({"proactive-fixture", "metadata-main", "metadata-fast", "metadata-expert"});
    memoryDecision privateMemory;
    privateMemory.bSuccess = privateMemory.bShouldRemember = true;
    privateMemory.category = "project";
    privateMemory.source = "conversation";
    privateMemory.subject = {memory::MemorySubjectKind::Participant, "local:quentin"};
    privateMemory.summary = "Maple leaf drawing PRIVATE_MEMORY_SENTINEL is a private botanical project.";
    bool added = false;
    longTermMemory stored;
    Check(stored.Save(privateMemory, added) && added &&
        stored.BuildPromptBlock("Describe a maple leaf.").find("PRIVATE_MEMORY_SENTINEL") != std::string::npos,
        "The privacy fixture did not seed a matching curated memory.");
    Fixture fixture(backend.port);
    fixture.context.AddMessage("user", "PRIVATE_SUMMARY_SENTINEL was discussed earlier.");
    for (int index = 0; index < 30; ++index)
        fixture.context.AddMessage(index % 2 ? "assistant" : "user", "Prior private leaf discussion " + std::to_string(index));
    Check(fixture.context.GetCompressedHistorySummary().find("PRIVATE_SUMMARY_SENTINEL") != std::string::npos,
        "The fixture did not put its sentinel into compressed history.");
    const auto opening = fixture.runtime.StartConversation("CUE_SENTINEL: a leaf drawing opened",
        "EVIDENCE_SENTINEL: verified local event", fixture.profile, true, false);
    Check(opening.succeeded, "The real proactive opening failed: " + opening.reason);
    CheckCanonicalState(backend.Last());
    const auto first = SystemText(backend.Last());
    for (const auto& marker : {"PRIVATE_SUMMARY_SENTINEL", "PRIVATE_SCREEN_SENTINEL",
        "CUE_SENTINEL", "EVIDENCE_SENTINEL", "Revia is choosing to speak first"})
        Check(first.find(marker) != std::string::npos, "The opening lost additive private context or evidence.");
    Check(first.find("Your current response posture is") < first.find("Revia is choosing to speak first"),
        "The proactive instruction replaced or preceded canonical identity.");

    const auto curiosity = fixture.runtime.StartCuriosityConversation(
        "TOPIC_SENTINEL: leaf geometry", "RATIONALE_SENTINEL: an observed pattern",
        "RESEARCH_SENTINEL: supplied untrusted reference https://example.org/leaf",
        fixture.profile, true, false);
    Check(curiosity.succeeded, "The real curiosity opening failed: " + curiosity.reason);
    const auto currentTask = backend.Last()["messages"].back()["content"].get<std::string>();
    Check(currentTask.find("TOPIC_SENTINEL") != std::string::npos &&
        currentTask.find("supplied source URL") != std::string::npos &&
        currentTask.find("[A private self-directed thought matured.]") == std::string::npos,
        "The model was asked to answer a placeholder instead of the actual research topic.");
    CheckCanonicalState(backend.Last());
    const auto second = SystemText(backend.Last());
    for (const auto& marker : {"TOPIC_SENTINEL", "RATIONALE_SENTINEL", "RESEARCH_SENTINEL",
        "https://example.org/leaf", "Do not claim the user asked for this"})
        Check(second.find(marker) != std::string::npos, "Curiosity lost its additive instructions or supplied grounding.");
    Check(fixture.lookups == 0 && fixture.captures == 0 && fixture.recalls == 0 &&
        fixture.inquiries == 0 && fixture.inputAppraisals == 0 && fixture.memorySubmissions == 0 &&
        backend.Count() == 2, "A proactive cue entered an interactive-only side effect or extra inference path.");
    for (const auto& message : fixture.context.GetRecentMessages())
        Check(message.content.find("SENTINEL") == std::string::npos &&
            message.content.find("[A local conversation opportunity occurred.]") == std::string::npos &&
            message.content.find("[A private self-directed thought matured.]") == std::string::npos,
            "Transient proactive evidence or cue was stored as dialogue.");
    Check(fixture.context.GetRecentMessages().back().role == "assistant" &&
        fixture.context.GetRecentMessages().back().content == curiosity.text,
        "The delivered proactive line was not available for the next user reply.");

    const auto historyBeforePublic = History(fixture.context);
    const int readsBeforePublic = fixture.cachedReads;
    const int embeddingsBeforePublic = backend.embeddingRequests.load();
    identity::RelationshipState viewer;
    viewer.entityId = "adapter:stream:viewer"; viewer.displayName = "PublicViewer";
    viewer.interactionCount = 3;
    const auto publicReply = fixture.runtime.ReplyPublic("Describe a maple leaf.", {}, "PUBLIC_INSTRUCTION",
        viewer, fixture.profile, true, false);
    Check(publicReply.succeeded, "The public follow-up fixture failed.");
    const auto publicRequest = backend.Last().dump();
    for (const auto& marker : {"PRIVATE_MEMORY_SENTINEL", "PRIVATE_SUMMARY_SENTINEL", "PRIVATE_SCREEN_SENTINEL", "you know them well",
        "CUE_SENTINEL", "EVIDENCE_SENTINEL", "TOPIC_SENTINEL", "RESEARCH_SENTINEL", "Prior private leaf discussion"})
        Check(publicRequest.find(marker) == std::string::npos, "A public reply inherited private proactive state: " + std::string(marker));
    Check(publicRequest.find("PUBLIC_INSTRUCTION") != std::string::npos &&
        publicRequest.find("they are nearly a stranger to you") != std::string::npos &&
        fixture.cachedReads == readsBeforePublic && History(fixture.context) == historyBeforePublic &&
        backend.embeddingRequests == embeddingsBeforePublic,
        "Public generation changed private history or used private context/query embedding.");

    // The restriction must survive every configured tier and an actual 400 fallback.
    const auto main = MetadataSettings(backend.port, "metadata-main");
    const auto fast = MetadataSettings(backend.port, "metadata-fast");
    const auto expert = MetadataSettings(backend.port, "metadata-expert");
    fixture.router.ApplyLLMSettings(main, fast, expert, MetadataEmbeddings(backend.port, true), fixture.profile, true, true);
    const memory::MemoryScope privateScope{"local:quentin", {identity::AudienceKind::Private, "local:private", 1, {}},
        identity::SpeakerSource::ExplicitIntroduction, 0, "proactive-fixture"};
    const std::vector<conversationMessage> question{{"user", "Describe a maple leaf.", "local:quentin", privateScope}};
    struct ExpectedRoute
    {
        intelligence::IntelligenceTier tier;
        std::string model;
    };
    for (const auto& expected : std::vector<ExpectedRoute>{{intelligence::IntelligenceTier::Main, "metadata-main"},
        {intelligence::IntelligenceTier::Fast, "metadata-fast"}, {intelligence::IntelligenceTier::Expert, "metadata-expert"}})
    {
        intelligence::IntelligenceDecision decision;
        decision.requestedTier = decision.selectedTier = expected.tier;
        const auto denied = fixture.router.RouteMessage(question.front().content, question, {}, {}, decision,
            llm::PrivateMemoryAccess::Denied);
        Check(denied.bSuccess && denied.selectedTier == intelligence::ToString(expected.tier) &&
            denied.selectedModel == expected.model && backend.Last().value("model", "") == expected.model &&
            SystemText(backend.Last()).find("PRIVATE_MEMORY_SENTINEL") == std::string::npos &&
            backend.embeddingRequests == embeddingsBeforePublic,
            "A configured model tier dropped the private-memory restriction.");
    }
    intelligence::IntelligenceDecision fallback;
    fallback.requestedTier = fallback.selectedTier = intelligence::IntelligenceTier::Fast;
    const auto beforeFallback = backend.Count();
    backend.rejectNext = true;
    const auto retried = fixture.router.RouteMessage(question.front().content, question, {}, {}, fallback,
        llm::PrivateMemoryAccess::Denied);
    Check(retried.bSuccess && retried.bRoutingFallback && retried.selectedTier == "Main" &&
        retried.selectedModel == "metadata-main" && retried.routingFallbackReason.find("Fast") != std::string::npos &&
        backend.Count() == beforeFallback + 2 && backend.embeddingRequests == embeddingsBeforePublic,
        "The privacy fixture did not exercise a bounded Fast-to-Main fallback without embedding.");
    for (const auto& request : backend.RequestsSince(beforeFallback))
        Check(SystemText(request).find("PRIVATE_MEMORY_SENTINEL") == std::string::npos,
            "A rejected or fallback request exposed private curated memory.");
    const auto fallbackRequests = backend.RequestsSince(beforeFallback);
    Check(fallbackRequests.front().value("model", "") == "metadata-fast" &&
        fallbackRequests.back().value("model", "") == "metadata-main",
        "Fast rejection changed the configured model request order.");

    // Denial must not mutate the shared profile or disable later private retrieval.
    const auto privateReply = fixture.router.RouteMessage(question.front().content, question);
    Check(privateReply.bSuccess && backend.embeddingRequests == embeddingsBeforePublic + 1 &&
        SystemText(backend.Last()).find("PRIVATE_MEMORY_SENTINEL") != std::string::npos,
        "A public request changed the profile or disabled subsequent private memory.");
    responseFilterSettings filters;
    filters.bAiReviewEnabled = false;
    const auto deniedLearning = fixture.coordinator.Execute(fixture.router, question.front().content,
        question, filters, {}, true, revia::agents::ResponseProvenance::NormalGeneration, 999, {}, {}, {},
        llm::PrivateMemoryAccess::Denied);
    Check(deniedLearning.response.bSuccess && !deniedLearning.memoryQueued &&
        backend.embeddingRequests == embeddingsBeforePublic + 1 &&
        SystemText(backend.Last()).find("PRIVATE_MEMORY_SENTINEL") == std::string::npos,
        "A denied request retrieved or classified private memory despite its restriction.");
    fixture.profile.bMemoryEnabled = false;
    fixture.router.ApplyProfile(fixture.profile);
    const auto profileOff = fixture.router.RouteMessage(question.front().content, question);
    Check(profileOff.bSuccess && backend.embeddingRequests == embeddingsBeforePublic + 1 &&
        SystemText(backend.Last()).find("PRIVATE_MEMORY_SENTINEL") == std::string::npos,
        "Per-request access enabled memory against the active profile setting.");
}

void TestCancelledProactiveCommit()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    Fixture fixture(backend.port);
    fixture.context.AddMessage("user", "A prior real conversation.");
    const auto original = History(fixture.context);
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto skipped = fixture.runtime.StartConversation("cancelled cue", "event evidence",
        fixture.profile, true, false, cancelled.get_token());
    Check(!skipped.succeeded && skipped.text.empty() && backend.Count() == 0 && History(fixture.context) == original,
        "A pre-cancelled proactive turn generated or changed dialogue.");
    fixture.cancelAtDelivery = true;
    const auto late = fixture.runtime.StartCuriosityConversation("leaf pattern", "observed evidence", {},
        fixture.profile, true, false, fixture.cancellation.get_token());
    Check(fixture.cancellation.stop_requested() && !late.succeeded && late.text.empty() &&
        !late.speechPending && backend.Count() == 1 && History(fixture.context) == original &&
        fixture.memorySubmissions == 0, "Cancellation at delivery committed a proactive reply or transient cue.");
}

void TestCpuFastBrainDefersToGpuMain()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend({"metadata-main", "metadata-fast"});
    messageRouter router;
    llmSettings main;
    main.host = "127.0.0.1"; main.port = backend.port; main.modelName = "metadata-main";
    main.bAutoStartServer = false; main.bVisionEnabled = false;
    main.bAutoMaxTokens = false; main.maxTokens = 64;
    llmSettings fast = main;
    fast.modelName = "metadata-fast";
    fast.device = "none";
    main.device = "CUDA0";
    aiProfile profile;
    profile.systemPrompt = "You are Revia.";
    profile.bMemoryEnabled = false;
    router.ApplyLLMSettings(main, fast, main, embeddingSettings{}, profile, true, false);

    const std::vector<conversationMessage> greeting{{"user", "how are you"}};
    intelligence::IntelligenceDecision decision;
    decision.requestedTier = decision.selectedTier = intelligence::IntelligenceTier::Fast;
    // A small model on the CPU reads the ~2,500-token prompt of every reply at a few
    // hundred tokens a second; Main on a GPU starts answering first, every time.
    const auto routed = router.RouteMessage(greeting.front().content, greeting, {}, {}, decision);
    Check(routed.bSuccess && routed.selectedTier == "Main" && routed.selectedModel == "metadata-main" && !routed.bRoutingFallback &&
        routed.routingReason.find("CPU") != std::string::npos,
        "A short turn went to the CPU brain while Main sat on a GPU: " + routed.selectedTier);

    // With Main's placement left automatic nothing says it is faster, so Fast keeps
    // the turns it was chosen for.
    main.device.clear();
    router.ApplyLLMSettings(main, fast, main, embeddingSettings{}, profile, true, false);
    const auto kept = router.RouteMessage(greeting.front().content, greeting, {}, {}, decision);
    Check(kept.bSuccess && kept.selectedTier == "Fast" && kept.selectedModel == "metadata-fast",
        "The Fast brain lost its turn without a GPU-placed Main to hand it to.");
}

void TestMainResponseReportsItsConfiguredModel()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    messageRouter router;
    llmSettings main;
    main.backend = "Placeholder";
    main.modelName = "metadata-main";
    aiProfile profile;
    profile.bMemoryEnabled = false;
    router.ApplyLLMSettings(main, embeddingSettings{}, profile);

    const std::vector<conversationMessage> question{{"user", "Describe a leaf."}};
    const auto output = router.RouteMessage(question.front().content, question);
    Check(output.bSuccess && output.selectedTier == "Main" && output.selectedModel == "metadata-main",
        "Main reported a model that differs from its configured transport: " + output.selectedModel);
}

void TestConfiguredModelMetadataFollowsAvailabilityFallbacks()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend({"metadata-main", "metadata-fast", "metadata-expert"});
    messageRouter router;
    const auto main = MetadataSettings(backend.port, "metadata-main");
    const auto fast = MetadataSettings(backend.port, "metadata-fast");
    const auto expert = MetadataSettings(backend.port, "metadata-expert");
    const auto embedding = MetadataEmbeddings(backend.port);
    aiProfile profile;
    profile.bMemoryEnabled = false;
    router.ApplyLLMSettings(main, fast, expert, embedding, profile, true, true);
    const std::vector<conversationMessage> question{{"user", "Describe a leaf."}};
    struct ExpectedRoute
    {
        intelligence::IntelligenceTier tier;
        std::string selectedTier;
        std::string model;
    };
    for (const auto& expected : std::vector<ExpectedRoute>{{intelligence::IntelligenceTier::Main, "Main", "metadata-main"},
             {intelligence::IntelligenceTier::Fast, "Fast", "metadata-fast"},
             {intelligence::IntelligenceTier::Expert, "Expert", "metadata-expert"},
             {intelligence::IntelligenceTier::Vision, "Vision", "metadata-main"},
             {intelligence::IntelligenceTier::ExpertVision, "ExpertVision", "metadata-expert"}})
    {
        intelligence::IntelligenceDecision decision;
        decision.requestedTier = decision.selectedTier = expected.tier;
        const auto output = router.RouteMessage(question.front().content, question, {}, {}, decision);
        Check(output.bSuccess && output.selectedTier == expected.selectedTier && output.selectedModel == expected.model &&
                  backend.Last().value("model", "") == expected.model,
            "Foreground model metadata disagreed with the final serving tier: " + output.selectedTier + "/" + output.selectedModel);
    }

    auto unavailableFast = fast;
    auto unavailableExpert = expert;
    unavailableFast.backend = unavailableExpert.backend = "None";
    router.ApplyLLMSettings(main, unavailableFast, unavailableExpert, embedding, profile, true, true);
    for (const auto tier : {intelligence::IntelligenceTier::Fast, intelligence::IntelligenceTier::Expert})
    {
        intelligence::IntelligenceDecision decision;
        decision.requestedTier = decision.selectedTier = tier;
        const auto output = router.RouteMessage(question.front().content, question, {}, {}, decision);
        Check(output.bSuccess && output.bRoutingFallback && output.selectedTier == "Main" && output.selectedModel == "metadata-main" &&
                  backend.Last().value("model", "") == "metadata-main",
            "An unavailable preferred tier retained its model metadata after falling back to Main.");
    }

    auto unavailableMain = main;
    unavailableMain.backend = "None";
    router.ApplyLLMSettings(unavailableMain, fast, unavailableExpert, embedding, profile, true, true);
    const auto output = router.RouteMessage(question.front().content, question);
    Check(output.bSuccess && output.bRoutingFallback && output.selectedTier == "Fast" && output.selectedModel == "metadata-fast" &&
              backend.Last().value("model", "") == "metadata-fast",
        "Main unavailability did not report the configured Fast model that served the response.");
}

void TestConfiguredModelMetadataForReviewCuriosityAndDeliberation()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend({"metadata-main", "metadata-fast", "metadata-expert"});
    messageRouter router;
    const auto main = MetadataSettings(backend.port, "metadata-main");
    const auto fast = MetadataSettings(backend.port, "metadata-fast");
    const auto expert = MetadataSettings(backend.port, "metadata-expert");
    const auto embedding = MetadataEmbeddings(backend.port);
    aiProfile profile;
    profile.bMemoryEnabled = false;
    const auto assertModel = [&](const responseOutput& output, const std::string& tier, const std::string& model)
    {
        Check(output.bSuccess && output.selectedTier == tier && output.selectedModel == model && backend.Last().value("model", "") == model,
            "A bounded response reported a different serving model: " + output.selectedTier + "/" + output.selectedModel);
    };
    router.ApplyLLMSettings(main, fast, expert, embedding, profile, true, true);
    assertModel(router.ReviewCode("Review this material.", "int value = 1;", R"({"type":"object"})"), "Expert", "metadata-expert");
    assertModel(router.GenerateCuriosityPlan("{}", {"silence"}), "Main", "metadata-main");
    assertModel(router.Deliberate("{}"), "Main", "metadata-main");

    auto unavailableExpert = expert;
    unavailableExpert.backend = "None";
    router.ApplyLLMSettings(main, fast, unavailableExpert, embedding, profile, true, true);
    assertModel(router.ReviewCode("Review this material.", "int value = 1;", R"({"type":"object"})"), "Main", "metadata-main");

    auto unavailableMain = main;
    unavailableMain.backend = "None";
    router.ApplyLLMSettings(unavailableMain, fast, unavailableExpert, embedding, profile, true, true);
    const auto curiosity = router.GenerateCuriosityPlan("{}", {"silence"});
    assertModel(curiosity, "Fast", "metadata-fast");
    Check(curiosity.bRoutingFallback, "Curiosity fallback evidence was lost.");
    const auto deliberation = router.Deliberate("{}");
    assertModel(deliberation, "Fast", "metadata-fast");
    Check(deliberation.bRoutingFallback, "Deliberation fallback evidence was lost.");
}

void TestConfiguredModelMetadataForVisionAndSettingsUpdates()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend({"metadata-main", "metadata-fast", "metadata-expert", "metadata-main-updated"});
    messageRouter router;
    auto main = MetadataSettings(backend.port, "metadata-main");
    const auto fast = MetadataSettings(backend.port, "metadata-fast");
    auto expert = MetadataSettings(backend.port, "metadata-expert");
    const auto embedding = MetadataEmbeddings(backend.port);
    aiProfile profile;
    profile.bMemoryEnabled = false;
    router.ApplyLLMSettings(main, fast, expert, embedding, profile, true, true);
    const auto imagePath = directory.root / "metadata-transport.png";
    {
        std::ofstream output(imagePath, std::ios::binary);
        output << "image transport fixture";
    }
    const auto normal = router.AnalyzeImage(imagePath, "Describe this image.", 160);
    Check(normal.bSuccess && normal.selectedTier == "Vision" && normal.selectedModel == "metadata-main" &&
              backend.Last().value("model", "") == "metadata-main",
        "Vision did not report configured Main metadata.");
    const auto difficult = router.AnalyzeImage(imagePath, "Inspect this architecture.", 160);
    Check(difficult.bSuccess && difficult.selectedTier == "ExpertVision" && difficult.selectedModel == "metadata-expert" &&
              backend.Last().value("model", "") == "metadata-expert",
        "ExpertVision did not report configured Expert metadata.");
    expert.backend = "None";
    router.ApplyLLMSettings(main, fast, expert, embedding, profile, true, true);
    const auto fallback = router.AnalyzeImage(imagePath, "Inspect this architecture.", 160);
    Check(fallback.bSuccess && fallback.bRoutingFallback && fallback.selectedTier == "Vision" &&
              fallback.selectedModel == "metadata-main" && backend.Last().value("model", "") == "metadata-main",
        "Unavailable ExpertVision did not report the configured Main fallback model.");

    main.modelName = "metadata-main-updated";
    router.ApplyLLMSettings(main, fast, expert, embedding, profile, true, true);
    const std::vector<conversationMessage> question{{"user", "Describe a leaf."}};
    const auto updated = router.RouteMessage(question.front().content, question);
    Check(updated.bSuccess && updated.selectedModel == "metadata-main-updated" &&
              backend.Last().value("model", "") == "metadata-main-updated",
        "Reapplied transport settings left stale model metadata.");
    profile.id = "updated-profile";
    profile.bHasMaxTokensOverride = true;
    profile.maxTokens = 96;
    router.ApplyProfile(profile);
    const auto profileOnly = router.RouteMessage(question.front().content, question);
    Check(profileOnly.bSuccess && profileOnly.selectedModel == "metadata-main-updated" &&
              backend.Last().value("model", "") == "metadata-main-updated",
        "Profile application changed captured transport model metadata.");
}

void TestBackgroundVisionContractAndPreemption()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    backend.holdBackgroundVision = true;
    const auto imagePath = directory.root / "transport-fixture.png";
    // The test backend inspects the request envelope rather than decoding pixels.
    { std::ofstream output(imagePath, std::ios::binary); output << "image transport fixture"; }
    llamaCppService service;
    llmSettings settings;
    settings.host = "127.0.0.1"; settings.port = backend.port;
    settings.modelName = "proactive-fixture";
    settings.parallelRequests = 1;
    embeddingSettings embedding;
    embedding.bEnabled = false;
    aiProfile profile;
    service.ApplySettings(settings, embedding, profile);
    responseOutput background;
    std::jthread worker([&] { background = service.AnalyzeImage(imagePath, "ambient", 160, {}, true); });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!backend.backgroundVisionStarted && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    Check(backend.backgroundVisionStarted, "The background vision request did not reach the backend.");
    const auto request = backend.Last();
    Check(request["max_tokens"].get<int>() >= 192 &&
        request["response_format"]["json_schema"]["schema"]["properties"]["summary"]["type"] == "string",
        "Ambient vision did not send a bounded structured assessment contract.");
    const auto started = std::chrono::steady_clock::now();
    const auto foreground = service.AnalyzeImage(imagePath, "user requested look", 160);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    worker.join();
    Check(foreground.bSuccess && !background.bSuccess && elapsed < 1s &&
        background.reason == "Background screen awareness yielded to user input.",
        "Background vision preemption: foreground=" + std::to_string(foreground.bSuccess) +
        " background=" + std::to_string(background.bSuccess) + " elapsed_ms=" +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) +
        " reason=" + background.reason + " foreground_reason=" + foreground.reason);
    Check(!backend.Last().contains("response_format"),
        "The ambient assessment schema leaked into user-requested vision.");
}
}

// Text fetched for one question, such as what the user copied, reaches that turn's
// prompt and nothing else: not the history, not the next turn.
void TestTurnReferenceStaysInItsTurn()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    Fixture fixture(backend.port);
    const std::size_t before = backend.Count();
    const auto asked = fixture.runtime.Reply("What's on my clipboard?", fixture.profile, true,
        false, {}, "CLIPBOARD_SENTINEL copied text");
    const auto requests = backend.RequestsSince(before);
    Check(asked.succeeded && std::any_of(requests.begin(), requests.end(), [](const json& request)
        { return SystemText(request).find("CLIPBOARD_SENTINEL") != std::string::npos; }),
        "Text fetched for the turn never reached the model.");
    Check(History(fixture.context).dump().find("CLIPBOARD_SENTINEL") == std::string::npos,
        "Text fetched for one turn was written into the conversation history.");

    const std::size_t next = backend.Count();
    const auto followUp = fixture.runtime.Reply("And the next thing?", fixture.profile, true, false);
    const auto later = backend.RequestsSince(next);
    Check(followUp.succeeded && std::none_of(later.begin(), later.end(), [](const json& request)
        { return request.dump().find("CLIPBOARD_SENTINEL") != std::string::npos; }),
        "Text fetched for one turn was sent again with the next.");
}

void RunProactiveStateTests()
{
    TestMainResponseReportsItsConfiguredModel();
    TestConfiguredModelMetadataFollowsAvailabilityFallbacks();
    TestConfiguredModelMetadataForReviewCuriosityAndDeliberation();
    TestConfiguredModelMetadataForVisionAndSettingsUpdates();
    TestTurnReferenceStaysInItsTurn();
    TestProactiveGenerationAndPublicBoundary();
    TestCancelledProactiveCommit();
    TestCpuFastBrainDefersToGpuMain();
    TestBackgroundVisionContractAndPreemption();
    std::cout << "Proactive canonical state, additive evidence, public boundary and cancellation owner tests passed.\n";
}
