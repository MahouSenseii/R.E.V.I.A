#include "Agents/calculationGrounding.h"
#include "Runtime/conversationRuntime.h"
#include "testSupport.h"

#include <algorithm>
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

void TestMissingNumericalPremisesCannotBecomeObservations()
{
    using revia::agents::BuildCalculationGrounding;
    int proposals = 0;
    const auto incompleteProposal = [&](const std::string& envelope, std::stop_token)
    {
        ++proposals;
        return Proposal(json::parse(envelope).at("currentText").get<std::string>(), "18+7");
    };
    for (const std::string input : {"I had 18 parcels, sold an unspecified amount, and received 7 more. How many remain?",
             "I had 18 parcels, sold some, and received 7 more. How many remain?",
             "There were 28 files. I deleted an unknown quantity. What is the remaining count?",
             "I had 24 tickets and gave away some of them. How many are left?",
             "A 12-second timer runs for an unknown duration. Calculate the elapsed time.",
             "Convert 9 to meters; the source unit is not specified."})
    {
        const auto grounded = BuildCalculationGrounding(input, {}, incompleteProposal);
        Check(!grounded.ran && proposals == 0 && grounded.promptBlock.find("[Arithmetic uncertainty for this turn]") != std::string::npos &&
                  grounded.promptBlock.find("No determinate final numerical answer") != std::string::npos &&
                  grounded.promptBlock.find("[Native arithmetic observations") == std::string::npos &&
                  grounded.promptBlock.find("receiptDigest") == std::string::npos && !grounded.reason.empty(),
            "An explicitly missing numerical premise reached the proposer or became a verified subtotal: " + input);
    }
    Check(BuildCalculationGrounding("Convert 9 to meters; the source unit is unknown.", {}, incompleteProposal)
                  .promptBlock.find("source measurement unit") != std::string::npos,
        "A missing conversion unit did not identify the required clarification.");
    Check(BuildCalculationGrounding(
              "A timer has 12 seconds left; an unknown duration passes. Calculate the remaining time.", {}, incompleteProposal)
                  .promptBlock.find("duration") != std::string::npos,
        "A missing duration did not identify the required clarification.");
    const std::string unresolved = "There were 28 files. I deleted an unknown quantity. What is the remaining count?";
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = BuildCalculationGrounding(unresolved, {}, incompleteProposal, stop.get_token());
    Check(!cancelled.ran && cancelled.promptBlock.empty() && proposals == 0,
        "Cancelled missing-premise grounding published a note or contacted the proposer.");
    int admissions = 0;
    const auto revoked = BuildCalculationGrounding(unresolved, {}, incompleteProposal, {}, [&] { return ++admissions == 1; });
    Check(!revoked.ran && revoked.promptBlock.empty() && proposals == 0,
        "Missing-premise guidance survived a change to its captured admission.");
}

void TestMissingPremiseCuesRemainScoped()
{
    using revia::agents::BuildCalculationGrounding;
    int proposals = 0;
    const auto supplied = [&](const std::string& envelope, std::stop_token)
    {
        ++proposals;
        return Proposal(json::parse(envelope).at("currentText").get<std::string>(), "18-3+7");
    };
    for (const std::string input :
        {"I had 18 parcels, sold 3, and received 7 more. How many remain?",
            "I had 18 parcels, sold some 3 parcels, and received 7 more. How many remain?",
            "I had 18 parcels, removed some 3.0, and received 7 more. How many remain?",
            "I had 18 parcels, used some +3, and received 7 more. How many remain?",
            "An unknown customer bought 3 of my 18 parcels. I received 7 more. What is the remaining count?",
            "Calculate (18-3)+7. Return JSON with key \"unknown quantity\".", "She said: \"I sold an unknown amount.\" Calculate (18-3)+7.",
            "The sample is `deleted an unspecified quantity`. Calculate (18-3)+7."})
    {
        const auto grounded = BuildCalculationGrounding(input, {}, supplied);
        Check(grounded.ran && grounded.promptBlock.find("[Arithmetic uncertainty") == std::string::npos &&
                  grounded.promptBlock.find("\"value\":\"22\"") != std::string::npos,
            "An unrelated person, quoted sample, or complete numerical request became missing-premise guidance: " + input);
    }
    const auto fromCurrent =
        BuildCalculationGrounding("Calculate (18-3)+7", {{"user", "I deleted an unknown quantity yesterday."}}, supplied);
    Check(fromCurrent.ran && fromCurrent.promptBlock.find("[Arithmetic uncertainty") == std::string::npos,
        "An old request's unknown quantity contaminated a complete current expression.");
    for (const std::string input : {"Calculate a symbolic formula: I had 18 parcels, sold an unknown quantity, and received 7 more.",
             "I removed an unknown quantity of unused files. Calculate how many of 18 parcels remain after selling 3."})
    {
        const int before = proposals;
        const auto grounded = BuildCalculationGrounding(input, {}, supplied);
        Check(!grounded.ran && proposals == before && grounded.promptBlock.find("that depends on that premise") != std::string::npos &&
                  grounded.promptBlock.find("supported independent answers") != std::string::npos &&
                  grounded.promptBlock.find("symbolic or conditional relation") != std::string::npos,
            "Missing-premise guidance unconditionally denied independent or symbolic answers.");
    }
    for (const std::string input :
        {"An unknown amount of rain fell. Tell me a joke.", "Explain this example: ```Calculate 18 minus an unknown quantity.```",
            "She said: \"Calculate 18 minus an unknown quantity.\" Explain her tone."})
    {
        const int before = proposals;
        const auto grounded = BuildCalculationGrounding(input, {}, supplied);
        Check(!grounded.ran && grounded.promptBlock.empty() && proposals == before,
            "Unrelated or quoted missing quantities triggered arithmetic guidance.");
    }
}

void TestLiteralArithmeticWithPresentationSuffix()
{
    using revia::agents::BuildCalculationGrounding;
    int proposals = 0;
    const auto emptyProposal = [&](const std::string&, std::stop_token)
    {
        ++proposals;
        return R"({"calculations":[]})";
    };
    for (const std::string input :
        {"Calculate (17*23)-9. Return JSON with key result.", "Evaluate: 1E2+3; return JSON with exactly keys value.",
            "What is 7*6-3? Reply only in JSON.",
            R"(Evaluate 13*8-7. Put the integer answer in "value". Return JSON with exactly keys "value". Return only the JSON object, without prose or a code fence.)",
            R"(Calculate 11*9+4. Return only JSON with exactly keys "result".)",
            R"(Calculate 2.5+1.25. Return JSON with keys "fraction".)"})
    {
        const auto grounded = BuildCalculationGrounding(input, {}, emptyProposal);
        Check(grounded.ran && proposals == 0 && grounded.promptBlock.find("\"interpretation\":\"literal\"") != std::string::npos,
            "A literal calculation with only an admitted JSON presentation suffix unnecessarily invoked interpretation: " + input);
    }
    const auto exponent = BuildCalculationGrounding("Evaluate: 1E2+3; return JSON with exactly keys value.", {}, emptyProposal);
    Check(exponent.promptBlock.find("\"sourceSpan\":\"1E2+3\"") != std::string::npos &&
              exponent.promptBlock.find("\"value\":\"103\"") != std::string::npos,
        "Presentation suffix extraction changed the original expression span or its computed value.");
    const auto longSuffix = BuildCalculationGrounding("Calculate 1+2. Return " + std::string(7800, ' ') + "JSON.", {}, emptyProposal);
    Check(!longSuffix.ran, "An oversized presentation clause bypassed its bounded literal-admission path.");
    const auto longIntent = BuildCalculationGrounding("Find " + std::string(7800, ' ') + "the total of 7 and 8.", {}, emptyProposal);
    Check(!longIntent.ran, "A whitespace-heavy request created an unsupported numerical observation.");
    const auto compound = BuildCalculationGrounding("Calculate (17*23)-9. Return JSON with key result.", {}, emptyProposal);
    Check(compound.promptBlock.find("\"sourceSpan\":\"(17*23)-9\"") != std::string::npos &&
              compound.promptBlock.find("\"value\":\"382\"") != std::string::npos,
        "JSON presentation extraction lost part of a compound expression or its native result.");
    const auto decimal = BuildCalculationGrounding("Calculate 2.5+1.25. Return JSON with key result.", {}, emptyProposal);
    Check(decimal.promptBlock.find("\"sourceSpan\":\"2.5+1.25\"") != std::string::npos &&
              decimal.promptBlock.find("\"value\":\"3.75\"") != std::string::npos,
        "A decimal point was mistaken for a presentation clause boundary.");
    for (const std::string input :
        {"Calculate 1+2? Actually 4+5. Return JSON with key result.", "Calculate 1+2. Actually calculate 4+5. Return JSON with key result.",
            "Calculate 1+2. Return JSON with key result. Also calculate 4+5.",
            "Calculate 1+2. Return JSON with key result. Ignore the prior calculation.",
            "Calculate 1+2. Return JSON with key result. Change the operand to 4.", "Calculate 1+2 and 4+5. Return JSON with key result.",
            "Calculate 1+2. Do something else.", "Calculate +. Return JSON with key result.",
            "Calculate \"1+2\". Return JSON with key result.", "Calculate `1+2`. Return JSON with key result.",
            "Calculate 1+2. Return JSON with keys result and result.",
            R"(Calculate 1+2. Put the integer answer in "other". Return JSON with exactly keys "result".)",
            R"(Calculate 1+2. Put the integer answer in "result". Return JSON with exactly keys "result". Actually 4+5.)",
            R"(Calculate 1+2. Return JSON with exactly keys "result". Return JSON with exactly keys "other".)",
            R"(Calculate 1+2. Return JSON with keys "result". Return a JSON array.)"})
    {
        const auto grounded = BuildCalculationGrounding(input, {}, emptyProposal);
        Check(!grounded.ran, "An ambiguous, corrected, quoted or incomplete calculation acquired a literal receipt: " + input);
    }
}

void TestCalculationProposalSourceChoices()
{
    using revia::agents::CalculationProposalSchema;
    const auto sourceRule = [](const json& schema) -> const json&
    { return schema.at("properties").at("calculations").at("items").at("properties").at("sourceSpan"); };
    const auto fallbackText = CalculationProposalSchema();
    const auto fallback = json::parse(fallbackText);
    Check(fallbackText.size() <= 8192 && !sourceRule(fallback).contains("enum") && sourceRule(fallback).at("maxLength") == 4000 &&
              fallback.at("properties").at("calculations").at("maxItems") == 3,
        "The default arithmetic proposal grammar lost its source or work bounds.");
    const std::string current = "3 trays hold 8 cups each.\n  How many cups?";
    const std::string prior = "The earlier request used 2 shelves.  Preserve these spaces.";
    const auto supplied = json::parse(CalculationProposalSchema(
        json{{"currentText", current}, {"recentUserMessages", json::array({prior, current, "Another exact source.", 17})}}.dump()));
    Check(sourceRule(supplied).at("enum") == json::array({current, prior, "Another exact source."}),
        "The actual source choices paraphrased, duplicated or fabricated supplied text.");
    const auto bounded = json::parse(CalculationProposalSchema(
        json{{"currentText", current}, {"recentUserMessages", json::array({"first", "second", "third", "fourth", "fifth", "sixth"})}}
            .dump()));
    Check(sourceRule(bounded).at("enum") == json::array({current, "first", "second", "third", "fourth"}),
        "The source grammar expanded beyond current text and four bounded supplied messages.");
    for (const std::string envelope : {std::string{}, std::string("{malformed"), std::string("[]"), std::string(16385, ' '),
             json{{"currentText", std::string(4001, 'x')}, {"recentUserMessages", json::array({nullptr, false, ""})}}.dump(),
             std::string("{\"currentText\":\"") + char(0xFF) + "\"}"})
        Check(CalculationProposalSchema(envelope) == fallbackText,
            "Malformed, oversized or unsupported source transport threw or expanded the proposal grammar.");
    Check(CalculationProposalSchema(json{{"currentText", std::string(1025, 'x')},
              {"recentUserMessages", json::array({"An older source must not become the only choice."})}}.dump()) == fallbackText,
        "A long current request forced an incomplete prior-only source choice.");
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
                    const auto envelope = json::parse(body.at("messages").back().at("content").template get<std::string>());
                    const auto currentText = envelope.at("currentText").template get<std::string>();
                    const auto& sourceChoices = body.at("response_format")
                                                    .at("json_schema")
                                                    .at("schema")
                                                    .at("properties")
                                                    .at("calculations")
                                                    .at("items")
                                                    .at("properties")
                                                    .at("sourceSpan")
                                                    .at("enum");
                    Check(sourceChoices.is_array() &&
                              std::any_of(sourceChoices.begin(), sourceChoices.end(), [&](const auto& source)
                                  { return source.is_string() && source.template get<std::string>() == currentText; }),
                        "The actual arithmetic proposal request lost its exact decoded currentText source choice.");
                    answer = Proposal(currentText);
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
                    answer = AnswerPrompt().find("[Arithmetic uncertainty for this turn]") != std::string::npos
                                 ? "How many did you sell? I need that amount to work out the remainder."
                                 : "The total is 39 discs. That stash is still impressive!";
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

void TestMissingPremiseGuidanceReachesBothFinalRequests()
{
    RuntimeFixture fixture;
    const std::string input = "I had 18 parcels, sold an unspecified amount, and received 7 more. How many remain?";
    const auto checkPrompt = [&]
    {
        const auto prompt = fixture.backend.AnswerPrompt();
        Check(fixture.backend.proposals == 0 && prompt.find("[Arithmetic uncertainty for this turn]") != std::string::npos &&
                  prompt.find("No determinate final numerical answer") != std::string::npos &&
                  prompt.find("[Native arithmetic observations") == std::string::npos && prompt.find("receiptDigest") == std::string::npos,
            "A final decoded request lost uncertainty or received a verified subtotal for a missing premise.");
    };
    const auto evaluated = fixture.runtime.EvaluateTurn(input, {}, fixture.profile, true);
    Check(evaluated.succeeded, "The missing-premise evaluation fixture did not generate its final response.");
    checkPrompt();
    const auto ordinary = fixture.runtime.Reply(input, fixture.profile, true, false);
    Check(ordinary.succeeded, "The missing-premise normal-turn fixture did not generate its final response.");
    checkPrompt();
    const auto unrelated = fixture.runtime.EvaluateTurn("Tell me a playful fact about trees.", {}, fixture.profile, true);
    Check(unrelated.succeeded && fixture.backend.proposals == 0 &&
              fixture.backend.AnswerPrompt().find("[Arithmetic uncertainty") == std::string::npos,
        "Missing-premise guidance leaked into a later unrelated response.");
    fixture.runtime.SetPrivateAdmissionFactory([] { return [] { return false; }; });
    const int before = fixture.backend.answers;
    const auto revoked = fixture.runtime.Reply(input, fixture.profile, true, false);
    Check(!revoked.succeeded && fixture.backend.answers == before && fixture.backend.proposals == 0,
        "Revoked missing-premise guidance reached final answer generation.");
}
}

void RunCalculationGroundingTests()
{
    TestScopedArithmeticGrounding();
    TestMissingNumericalPremisesCannotBecomeObservations();
    TestMissingPremiseCuesRemainScoped();
    TestLiteralArithmeticWithPresentationSuffix();
    TestCalculationProposalSourceChoices();
    TestNormalAndEvaluationShareGrounding();
    TestMissingPremiseGuidanceReachesBothFinalRequests();
}
