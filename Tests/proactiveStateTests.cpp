#include "testSupport.h"
#include "Runtime/conversationRuntime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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
    Backend()
    {
        server.Get("/health", [](const auto&, auto& response)
        { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response)
        { response.set_content(R"({"data":[{"id":"proactive-fixture"}]})", "application/json"); });
        server.Post("/v1/chat/completions", [this](const auto& request, auto& response)
        {
            {
                std::lock_guard lock(mutex);
                requests.push_back(json::parse(request.body));
            }
            if (rejectNext.exchange(false))
            {
                response.status = 400;
                response.set_content(R"({"error":"fixture context rejection"})", "application/json");
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
private:
    httplib::Server server;
    std::mutex mutex;
    std::vector<json> requests;
    std::jthread thread;
};

std::string SystemText(const json& request)
{
    std::string text;
    for (const auto& message : request.at("messages"))
        if (message.value("role", "") == "system") text += message.at("content").get<std::string>();
    return text;
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
        : runtime(router, context, coordinator, speech, affect, emotions, events, log,
            [this](RuntimeState state, const std::string&)
            {
                if (cancelAtDelivery && state == RuntimeState::Responding) cancellation.request_stop();
            }, [](const AffectSnapshot&) {},
            []
            {
                actions::CapabilitySettings::InternetAccess access;
                access.enabled = access.automaticLookup = access.autonomousResearch = true;
                return access;
            },
            [this](const std::string&, const std::string&)
            { ++lookups; return actions::ActionOutcome{}; },
            [] { responseFilterSettings filters; filters.bAiReviewEnabled = false; return filters; },
            [this] { ++cachedReads; return std::string("PRIVATE_SCREEN_SENTINEL: cached local observation."); },
            [this] { return person; }, [this] { return development; },
            [this](const emotion::Stimulus& stimulus)
            {
                if (stimulus.eventType != "reply_delivered" && stimulus.eventType != "reply_failed") ++inputAppraisals;
            },
            [this] { ++captures; return std::string("UNEXPECTED_CAPTURE"); },
            [this] { return preferences; },
            [this] { ++inquiries; return agents::SelfInquiryLimits{}; },
            [this](const memory::RecallRequest&, const std::string&)
            { ++recalls; return std::string("UNEXPECTED_ARCHIVE_READ"); })
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
    Backend backend;
    memoryDecision privateMemory;
    privateMemory.bSuccess = privateMemory.bShouldRemember = true;
    privateMemory.category = "project";
    privateMemory.source = "conversation";
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
    const std::vector<conversationMessage> question{{"user", "Describe a maple leaf."}};
    for (const auto tier : {intelligence::IntelligenceTier::Main,
        intelligence::IntelligenceTier::Fast, intelligence::IntelligenceTier::Expert})
    {
        intelligence::IntelligenceDecision decision;
        decision.requestedTier = decision.selectedTier = tier;
        const auto denied = fixture.router.RouteMessage(question.front().content, question, {}, {}, decision,
            llm::PrivateMemoryAccess::Denied);
        Check(denied.bSuccess && denied.selectedTier == intelligence::ToString(tier) &&
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
        backend.Count() == beforeFallback + 2 && backend.embeddingRequests == embeddingsBeforePublic,
        "The privacy fixture did not exercise a bounded Fast-to-Main fallback without embedding.");
    for (const auto& request : backend.RequestsSince(beforeFallback))
        Check(SystemText(request).find("PRIVATE_MEMORY_SENTINEL") == std::string::npos,
            "A rejected or fallback request exposed private curated memory.");

    // Denial must not mutate the shared profile or disable later private retrieval.
    const auto privateReply = fixture.router.RouteMessage(question.front().content, question);
    Check(privateReply.bSuccess && backend.embeddingRequests == embeddingsBeforePublic + 1 &&
        SystemText(backend.Last()).find("PRIVATE_MEMORY_SENTINEL") != std::string::npos,
        "A public request changed the profile or disabled subsequent private memory.");
    responseFilterSettings filters;
    filters.bAiReviewEnabled = false;
    const auto deniedLearning = fixture.coordinator.Execute(fixture.router, question.front().content,
        question, filters, {}, true, 999, {}, {}, {}, llm::PrivateMemoryAccess::Denied);
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
}

void RunProactiveStateTests()
{
    TestProactiveGenerationAndPublicBoundary();
    TestCancelledProactiveCommit();
    std::cout << "Proactive canonical state, additive evidence, public boundary and cancellation owner tests passed.\n";
}
