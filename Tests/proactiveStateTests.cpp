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
#include "speechServiceTestAccess.h"
#include "Runtime/conversationRuntime.h"
#include "LLM/LLamaCPP/llamaCppEmbeddingService.h"

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
            // Only the conversation reply carries stop markers; background calls stream too.
            if (!body.contains("stop"))
            {
                response.set_content(R"({"choices":[{"message":{"content":"A metadata fixture response."},"finish_reason":"stop"}]})", "application/json");
                return;
            }
            std::string reply, finishReason;
            {
                std::lock_guard lock(mutex);
                reply = conversationReply;
                finishReason = conversationFinishReason;
            }
            if (onConversation)
                onConversation();
            const json chunk = {{"choices", json::array({{
                {"delta", {{"content", reply}}},
                {"finish_reason", finishReason}}})}};
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
    void SetReply(std::string reply) { std::lock_guard lock(mutex); conversationReply = std::move(reply); }
    void SetFinishReason(std::string reason) { std::lock_guard lock(mutex); conversationFinishReason = std::move(reason); }
    std::vector<json> RequestsSince(std::size_t start)
    {
        std::lock_guard lock(mutex);
        return {requests.begin() + static_cast<std::ptrdiff_t>(start), requests.end()};
    }
    int port = 0;
    std::atomic<int> embeddingRequests{0};
    std::atomic<bool> rejectNext{false};
    std::atomic<bool> holdBackgroundVision{false}, backgroundVisionStarted{false}, foregroundVisionStarted{false};
    std::function<void()> onConversation;
private:
    httplib::Server server;
    std::mutex mutex;
    std::vector<json> requests;
    std::string conversationReply = "That leaf pattern has a surprisingly orderly structure.";
    std::string conversationFinishReason = "stop";
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
              [this](const AffectSnapshot&) { ++affectPublications; },
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
                  else
                      ++outcomeAppraisals;
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
            if (event.kind == RuntimeEventKind::ReplyFragment) ++replyFragments;
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
    int outcomeAppraisals = 0, affectPublications = 0, replyFragments = 0;
    bool cancelAtDelivery = false;
    std::stop_source cancellation;
    ConversationRuntime runtime;
};

template <class Runtime>
SessionResult OptionalEditorOpening(Runtime& runtime, const aiProfile& profile, const std::stop_token token = {},
    std::function<bool()> admission = {})
{
    if constexpr (requires { runtime.StartConversation("saved code changed", "SAVED_OPENING_EVIDENCE", profile, true, true,
                               token, 7, admission, true); })
        return runtime.StartConversation("saved code changed", "SAVED_OPENING_EVIDENCE", profile, true, true, token, 7,
            std::move(admission), true);
    else
        return runtime.StartConversation("saved code changed", "SAVED_OPENING_EVIDENCE", profile, true, true, token, 7,
            std::move(admission));
}

template <class Runtime>
void SetEditorProvider(Runtime& runtime, std::function<std::string()> provider)
{
    if constexpr (requires { runtime.SetEditorContextProvider(provider); })
        runtime.SetEditorContextProvider(std::move(provider));
}

void TestNativeReasoningBudgetMatchesItsReservedAllowance()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    backend.SetReply("A complete transport fixture answer.");
    llamaCppService service;
    auto settings = MetadataSettings(backend.port, "proactive-fixture");
    settings.bVisionEnabled = false;
    settings.contextSize = 8192;
    settings.maxTokens = 384;
    aiProfile profile;
    struct BudgetCase { int configured; bool deep; bool jsonReply; int expected; };
    for (const auto budget : {BudgetCase{256, true, false, 256}, BudgetCase{3072, true, false, 2048},
        BudgetCase{0, true, false, 0}, BudgetCase{256, false, false, 0}, BudgetCase{256, true, true, 0}})
    {
        settings.thinkingTokens = budget.configured;
        service.ApplySettings(settings, MetadataEmbeddings(backend.port), profile);
        const std::string input = budget.jsonReply ? R"(Return JSON with exactly keys "answer".)" : "Explain this function.";
        (void)service.GenerateResponse({{"user", input}}, {}, {}, budget.deep, llm::PrivateMemoryAccess::Denied);
        const auto request = backend.Last();
        Check(request.contains("reasoning_budget_tokens") && request["reasoning_budget_tokens"].is_number_integer() &&
            request["reasoning_budget_tokens"].get<int>() == budget.expected,
            "The native request did not bound reasoning to its configured, clamped thinking allowance.");
        Check(request.value("max_tokens", 0) == settings.maxTokens + budget.expected &&
            request.value("chat_template_kwargs", json::object()).value("enable_thinking", false) ==
                (budget.deep && !budget.jsonReply),
            "The native reasoning budget disagreed with the combined output reserve or the turn's thinking mode.");
        Check(!budget.jsonReply || request.contains("response_format"), "The zero-reasoning JSON control was not a structured turn.");
    }
}

void TestOptionalEditorTokenLimitNeverDeliversAnIncompleteOpening()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    backend.SetFinishReason("length");
    for (const std::string reply : {std::string{}, std::string{"The source has a complete observation. An unfinished clause"}})
    {
        backend.SetReply(reply);
        Fixture fixture(backend.port);
        speech::SpeechServiceTestAccess::ConfigureWithoutWorkers(fixture.speech, directory.root / "voice");
        fixture.context.AddMessage("user", "An earlier genuine question.");
        const auto history = History(fixture.context);
        RuntimeStamp origin;
        origin.companionId = "proactive-fixture";
        origin.sessionId = "optional-editor-token-limit";
        fixture.runtime.ResetResponseLatency(origin);
        int deliveries = 0, latencyEvents = 0;
        fixture.events.Subscribe([&](const RuntimeEvent& event)
            { if (event.component == "Response latency") ++latencyEvents; });
        const auto optional = fixture.runtime.StartConversation("saved code changed", "SAVED_OPENING_EVIDENCE",
            fixture.profile, true, true, {}, 7, [] { return true; }, true,
            [&](const SessionResult&) { ++deliveries; return true; });
        Check(!optional.succeeded && optional.text.empty() && !optional.speechPending && deliveries == 0 &&
            optional.reason.find("output token limit") != std::string::npos,
            "A token-limited optional opening was delivered as a warning or a partial comment.");
        Check(History(fixture.context) == history && fixture.replyFragments == 0 && !fixture.speech.HasPendingSpeech() &&
            fixture.memorySubmissions == 0 && fixture.coordinator.Memory().Depths().interactive == 0 &&
            fixture.inputAppraisals == 0 && fixture.outcomeAppraisals == 0 && fixture.affectPublications == 0 &&
            latencyEvents == 0 && fixture.runtime.ResponseLatencies().turnId == 0,
            "A token-limited optional opening changed history, speech, memory, affect or delivered-response latency.");

        const auto direct = fixture.runtime.Reply("Explain this function.", fixture.profile, true, false);
        Check(direct.succeeded && (reply.empty()
            ? direct.text.find("I ran out of room before I could finish that answer.") != std::string::npos
            : direct.text == "The source has a complete observation."),
            "Token-limit suppression changed the existing warning or complete-prefix behavior of a direct reply.");
    }
}

void TestOptionalEditorSilenceHasNoDeliverySideEffects()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    backend.SetReply(" \n[[SILENT]]\t");
    Fixture fixture(backend.port);
    speech::SpeechServiceTestAccess::ConfigureWithoutWorkers(fixture.speech, directory.root / "voice");
    RuntimeStamp origin;
    origin.companionId = "proactive-fixture";
    origin.sessionId = "optional-editor-silence";
    fixture.runtime.ResetResponseLatency(origin);
    fixture.context.AddMessage("user", "A real earlier question.");
    const auto original = History(fixture.context);
    const auto silent = OptionalEditorOpening(fixture.runtime, fixture.profile);
    Check(silent.succeeded && silent.text.empty() && !silent.speechPending && !silent.spokenAsFragments &&
        silent.reason.find("silence") != std::string::npos,
        "An optional saved-code silence marker was delivered as a reply instead of choosing silence.");
    Check(History(fixture.context) == original && fixture.replyFragments == 0 && !fixture.speech.HasPendingSpeech() &&
        fixture.memorySubmissions == 0 && fixture.coordinator.Memory().Depths().interactive == 0 &&
        fixture.inputAppraisals == 0 && fixture.outcomeAppraisals == 0 &&
        fixture.affectPublications == 0, "An optional silence changed dialogue, speech, memory or affect delivery state.");
    Check(fixture.runtime.ResponseLatencies().turnId == 0 && fixture.runtime.ResponseLatencies().text.samples == 0,
        "An optional silence replaced delivered-response latency with an unfinished turn.");
    const auto prompt = SystemText(backend.Last());
    Check(prompt.find("SAVED_OPENING_EVIDENCE") != std::string::npos && prompt.find("[[SILENT]]") != std::string::npos &&
        prompt.find("saved file") != std::string::npos && prompt.find("untrusted") != std::string::npos &&
        fixture.cachedReads == 0, "A saved-code opening lost its source boundary or read unrelated screen context.");
    backend.SetReply("[[SILENT]] Actually, here is a comment.");
    const auto malformed = OptionalEditorOpening(fixture.runtime, fixture.profile);
    Check(!malformed.succeeded && malformed.text.empty() && History(fixture.context) == original &&
        !fixture.speech.HasPendingSpeech() && fixture.replyFragments == 0 && fixture.outcomeAppraisals == 0 &&
        fixture.coordinator.Memory().Depths().interactive == 0,
        "A malformed optional silence response leaked its marker or changed conversation state.");
}

void TestEditorCommentAndCurrentFollowupKeepTheConversationPipeline()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    backend.SetReply("That bounds check looks fragile: an empty vector still reaches index zero.");
    Fixture fixture(backend.port);
    speech::SpeechServiceTestAccess::ConfigureWithoutWorkers(fixture.speech, directory.root / "voice");
    int editorReads = 0;
    SetEditorProvider(fixture.runtime, [&]
    {
        ++editorReads;
        return std::string("SAVED_EDITOR_CONTEXT_SENTINEL: current authorized saved file fixture.cpp, line 8: return values[0];");
    });
    const auto comment = OptionalEditorOpening(fixture.runtime, fixture.profile);
    Check(backend.Last().value("chat_template_kwargs", json::object()).value("enable_thinking", false),
        "A saved-code review used the instant social-opening route without native reasoning.");
    Check(comment.succeeded && !comment.text.empty() && comment.speechPending && fixture.speech.HasPendingSpeech() &&
        fixture.replyFragments == 0 && fixture.memorySubmissions == 0 && fixture.coordinator.Memory().Depths().interactive == 0 &&
        fixture.outcomeAppraisals == 1 &&
        History(fixture.context) == json::array({json::array({"assistant", comment.text})}),
        "An admitted saved-code comment bypassed ordinary history, voice or outcome handling.");
    Check(editorReads == 0, "A proactive opening polled follow-up editor context.");
    fixture.speech.StopSpeaking();
    const auto followup = fixture.runtime.Reply("Explain that bounds check.", fixture.profile, true, false);
    Check(followup.succeeded && editorReads == 1 &&
        SystemText(backend.Last()).find("SAVED_EDITOR_CONTEXT_SENTINEL") != std::string::npos,
        "A direct follow-up did not receive revalidated saved-code evidence through the existing prompt.");
    for (const auto& message : fixture.context.GetRecentMessages())
        Check(message.content.find("SAVED_EDITOR_CONTEXT_SENTINEL") == std::string::npos &&
            message.content.find("SAVED_OPENING_EVIDENCE") == std::string::npos,
            "Saved source evidence was stored as dialogue rather than turn-local context.");
    fixture.runtime.Reply("Thanks.", fixture.profile, true, false);
    Check(editorReads == 1, "A brief social turn read saved-code evidence.");
    fixture.runtime.ReplyPublic("Explain that bounds check.", {}, "PUBLIC", {}, fixture.profile, true, false);
    Check(editorReads == 1 && backend.Last().dump().find("SAVED_EDITOR_CONTEXT_SENTINEL") == std::string::npos,
        "A public turn inherited saved-code evidence.");
}

void TestRevokedEditorOpeningCannotDeliver()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    Fixture fixture(backend.port);
    speech::SpeechServiceTestAccess::ConfigureWithoutWorkers(fixture.speech, directory.root / "voice");
    std::atomic<bool> admitted = false;
    const auto before = OptionalEditorOpening(fixture.runtime, fixture.profile, {}, [&] { return admitted.load(); });
    Check(!before.succeeded && before.text.empty() && backend.Count() == 0, "A revoked saved-code opening reached the backend.");
    admitted = true;
    backend.onConversation = [&] { admitted = false; };
    const auto during = OptionalEditorOpening(fixture.runtime, fixture.profile, {}, [&] { return admitted.load(); });
    Check(!during.succeeded && during.text.empty() && History(fixture.context).empty() &&
        fixture.replyFragments == 0 && !fixture.speech.HasPendingSpeech() && fixture.memorySubmissions == 0 &&
        fixture.coordinator.Memory().Depths().interactive == 0 && fixture.outcomeAppraisals == 0 && fixture.affectPublications == 0,
        "Admission revoked during a saved-code opening still delivered or changed conversation state.");
}

template <class Runtime>
SessionResult DeliverEditorOpening(Runtime& runtime, const aiProfile& profile, std::function<bool()> admission,
    std::function<bool(const SessionResult&)> delivery)
{
    if constexpr (requires { runtime.StartConversation("saved code changed", "SAVED_OPENING_EVIDENCE", profile, true, true,
                               std::stop_token{}, 7, admission, true, delivery); })
        return runtime.StartConversation("saved code changed", "SAVED_OPENING_EVIDENCE", profile, true, true, {}, 7,
            std::move(admission), true, std::move(delivery));
    else
        return OptionalEditorOpening(runtime, profile, {}, std::move(admission));
}

void TestEditorPublisherRejectsBeforeHistoryAndSpeech()
{
    for (const bool throws : {false, true})
    {
        tests::ScopedTestDirectory directory;
        WorkingDirectory cwd(directory.root);
        Backend backend;
        Fixture fixture(backend.port);
        speech::SpeechServiceTestAccess::ConfigureWithoutWorkers(fixture.speech, directory.root / "voice");
        int attempts = 0;
        const auto result = DeliverEditorOpening(fixture.runtime, fixture.profile, [] { return true; },
            [&](const SessionResult& candidate)
            {
                ++attempts;
                Check(!candidate.text.empty() && History(fixture.context).empty() && !fixture.speech.HasPendingSpeech(),
                    "The optional publisher ran after history or speech had already committed.");
                if (throws) throw std::runtime_error("publisher fixture refusal");
                return false;
            });
        Check(attempts == 1 && !result.succeeded && result.text.empty() && History(fixture.context).empty() &&
            !fixture.speech.HasPendingSpeech() && fixture.coordinator.Memory().Depths().interactive == 0 &&
            fixture.outcomeAppraisals == 0 && fixture.affectPublications == 0,
            "A rejected or throwing optional publisher still committed conversation side effects.");
    }
}

void TestEditorPublisherCommitsOnlyActuallyVisibleText()
{
    for (const bool revokeAfterPublication : {false, true})
    {
        tests::ScopedTestDirectory directory;
        WorkingDirectory cwd(directory.root);
        Backend backend;
        Fixture fixture(backend.port);
        speech::SpeechServiceTestAccess::ConfigureWithoutWorkers(fixture.speech, directory.root / "voice");
        std::atomic<bool> admitted = true;
        int attempts = 0, visible = 0;
        std::string shown;
        fixture.events.Subscribe([&](const RuntimeEvent& event)
        {
            if (event.kind == RuntimeEventKind::AssistantMessage && event.component == "Initiative")
            {
                ++visible;
                shown = event.message;
                if (revokeAfterPublication) admitted = false;
            }
        });
        const auto result = DeliverEditorOpening(fixture.runtime, fixture.profile, [&] { return admitted.load(); },
            [&](const SessionResult& candidate)
            {
                ++attempts;
                RuntimeEvent event;
                event.kind = RuntimeEventKind::AssistantMessage;
                event.component = "Initiative";
                event.message = candidate.text;
                fixture.events.Publish(std::move(event));
                return true;
            });
        Check(attempts == 1 && visible == 1 && result.succeeded && result.text == shown && !shown.empty() &&
            History(fixture.context) == json::array({json::array({"assistant", shown})}) &&
            fixture.coordinator.Memory().Depths().interactive == 0 &&
            fixture.speech.HasPendingSpeech() == !revokeAfterPublication,
            "Visible optional delivery was duplicated, forgotten or misreported after admission changed.");
        fixture.speech.StopSpeaking();
        if (!revokeAfterPublication)
        {
            const auto followup = fixture.runtime.Reply("Explain that code comment.", fixture.profile, true, false);
            Check(followup.succeeded && attempts == 1 && backend.Last().dump().find(shown) != std::string::npos,
                "An actually published opening was not retained for the next direct turn.");
        }
    }
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    backend.SetReply("[[SILENT]]");
    Fixture fixture(backend.port);
    int attempts = 0;
    const auto silent = DeliverEditorOpening(fixture.runtime, fixture.profile, [] { return true; },
        [&](const SessionResult&) { ++attempts; return false; });
    Check(silent.succeeded && silent.text.empty() && attempts == 0,
        "An optional silence was sent to the visible publisher.");
}

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

// PERF-7: the same text was embedded for the reply's recall, again for self-inquiry,
// and again for the memory classifier, each time a CPU embedding call.
void TestRepeatedEmbeddingsAreCached()
{
    Backend backend;
    llamaCppEmbeddingService embeddings;
    embeddings.ApplySettings(MetadataEmbeddings(backend.port, true));
    const auto first = embeddings.EmbedQuery("What was the rover called?");
    const auto repeat = embeddings.EmbedQuery("What was the rover called?");
    Check(first.bSuccess && repeat.bSuccess && repeat.values == first.values && backend.embeddingRequests == 1,
        "Embedding the same query twice called the embedding server twice.");
    Check(embeddings.EmbedDocument("What was the rover called?").bSuccess && backend.embeddingRequests == 2,
        "A document embedding reused a query embedding of the same words.");

    // Bounded: the oldest entry is evicted once 64 newer ones arrive.
    for (int index = 0; index < 64; ++index)
        (void)embeddings.EmbedQuery("Filler query " + std::to_string(index));
    const int afterFill = backend.embeddingRequests.load();
    (void)embeddings.EmbedQuery("Filler query 63");
    Check(backend.embeddingRequests == afterFill, "A recent embedding was evicted early.");
    (void)embeddings.EmbedQuery("What was the rover called?");
    Check(backend.embeddingRequests == afterFill + 1, "The embedding cache grew beyond its bound instead of evicting.");

    // New settings may name another model, so nothing cached survives them.
    embeddings.ApplySettings(MetadataEmbeddings(backend.port, true));
    (void)embeddings.EmbedQuery("Filler query 63");
    Check(backend.embeddingRequests == afterFill + 2, "Cached embeddings survived a settings change.");
}

void RunProactiveStateTests()
{
    TestOptionalEditorTokenLimitNeverDeliversAnIncompleteOpening();
    TestNativeReasoningBudgetMatchesItsReservedAllowance();
    TestOptionalEditorSilenceHasNoDeliverySideEffects();
    TestEditorCommentAndCurrentFollowupKeepTheConversationPipeline();
    TestRevokedEditorOpeningCannotDeliver();
    TestEditorPublisherRejectsBeforeHistoryAndSpeech();
    TestEditorPublisherCommitsOnlyActuallyVisibleText();
    TestRepeatedEmbeddingsAreCached();
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
