#include "Agents/calculationGrounding.h"
#include "Runtime/conversationRuntime.h"
#include "testSupport.h"

#include <atomic>
#include <httplib.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

namespace
{
using revia::tests::Check;
using nlohmann::json;

std::string Proposal(const std::string& span, const std::string& expression = "7*6-3")
{
    return json{
        {"calculations", json::array({{{"label", "remaining discs"}, {"expression", expression}, {"unit", ""}, {"sourceSpan", span}}})}}
        .dump();
}

void TestScopedArithmeticGrounding()
{
    using revia::agents::BuildCalculationGrounding;
    const std::string input = "Seven boxes contain 6 discs each; remove 3. What is the total remaining?";
    int calls = 0;
    const auto proposer = [&](const std::string& envelope, std::stop_token)
    {
        ++calls;
        Check(json::parse(envelope).at("currentText") == input, "The numerical proposal lost its exact current input.");
        return Proposal("Seven boxes contain 6 discs each; remove 3.");
    };
    const auto result = BuildCalculationGrounding(input, {}, proposer);
    Check(result.ran && calls == 1 && result.promptBlock.find("\"value\":\"39\"") != std::string::npos &&
              result.promptBlock.find("model-proposed") != std::string::npos &&
              result.promptBlock.find("receiptDigest") != std::string::npos,
        "Actual native arithmetic did not ground the model-proposed expression with limited coverage.");
    const auto literal = BuildCalculationGrounding("Calculate: (8+4)*3/2", {}, proposer);
    Check(literal.ran && calls == 1 && literal.promptBlock.find("\"value\":\"18\"") != std::string::npos &&
              literal.promptBlock.find("\"interpretation\":\"literal\"") != std::string::npos,
        "A literal expression unnecessarily invoked the model or lost its native result.");
    const auto exponent = BuildCalculationGrounding("Calculate 1E2+3", {}, proposer);
    Check(exponent.ran && calls == 1 && exponent.promptBlock.find("\"sourceSpan\":\"1E2+3\"") != std::string::npos,
        "Literal arithmetic did not preserve the exact original source span.");

    for (const std::string text : {"I have 6 games. Tease me about my collection.", "I have 2 hours free. Tell me a joke.",
             "Hi Revia! Give me a warm greeting.", "What is your favorite 90s song?", "She said: \"Calculate 7*6-3.\" Explain her tone.",
             "Explain the code sample `calculate 7*6-3`.", "Never calculate 7*6-3; just say hello."})
        Check(!BuildCalculationGrounding(text, {}, proposer).ran && calls == 1,
            "An ordinary, reported or negated request invoked arithmetic interpretation.");
    const auto forged = [&](const std::string&, std::stop_token) { return Proposal("not in the user's supplied text"); };
    Check(!BuildCalculationGrounding(input, {}, forged).ran, "A fabricated source span was admitted as current numerical evidence.");
    const auto unsupported = [&](const std::string&, std::stop_token) { return Proposal("remove 3", "import os"); };
    Check(!BuildCalculationGrounding(input, {}, unsupported).ran, "Code in a numerical proposal became an observation.");
    const auto invalid = [](const std::string&, std::stop_token) { return "{\"calculations\":[],\"calculations\":[]}"; };
    Check(!BuildCalculationGrounding(input, {}, invalid).ran, "Duplicate numerical proposal keys were silently selected.");
    int revisionCalls = 0;
    const auto revised = [&](const std::string&, std::stop_token)
    {
        ++revisionCalls;
        return Proposal("Actually calculate 4+5", "4+5");
    };
    const auto amended = BuildCalculationGrounding("Calculate 1+2? Actually calculate 4+5", {}, revised);
    Check(amended.ran && revisionCalls == 1 && amended.promptBlock.find("\"value\":\"9\"") != std::string::npos,
        "The literal path ignored a later amendment in the current request.");
    const auto counted = [](const std::string&, std::stop_token) { return Proposal("12 apples and give away 5", "12-5"); };
    Check(BuildCalculationGrounding("I have 12 apples and give away 5. How many do I have?", {}, counted).ran,
        "A direct numerical question without the word total missed the shared arithmetic path.");
    const auto throwing = [](const std::string&, std::stop_token) -> std::string { throw std::runtime_error("fixture failure"); };
    Check(!BuildCalculationGrounding(input, {}, throwing).ran, "Unavailable arithmetic interpretation became verified.");
    std::stop_source stop;
    stop.request_stop();
    Check(!BuildCalculationGrounding(input, {}, proposer, stop.get_token()).ran && calls == 1,
        "Cancelled numerical grounding contacted the model.");
    bool admitted = true;
    const auto revoke = [&](const std::string&, std::stop_token)
    {
        admitted = false;
        return Proposal("remove 3");
    };
    Check(!BuildCalculationGrounding(input, {}, revoke, {}, [&] { return admitted; }).ran,
        "Numerical evidence was returned after the captured admission was revoked.");
}

class GroundingBackend
{
  public:
    GroundingBackend()
    {
        server.Get("/health", [](const auto&, auto& response) { response.set_content(R"({"status":"ok"})", "application/json"); });
        server.Get("/v1/models",
            [](const auto&, auto& response) { response.set_content(R"({"data":[{"id":"grounding-fixture"}]})", "application/json"); });
        server.Post("/v1/chat/completions",
            [this](const auto& request, auto& response)
            {
                const auto body = json::parse(request.body);
                const auto system = body.at("messages").at(0).at("content").template get<std::string>();
                std::string answer;
                if (system.find("restricted native calculator") != std::string::npos ||
                    system.find("restricted native\ncalculator") != std::string::npos)
                {
                    ++proposals;
                    answer = Proposal("7 boxes have 6 discs each; remove 3.");
                    if (revoke)
                        revoke();
                }
                else
                {
                    {
                        std::lock_guard lock(mutex);
                        lastAnswerPrompt.clear();
                        for (const auto& message : body.at("messages"))
                            lastAnswerPrompt += message.at("content").template get<std::string>() + "\n";
                    }
                    ++answers;
                    answer = "The total is 39 discs. That stash is still impressive!";
                }
                const json choice = {{"choices", json::array({{{"message", {{"content", answer}}}, {"finish_reason", "stop"}}})}};
                if (body.value("stream", false))
                {
                    const json chunk = {{"choices", json::array({{{"delta", {{"content", answer}}}, {"finish_reason", "stop"}}})}};
                    response.set_content("data: " + chunk.dump() + "\n\ndata: [DONE]\n\n", "text/event-stream");
                }
                else
                    response.set_content(choice.dump(), "application/json");
            });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Grounding fixture could not bind loopback.");
        worker = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Check(server.is_running(), "Grounding fixture did not start.");
    }
    ~GroundingBackend()
    {
        server.stop();
        worker.join();
    }
    std::string AnswerPrompt()
    {
        std::lock_guard lock(mutex);
        return lastAnswerPrompt;
    }
    int port = 0;
    std::atomic<int> proposals{0}, answers{0};
    std::function<void()> revoke;

  private:
    httplib::Server server;
    std::jthread worker;
    std::mutex mutex;
    std::string lastAnswerPrompt;
};

struct RuntimeFixture
{
    revia::tests::ScopedTestDirectory directory;
    std::filesystem::path previous = std::filesystem::current_path();
    GroundingBackend backend;
    messageRouter router{(directory.root / "router.db").string()};
    conversationContext context;
    revia::agents::TurnCoordinator coordinator{(directory.root / "agent.db").string()};
    revia::speech::SpeechService speech;
    revia::runtime::AffectController affect;
    revia::emotion::EmotionRuntime emotions;
    revia::runtime::RuntimeEventBus events;
    logger log{directory.root / "Logs"};
    revia::runtime::ConversationRuntime runtime{router, context, coordinator, speech, affect, emotions, events, log,
        [](auto, const auto&) {}, [](const auto&) {}, {}, {}, {},
        []
        {
            responseFilterSettings filters;
            filters.bAiReviewEnabled = false;
            return filters;
        },
        {}, {}, {}, {}, {}, {},
        []
        {
            revia::agents::SelfInquiryLimits limits;
            limits.enabled = false;
            return limits;
        }};
    aiProfile profile;

    RuntimeFixture()
    {
        std::filesystem::current_path(directory.root);
        profile.systemPrompt = "You are Revia. Be warm and answer the actual question.";
        profile.id = "grounding-fixture";
        profile.bMemoryEnabled = false;
        llmSettings settings;
        settings.host = "127.0.0.1";
        settings.port = backend.port;
        settings.modelName = "grounding-fixture";
        settings.bAutoStartServer = settings.bAutoMaxTokens = settings.bVisionEnabled = false;
        settings.maxTokens = 256;
        embeddingSettings embeddings;
        embeddings.bEnabled = embeddings.bAutoStartServer = false;
        router.ApplyLLMSettings(settings, embeddings, profile);
    }
    ~RuntimeFixture()
    {
        std::filesystem::current_path(previous);
    }
};

void TestNormalAndEvaluationShareGrounding()
{
    RuntimeFixture fixture;
    const std::string input = "7 boxes have 6 discs each; remove 3. What is the total remaining?";
    const auto evaluated = fixture.runtime.EvaluateTurn(input, {}, fixture.profile, true);
    Check(evaluated.succeeded && fixture.backend.proposals == 1 &&
              fixture.backend.AnswerPrompt().find("\"value\":\"39\"") != std::string::npos,
        "EvaluateTurn did not put actual computed arithmetic into the final generation's posture: succeeded=" +
            std::to_string(evaluated.succeeded) + ", proposals=" + std::to_string(fixture.backend.proposals.load()) +
            ", reason=" + evaluated.reason);
    const auto ordinary = fixture.runtime.Reply(input, fixture.profile, true, false);
    Check(ordinary.succeeded && fixture.backend.proposals == 2 && fixture.backend.AnswerPrompt().find("receiptDigest") != std::string::npos,
        "Normal Reply did not use the same numerical grounding path.");
    const auto unrelated = fixture.runtime.EvaluateTurn("Tell me a playful fact about trees.", {}, fixture.profile, true);
    Check(unrelated.succeeded && fixture.backend.proposals == 2 &&
              fixture.backend.AnswerPrompt().find("[Native arithmetic observations") == std::string::npos,
        "A later unrelated turn inherited arithmetic observations or additional planning.");
    bool admitted = true;
    fixture.runtime.SetPrivateAdmissionFactory([&] { return [&] { return admitted; }; });
    fixture.backend.revoke = [&] { admitted = false; };
    const int before = fixture.backend.answers;
    const auto revoked = fixture.runtime.Reply(input, fixture.profile, true, false);
    Check(!revoked.succeeded && fixture.backend.answers == before,
        "A numerical proposal arriving after revocation reached final answer generation.");
}
}

void RunCalculationGroundingTests()
{
    TestScopedArithmeticGrounding();
    TestNormalAndEvaluationShareGrounding();
}
