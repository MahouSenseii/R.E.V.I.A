#include "reviaSessionTestAccess.h"
#include "speechServiceTestAccess.h"
#include "Emotion/stimulusBuilder.h"
#include "Agents/conversationStylePolicy.h"

#include <atomic>
#include <cmath>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>

namespace
{
using namespace std::chrono_literals;
using namespace revia;
using namespace revia::runtime;
using Access = ReviaSessionTestAccess;
using SpeechAccess = speech::SpeechServiceTestAccess;
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

void Write(const std::filesystem::path& path, const json& value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << value.dump(2);
    output.close();
    Check(!output.fail(), "Could not write the emotion owner fixture.");
}

template<class Predicate> bool Until(Predicate predicate)
{
    const auto end = std::chrono::steady_clock::now() + 5s;
    do
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

class Backend
{
public:
    explicit Backend(bool curiosityFixture = false, std::string idleAction = "silence", std::string operation = "")
    {
        server.Get("/health", [](const auto&, auto& response)
        { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response)
        { response.set_content(R"({"data":[{"id":"fixture-main"}]})", "application/json"); });
        server.Get("/props", [](const auto&, auto& response)
        { response.set_content(R"({"total_slots":1,"default_generation_settings":{"n_ctx":8192}})", "application/json"); });
        server.Post("/v1/chat/completions", [this, curiosityFixture, idleAction, operation](const auto& request, auto& response)
        {
            std::lock_guard lock(mutex);
            last = json::parse(request.body);
            if (onRequest) onRequest();
            if (failReplies.load())
            {
                response.status = 500;
                response.set_content(R"({"error":{"message":"synthetic inference failure"}})", "application/json");
                return;
            }
            const bool nomination = last.contains("response_format");
            const std::string answer = curiosityFixture
                ? nomination ? json({{"action", idleAction},
                    {"topic", idleAction == "silence" ? "" : "fixture private activity"},
                    {"query", operation}, {"rationale", "A specific independent activity is worth pursuing."},
                    {"confidence", 0.9}}).dump()
                    : "A small paper moon above an imaginary tide: PRIVATE_DRAFT_SENTINEL."
                : ++replies == 1
                ? "I heard the request. We can work through the details."
                : "Maple leaves have pointed lobes. Oak leaf shapes vary by species, with rounded or pointed lobes. Both trees change their leaves with the seasons.";
            const json chunk = {{"choices", json::array({{
                {"delta", {{"content", answer}}},
                {"finish_reason", "stop"}}})}};
            if (last.value("stream", false))
                response.set_content("data: " + chunk.dump() + "\n\ndata: [DONE]\n\n", "text/event-stream");
            else
                response.set_content(json{{"choices", json::array({{
                    {"message", {{"role", "assistant"}, {"content", answer}}},
                    {"finish_reason", "stop"}}})}}.dump(), "application/json");
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind emotion fixture backend.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        if (!Until([&] { return server.is_running(); }))
        {
            server.stop(); thread.join();
            Check(false, "Emotion fixture backend did not start.");
        }
    }
    ~Backend() { server.stop(); thread.join(); }
    std::string Prompt()
    {
        std::lock_guard lock(mutex);
        std::string result;
        for (const auto& message : last.at("messages"))
            if (message.value("role", "") == "system") result += message.at("content").get<std::string>();
        return result;
    }
    json Request()
    {
        std::lock_guard lock(mutex);
        return last;
    }
    void ObserveRequests(std::function<void()> observer)
    { std::lock_guard lock(mutex); onRequest = std::move(observer); }
    void FailReplies() { failReplies.store(true); }
    int port = 0;
private:
    httplib::Server server;
    std::mutex mutex;
    json last;
    unsigned replies = 0;
    std::atomic<bool> failReplies = false;
    std::function<void()> onRequest;
    std::jthread thread;
};

void Configure(const std::filesystem::path& root, int port, bool publicChannels = false)
{
    Write(root / "Config/settings.json", {
        {"activeProfile", "fixture"},
        {"llm", {{"backend", "LLamaCpp"}, {"host", "127.0.0.1"}, {"port", port},
            {"modelName", "fixture-main"}, {"autoStartServer", false}, {"visionEnabled", false},
            {"maxTokens", 256}, {"autoMaxTokens", false}, {"modelPath", (root / "absent.gguf").string()},
            {"mediaPath", (root / "RuntimeData/Vision").string()}}},
        {"intelligence", {{"enabled", false}}},
        {"embedding", {{"enabled", false}, {"autoStartServer", false}}},
        {"speech", {{"enabled", false}, {"backend", "WindowsSapi"}, {"speakGreeting", false},
            {"voiceDataPath", (root / "RuntimeData/Voices").string()}}},
        {"speechRecognition", {{"enabled", false}}},
        {"presence", {{"enabled", publicChannels}, {"avatarBridgeEnabled", false},
            {"externalAdaptersEnabled", publicChannels}, {"adapterPollMs", 50},
            {"statePath", (root / "Presence/state.json").string()},
            {"eventPath", (root / "Presence/events.jsonl").string()},
            {"inboxPath", (root / "Presence/Inbox").string()},
            {"outboxPath", (root / "Presence/Outbox").string()}, {"speakStreamReplies", false}}},
        {"vision", {{"enabled", false}}},
        {"perception", {{"enabled", false}}}, {"initiative", {{"enabled", false}}},
        {"bargeIn", {{"enabled", false}}}, {"conversation", {{"archiveEnabled", false}}},
        {"image", {{"enabled", false}}}, {"resources", {{"startupSampleSeconds", 0}}},
        {"responseFilter", {{"aiReviewEnabled", false}}}
    });
    Write(root / "Config/Profiles/fixture.json", {
        {"id", "fixture"}, {"displayName", "Fixture"}, {"systemPrompt", "You are Revia. Emotion owner fixture."},
        {"shouldSpeak", true}, {"memoryEnabled", false}
    });
}

bool SameAffect(const AffectSnapshot& left, const AffectSnapshot& right)
{
    return left.state == right.state && std::abs(left.intensity - right.intensity) < .0001F;
}

struct Subscription
{
    RuntimeEventBus& events;
    RuntimeEventBus::SubscriptionId id;
    ~Subscription() { events.Unsubscribe(id); }
};

struct RequestObservation
{
    Backend& backend;
    ~RequestObservation() { backend.ObserveRequests({}); }
};

void TestContextCompactionKeepsCurrentState()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    llmSettings settings;
    settings.port = backend.port;
    settings.contextSize = 2048;
    settings.maxTokens = 128;
    settings.bAutoMaxTokens = false;
    embeddingSettings embeddings;
    embeddings.bEnabled = false;
    aiProfile profile;
    profile.systemPrompt = "You are Revia. Identity must survive compaction.";
    profile.bMemoryEnabled = false;
    llamaCppService service;
    service.ApplySettings(settings, embeddings, profile);
    service.SetPosture("Current state: angry about a personal jab. Do not invent internal errors.");
    std::vector<conversationMessage> history;
    for (int index = 0; index < 20; ++index)
    {
        history.push_back({"user", std::string(600, 'x')});
        history.push_back({"assistant", std::string(600, 'y')});
    }
    history.push_back({"user", "LATEST " + std::string(4000, 'z') + " END OF CURRENT TURN"});
    Check(service.GenerateResponse(history).bSuccess, "Compacted conversation failed.");
    const auto request = backend.Request();
    const auto& messages = request.at("messages");
    Check(messages.front().value("content", "").find("Identity must survive") != std::string::npos,
        "Context compaction lost the stable identity.");
    Check(messages.front().value("role", "") == "system" &&
        messages.front().value("content", "").find("angry about a personal jab") != std::string::npos,
        "Context compaction discarded the current emotional state.");
    for (std::size_t index = 1; index < messages.size(); ++index)
        Check(messages[index].value("role", "") != "system",
            "The request contains a late system message rejected by Qwen's template.");
    Check(messages.back().value("role", "") == "user" &&
        messages.back().value("content", "").starts_with("LATEST ") &&
        messages.back().value("content", "").ends_with("END OF CURRENT TURN"),
        "Context compaction lost the latest user's message boundaries.");
    std::size_t characters = 0;
    for (const auto& message : messages) characters += message.value("content", "").size();
    Check(characters <= 3072 && request.value("cache_prompt", false),
        "The actual request exceeded its context budget or disabled prefix reuse.");

    settings.maxTokens = 1024;
    service.ApplySettings(settings, embeddings, profile);
    service.SetPosture("You are irritated about that jab.");
    history.push_back({"assistant", "A long defensive reply. " + std::string(10000, 'y')});
    history.push_back({"user", "You need therapy."});
    Check(service.GenerateResponse(history).bSuccess, "Brief social generation failed.");
    const auto brief = backend.Request();
    Check(brief.at("max_tokens") == 128 && brief.at("messages").size() <= 5 &&
        brief.at("messages").back().value("content", "") == "You need therapy.",
        "A brief reaction kept the monologue budget or lost the latest user turn.");
    Check(brief.dump().size() < 4000,
        "A brief reaction still sent old monologues for repeated prompt evaluation.");
    Check(!agents::ConversationStylePolicy::IsBriefSocialTurn("You need therapy. Explain your reasoning in detail.") &&
        !agents::ConversationStylePolicy::IsBriefSocialTurn("What do you remember about my therapy?") &&
        !agents::ConversationStylePolicy::IsBriefSocialTurn("How do you feel about the architecture we discussed?"),
        "A substantive request or memory query received the small-talk budget.");
}

void TestConversationConsumers()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    std::mutex observationMutex;
    emotion::EmotionSnapshot atRequest;
    std::atomic<bool> consistentEvents = true;
    Backend backend;
    Configure(directory.root, backend.port);
    ReviaSession session;
    Check(session.Start(), "Emotion conversation session failed to start.");
    const Subscription subscription{session.Events(), session.Events().Subscribe([&](const RuntimeEvent& event)
    {
        if (event.kind != RuntimeEventKind::AffectChanged) return;
        const auto expected = Access::Emotions(session).ToAffectSnapshot();
        if (event.affect != expected.state || std::abs(event.affectIntensity - expected.intensity) > .0001F)
            consistentEvents = false;
    })};
    Check(session.Submit("You are useless.").succeeded, "Input appraisal fixture failed.");
    Check(session.CurrentEmotion()[emotion::Emotion::Irritation] > .1F,
        "The actual input did not reach canonical appraisal.");

    auto& legacy = Access::LegacyAffect(session);
    legacy.Reset();
    legacy.ObserveInput("Are you sad?");
    SpeechAccess::ConfigureWithoutWorkers(Access::Speech(session), directory.root / "RuntimeData/Voices");
    const RequestObservation observer{backend};
    backend.ObserveRequests([&]
    {
        std::lock_guard lock(observationMutex);
        atRequest = Access::Emotions(session).Current();
    });
    const auto observed = [&]
    {
        std::lock_guard lock(observationMutex);
        return atRequest;
    };
    const auto reply = session.Submit("Describe a maple leaf.");
    Check(reply.succeeded, "The actual emotion conversation request failed.");
    const auto delivered = Access::Emotions(session).ToAffectSnapshot();
    const auto queued = SpeechAccess::PendingAffects(Access::Speech(session));
    Check(!queued.empty(), "The real conversation did not enqueue speech.");
    for (const auto& item : queued)
        Check(SameAffect(item, observed().affect) || SameAffect(item, delivered),
            "Conversation speech used a competing legacy emotional state.");
    Check(backend.Prompt().find("Your current response posture is Irritation") != std::string::npos,
        "The real prompt did not use canonical input emotion.");
    Check(consistentEvents, "Presentation events disagreed with the canonical emotion owner.");

    Access::Speech(session).StopSpeaking();
    Access::Emotions(session).Reset();
    Check(session.Submit("Are you sad?").succeeded, "Named-feeling query failed.");
    Check(observed().emotion.IsCalm() && backend.Prompt().find("posture is Neutral") != std::string::npos,
        "A question naming a feeling created it or revived stale legacy state.");
    Check(backend.Prompt().find("Runtime self-knowledge") == std::string::npos &&
        backend.Prompt().find("hard response filter") == std::string::npos,
        "A real social turn still primed the model to describe diagnostic internals.");

    Access::Speech(session).StopSpeaking();
    Access::Emotions(session).Reset();
    legacy.Reset();
    legacy.ObserveInput("I hate you.");
    const auto reflex = session.Submit("Revia");
    Check(reflex.succeeded && reflex.text == "I'm here.",
        "The actual reflex caller used stale legacy affect.");
    backend.ObserveRequests({});
    session.Stop();
}

void TestReportedSpeechThroughSession()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    Configure(directory.root, backend.port);
    ReviaSession session;
    Check(session.Start(), "Attribution session did not start.");
    Check(session.Submit("My name is Davis.").succeeded, "Messenger introduction failed.");
    const auto before = session.CurrentRelationship();
    const std::string report = "Someone else said to you: \"You're useless. I just said that. My name is Sam. Stop saying chuckles.\"";
    Check(session.Submit(report).succeeded, "Reported-speech session turn failed.");
    const auto after = session.CurrentRelationship();
    Check(before.entityId == after.entityId && before.respect == after.respect &&
        before.resentment == after.resentment && before.irritation == after.irritation,
        "The actual session renamed or penalized the messenger for a quoted insult.");
    const auto request = backend.Request();
    Check(request.at("messages").back().value("content", "").find("speaker: another person, not the current user; addressed to: Revia") != std::string::npos,
        "The actual model request did not receive quoted-speaker evidence.");
    Check(session.Submit("What would you say to them directly?").succeeded,
        "Direct-reply session follow-up failed.");
    Check(backend.Prompt().find("I=Revia, you=that person") != std::string::npos,
        "The actual follow-up request lost the original quoted speaker.");
    Check(session.Submit("You said \"Why do you think I need help?\". Those were your words.").succeeded,
        "Quoted Revia correction failed.");
    Check(backend.Prompt().find("user is asking about their own motive") == std::string::npos,
        "The style classifier treated quoted Revia speech as the user's motive question.");
    session.Stop();
}

void TestMaintenanceAndPersistence()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    Configure(directory.root, backend.port);
    ReviaSession session;
    Access::MaintenanceEvery(session, 25ms, 90ms, 10min);
    Access::IdentitySaveEvery(session, 40ms);
    Check(session.Start(), "Emotion maintenance session failed to start.");
    // No UI polling or curiosity worker is enabled in this fixture.
    Check(session.Submit("You are useless.").succeeded, "Maintenance stimulus failed.");
    auto& people = Access::People(session);
    const auto person = session.CurrentRelationship();
    Check(person.irritation > 0.F, "Real relationship evidence did not create friction.");
    Check(Until([&] { return session.CurrentEmotion().IsCalm(.05F); }),
        "The actual session maintenance worker never settled emotion.");
    Check(Until([&] { return people.Find(person.entityId)->irritation == 0.F; }),
        "The actual session maintenance worker never cooled relationship friction.");
    const auto cooled = *people.Find(person.entityId);
    Check(cooled.familiarity == person.familiarity && cooled.trust == person.trust &&
        cooled.affinity == person.affinity && cooled.interactionCount == person.interactionCount,
        "Settling changed durable relationship meaning instead of transient friction.");
    const auto identityPath = directory.root / "RuntimeData/Identity/identity.json";
    Check(Until([&] { return std::filesystem::is_regular_file(identityPath); }),
        "State maintenance lost periodic identity persistence.");
    session.Stop();
    const auto stopped = Access::Emotions(session).Current();
    std::this_thread::sleep_for(80ms);
    const auto later = Access::Emotions(session).Current();
    Check(stopped.emotion.values == later.emotion.values && stopped.mood.valence == later.mood.valence,
        "Emotion maintenance mutated state after Stop returned.");
    ReviaSession restarted;
    Check(restarted.Start(), "Emotion identity restart failed.");
    Check(std::abs(restarted.CurrentMood().valence - stopped.mood.valence) < .0001F &&
        restarted.CurrentEmotion().IsCalm(), "Restart lost saved mood or restored transient emotion.");
    const auto restored = Access::People(restarted).Find(person.entityId);
    Check(restored && restored->trust == person.trust && restored->irritation == cooled.irritation,
        "Final identity snapshot did not retain the settled relationship.");
    restarted.Stop();
}

void TestDeliveryRequiresEmotionalEvidence()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    Configure(directory.root, backend.port);
    ReviaSession session;
    Check(session.Start(), "Delivery evidence session failed to start.");
    for (const std::string leaf : {"maple", "oak", "willow", "birch", "ginkgo", "elm", "beech"})
    {
        const auto result = session.Submit("Describe a " + leaf + " leaf in one sentence.");
        Check(result.succeeded && result.fromAssistant, "A plain delivery fixture failed.");
        Check(session.CurrentEmotion().IsCalm(.01F),
            "Plain reply delivery changed emotion without relevant evidence.");
    }
    for (const std::string command : {"/help", "/goals", "/help"})
        Check(session.Submit(command).succeeded && session.CurrentEmotion().IsCalm(.01F),
            "A generic command result manufactured achievement reward.");
    Check(session.Submit("Thank you, that was helpful.").succeeded &&
        session.CurrentEmotion()[emotion::Emotion::Gratitude] > .1F,
        "Delivery gating suppressed actual appreciation evidence.");
    Access::Emotions(session).Reset();
    Check(session.Submit("You are useless.").succeeded &&
        session.CurrentEmotion()[emotion::Emotion::Irritation] > .1F &&
        session.CurrentEmotion()[emotion::Emotion::Pride] == 0.F,
        "A delivered reply rewarded hostility or lost its input appraisal.");
    Access::Emotions(session).Reset();
    backend.FailReplies();
    Check(!session.Submit("Describe a cedar leaf in one sentence.").succeeded &&
        session.CurrentEmotion()[emotion::Emotion::Frustration] > .1F,
        "A real failed inference lost confirmed setback appraisal.");
    session.Stop();
}

void TestQuietAdmissionAndRelationshipClocks()
{
    emotion::EmotionRuntime emotions;
    const identity::DevelopmentState development;
    Check(!emotions.ObserveQuietConversation(development, 10min), "Fresh state claimed a quiet period.");
    std::this_thread::sleep_for(70ms);
    Check(!emotions.ObserveQuietConversation(development, 60ms, true, .8F),
        "Being occupied still forced a lonely reaction.");
    const auto quiet = emotions.ObserveQuietConversation(development, 60ms, false, .8F);
    Check(quiet && quiet->emotion[emotion::Emotion::Boredom] > .2F,
        "Accumulated boredom did not reach the canonical emotion state.");
    Check(quiet && quiet->emotion[emotion::Emotion::Loneliness] > .1F,
        "A confirmed quiet interval did not reach canonical emotion.");
    Check(!emotions.ObserveQuietConversation(development, 60ms), "One quiet interval was appraised twice.");
    auto input = emotion::BuildConversationStimulus("fixture", identity::ReadConversationSignals("Okay.", {}, true));
    const auto lonelyBefore = emotions.Emotion()[emotion::Emotion::Loneliness];
    emotions.Observe(input, development);
    Check(emotions.Emotion()[emotion::Emotion::Loneliness] < lonelyBefore,
        "Renewed contact did not relieve the quiet feeling.");
    Check(!emotions.ObserveQuietConversation(development, 10min), "Actual input did not reset quiet admission.");
    std::this_thread::sleep_for(70ms);
    Check(emotions.ObserveQuietConversation(development, 60ms).has_value(), "A later quiet interval could never be observed.");

    tests::ScopedTestDirectory directory;
    identity::RelationshipRegistry people(directory.root / "identity.json");
    std::string error;
    Check(people.Load(error), error);
    identity::RelationshipEvent old;
    old.entityId = "old"; old.negativeInteraction = old.conflict = 1.F;
    const float previouslyIrritated = people.Apply(old).irritation;
    std::this_thread::sleep_for(80ms);
    const auto settleAt = std::chrono::steady_clock::now();
    auto recent = old; recent.entityId = "recent";
    people.Apply(recent);
    const float recentlyIrritated = people.Find("recent")->irritation;
    // An input arriving after the maintenance tick's timestamp must be preserved.
    people.SettleAll(settleAt, 70ms);
    Check(people.Find("old")->irritation < previouslyIrritated &&
        people.Find("old")->irritation > 0.F && people.Find("recent")->irritation == recentlyIrritated,
        "Settling ignored each person's own last interaction.");
}

void TestPublicCompletionAndPresence()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    std::atomic<int> publications = 0;
    std::atomic<bool> consistent = true;
    Backend backend;
    Configure(directory.root, backend.port, true);
    ReviaSession session;
    Check(session.Start(), "Public emotion session did not start.");
    const Subscription subscription{session.Events(), session.Events().Subscribe([&](const RuntimeEvent& event)
    {
        if (event.kind != RuntimeEventKind::AffectChanged) return;
        ++publications;
        const auto state = Access::Emotions(session).ToAffectSnapshot();
        if (state.state != event.affect || std::abs(state.intensity - event.affectIntensity) > .0001F)
            consistent = false;
    })};
    const auto pending = directory.root / "Presence/Inbox/emotion.pending";
    Write(pending, {{"version", 1}, {"id", "emotion"}, {"source", "stream"},
        {"channel", "fixture"}, {"author_id", "viewer-1"}, {"author", "Viewer"},
        {"role", "viewer"}, {"addressed_to_revia", true}, {"text", "You are useless."}});
    std::filesystem::rename(pending, directory.root / "Presence/Inbox/emotion.json");
    const auto replyPath = directory.root / "Presence/Outbox/stream-reply-emotion.json";
    Check(Until([&] { return std::filesystem::is_regular_file(replyPath); }),
        "The actual public adapter turn did not finish.");
    std::ifstream replyFile(replyPath);
    Check(json::parse(replyFile).value("succeeded", false), "The isolated public reply failed.");
    Check(publications == 2 && consistent,
        "Public completion duplicated appraisal publication or overwrote canonical state.");
    const auto state = Access::Emotions(session).ToAffectSnapshot();
    const auto presence = session.Presence();
    Check(presence.affect == state.state && std::abs(presence.affectIntensity - state.intensity) < .0001F,
        "The actual Presence listener did not retain canonical affect.");
    session.Stop();
}

void TestActualQuietWorkerAndGoalOutcomes()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend;
    Configure(directory.root, backend.port);
    {
        ReviaSession quietSession;
        Access::MaintenanceEvery(quietSession, 20ms, 5min, 60ms);
        Check(quietSession.Start(), "Quiet-worker session failed to start.");
        Check(Until([&] { return quietSession.CurrentEmotion()[emotion::Emotion::Loneliness] > .1F; }),
            "The session maintenance caller never appraised a quiet conversation.");
        const auto first = quietSession.CurrentEmotion()[emotion::Emotion::Loneliness];
        Check(Until([&] { return quietSession.CurrentEmotion()[emotion::Emotion::Loneliness] < first; }),
            "The actual quiet worker renewed the same quiet event on every tick.");
        quietSession.Stop();
    }
    Write(directory.root / "capabilities.json", {
        {"mode", "supervised"}, {"approvedRoots", {actions::PathToUtf8(directory.root)}},
        {"autoApproveRiskThrough", "read_only"}, {"createMissingApprovedRoots", false}});
    const auto payload = directory.root / "payload.txt";
    { std::ofstream output(payload); output << "fixture payload"; }
    ReviaSession session;
    Check(session.Start(), "Goal emotion session failed to start.");
    Access::PrepareActions(session, directory.root);
    const auto makeGoal = [&](bool missing)
    {
        goals::Goal goal;
        goal.title = "Read a disposable local file";
        goal.scope = session.Capabilities();
        goal.budget.maxRetriesPerStep = 0;
        goals::GoalStep step;
        step.description = goal.title;
        step.action.id = actions::NewActionId();
        step.action.type = actions::ActionType::ReadTextFile;
        step.action.source = missing ? directory.root / "missing.txt" : payload;
        step.check.id = actions::NewActionId();
        step.check.type = actions::ActionType::ReadTextFile;
        step.check.source = payload;
        step.expected = "fixture payload";
        goal.steps.push_back(std::move(step));
        return goal;
    };
    Access::Emotions(session).Reset();
    const auto completed = Access::RunGoal(session, makeGoal(false));
    Check(completed.status == goals::GoalStatus::Succeeded &&
        session.CurrentEmotion()[emotion::Emotion::Pride] > .1F,
        "The actual goal runner completion never reached canonical appraisal.");
    Access::Emotions(session).Reset();
    const auto failed = Access::RunGoal(session, makeGoal(true));
    Check(failed.status != goals::GoalStatus::Succeeded &&
        !session.CurrentEmotion().IsCalm(), "The actual goal failure did not reach appraisal.");
    Access::Emotions(session).Reset();
    const auto cancelled = Access::RunGoal(session, makeGoal(false), true);
    Check(cancelled.status == goals::GoalStatus::Cancelled && session.CurrentEmotion().IsCalm(),
        "A user-cancelled goal was appraised as a failure.");
    session.Stop();
}

void TestIdleActivitiesProduceWorkWithoutChat()
{
    for (const std::string action : {"create", "computer"})
    {
        tests::ScopedTestDirectory directory;
        WorkingDirectory cwd(directory.root);
        const auto permitted = directory.root / "Approved";
        std::filesystem::create_directories(permitted);
        const auto source = permitted / "quiet  idea.txt";
        { std::ofstream out(source); out << "LOCAL_PC_SENTINEL"; }
        const std::string operation = action == "computer"
            ? json({{"action", "read_text_file"}, {"target", source.string()}}).dump() : "";
        Backend backend(true, action, operation);
        Configure(directory.root, backend.port);
        Write(directory.root / "Config/capabilities.json", {
            {"mode", "supervised"}, {"approvedRoots", {permitted.string()}},
            {"approvedApplications", json::array()}, {"autoApproveRiskThrough", "read_only"}});
        ReviaSession session;
        std::atomic<int> chat{0}, completed{0}, inspected{0};
        const Subscription subscription{session.Events(), session.Events().Subscribe([&](const RuntimeEvent& event)
        {
            if (event.kind == RuntimeEventKind::AssistantMessage) ++chat;
            if (event.component == "Autonomy" && event.phase == "completed") ++completed;
            if (event.component == "Autonomy" && event.detail.find("LOCAL_PC_SENTINEL") != std::string::npos) ++inspected;
        })};
        Check(session.Start(), "The independent activity fixture did not start.");
        const int beforeChat = chat.load();
        Access::StartIdleReviewFixture(session);
        Check(Until([&] { return completed.load() > 0; }), "The real idle worker did not execute " + action);
        Check(chat == beforeChat, "A private activity became unsolicited dialogue.");
        const auto activity = session.CurrentActivity();
        Check(activity && activity->status == autonomy::ActivityStatus::Completed,
            "The completed independent activity was not exposed by its owner.");
        if (action == "create")
        {
            const auto notes = directory.root / "RuntimeData/Workspace/Notes";
            Check(std::filesystem::exists(notes), "Creation produced no workspace directory.");
            int count = 0;
            for (const auto& note : std::filesystem::directory_iterator(notes))
            {
                std::ifstream input(note.path());
                const std::string body((std::istreambuf_iterator<char>(input)), {});
                Check(body.find("PRIVATE_DRAFT_SENTINEL") != std::string::npos,
                    "The kept artifact contained only a plan, not the actual draft.");
                ++count;
            }
            Check(count == 1, "An idle creation wrote more than one artifact.");
            Check(backend.Request()["chat_template_kwargs"]["enable_thinking"] == false,
                "Private creation spent its output budget on hidden reasoning.");
        }
        else
        {
            Check(inspected == 1, "The PC action did not read the approved file through dispatch.");
            Access::StopIdleReviewFixture(session);
            autonomy::ActivityDecision repeat;
            repeat.type = autonomy::ActivityType::Computer;
            repeat.subject = "Read the approved note";
            repeat.reason = "Fixture scope and pacing check";
            repeat.operation = operation;
            repeat.score = .9F;
            Access::RunIdleActivity(session, repeat);
            Check(inspected == 1, "A second activity bypassed spacing.");
            Access::AgeIdleBudget(session);
            Access::RunIdleActivity(session, repeat);
            Check(inspected == 2, "Expired activity history permanently blocked future work.");

            const auto outside = directory.root / "outside.txt";
            { std::ofstream out(outside); out << "OUTSIDE_SCOPE_SENTINEL"; }
            Access::AgeIdleBudget(session);
            repeat.operation = json({{"action", "read_text_file"}, {"source", outside.string()}}).dump();
            Access::RunIdleActivity(session, repeat);
            Check(session.CurrentActivity()->status == autonomy::ActivityStatus::Paused && inspected == 2,
                "An autonomous PC read escaped its approved roots.");
            Access::AgeIdleBudget(session);
            repeat.operation = json({{"action", "move_to_recycle_bin"}, {"source", source.string()}}).dump();
            Access::RunIdleActivity(session, repeat);
            Check(session.CurrentActivity()->status == autonomy::ActivityStatus::Cancelled &&
                std::filesystem::exists(source), "An independent activity deleted user work.");
        }
        session.Stop();
    }
}

void TestIdleReviewRecoversWithResidentGpuMemory()
{
    tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Backend backend(true);
    Configure(directory.root, backend.port);
    // Drive the real sampling callback deterministically, without the test machine's
    // resource sampler racing the supplied GPU readings.
    auto settingsPath = directory.root / "Config/settings.json";
    json settings;
    { std::ifstream input(settingsPath); input >> settings; }
    settings["resources"]["usageSampleSeconds"] = 0;
    Write(settingsPath, settings);
    ReviaSession session;
    Check(session.Start(), "Idle review session did not start.");
    resources::UsageSnapshot usage;
    usage.measured = true;
    resources::UsageMeter vram;
    vram.id = "gpu:CUDA0:vram";
    vram.unit = resources::MeterUnit::Mebibytes;
    vram.capacity = 12288.0;
    vram.used = 11400.0;
    vram.measured = true;
    resources::UsageMeter compute;
    compute.id = "gpu:CUDA0:compute";
    compute.unit = resources::MeterUnit::Percent;
    compute.capacity = 100.0;
    compute.used = 90.0;
    compute.measured = true;
    usage.meters = {vram, compute};
    for (int i = 0; i < 3; ++i) Access::SampleLoad(session, usage);
    Check(!session.CurrentLoad().allowOptionalBackgroundWork,
        "A sustained busy GPU did not defer background work.");
    std::atomic<int> deferred{0}, decisions{0};
    const Subscription subscription{session.Events(), session.Events().Subscribe([&](const RuntimeEvent& event)
    {
        if (event.component != "Curiosity") return;
        if (event.phase == "Deferred") ++deferred;
        if (event.phase == "Kept private") ++decisions;
    })};
    Access::StartIdleReviewFixture(session);
    Check(Until([&] { return deferred.load() > 0; }),
        "The actual idle worker did not report its resource deferral.");
    Check(decisions == 0, "A blocked idle worker still reached inference.");
    usage.meters.back().used = 2.0;
    Access::SampleLoad(session, usage);
    Access::SampleLoad(session, usage);
    Check(!session.CurrentLoad().allowOptionalBackgroundWork,
        "Two transient idle samples bypassed resource hysteresis.");
    Access::SampleLoad(session, usage);
    Check(session.CurrentLoad().allowOptionalBackgroundWork &&
        session.CurrentLoad().state == resources::LoadState::Pressured,
        "Idle compute did not recover work while VRAM remained resident.");
    Check(Until([&] { return decisions.load() >= 1; }),
        "Deferred idle curiosity never reached the backend after recovery.");
    Check(Until([&] { return decisions.load() >= 2; }),
        "Scheduled curiosity stopped without another user or desktop event.");
    const auto request = backend.Request();
    Check(request["response_format"].value("type", "") == "json_schema" &&
        request["response_format"]["json_schema"]["schema"]["properties"]["rationale"]["maxLength"].get<int>() <= 400,
        "The real idle worker did not constrain its nomination fields on the wire.");
    session.Stop();
}
}

void RunEmotionOwnershipTests()
{
    TestContextCompactionKeepsCurrentState();
    TestConversationConsumers();
    TestReportedSpeechThroughSession();
    TestDeliveryRequiresEmotionalEvidence();
    TestMaintenanceAndPersistence();
    TestQuietAdmissionAndRelationshipClocks();
    TestPublicCompletionAndPresence();
    TestActualQuietWorkerAndGoalOutcomes();
    TestIdleReviewRecoversWithResidentGpuMemory();
    TestIdleActivitiesProduceWorkWithoutChat();
    std::cout << "Canonical emotion consumers, real session settling, quiet admission and persistence tests passed.\n";
}
