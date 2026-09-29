#include "testSupport.h"
#include "reviaSessionTestAccess.h"

#include "Core/secretStore.h"
#include "Intelligence/advisor.h"
#include "LLM/httpsTransport.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

// A frontier model on call, never in her seat.
//
// What decides a consult is a rule, not the small model's confidence; what leaves is a
// brief that has been checked for a credential and cut to the share level; what comes
// back is notes she reads as input. The key is in a store nothing else reads, or in the
// environment, and never in a settings file. The last test runs the whole path through
// a real session against a fake advisor and a fake local brain.
namespace
{
using namespace std::chrono_literals;
using revia::core::SecretStore;
using revia::intelligence::AdvisorBrief;
using revia::intelligence::AdvisorClient;
using revia::intelligence::AdvisorDecision;
using revia::intelligence::AdvisorNotes;
using revia::intelligence::AdvisorTrigger;
using revia::intelligence::AsksForAdvisor;
using revia::intelligence::BuildAdvisorBrief;
using revia::intelligence::DecideAdvisorConsult;
using revia::llm::HttpsRequest;
using revia::llm::HttpsResponse;
using revia::llm::HttpsTransport;
using revia::runtime::ReviaSession;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using Access = revia::runtime::ReviaSessionTestAccess;
using json = nlohmann::json;

// The advisor as the client sees it: records what was sent, answers what it is told to.
struct Recorder
{
    std::mutex mutex;
    std::vector<HttpsRequest> requests;
    HttpsResponse reply;
    bool available = true;
};

class FakeTransport final : public HttpsTransport
{
public:
    explicit FakeTransport(std::shared_ptr<Recorder> inputRecorder) : recorder(std::move(inputRecorder)) {}
    HttpsResponse Post(const HttpsRequest& request, std::stop_token) override
    {
        std::lock_guard lock(recorder->mutex);
        recorder->requests.push_back(request);
        return recorder->reply;
    }
    std::string Describe() const override { return "fake transport"; }
    bool Available() const override { return recorder->available; }
private:
    std::shared_ptr<Recorder> recorder;
};

HttpsResponse AnthropicReply(const std::string& text)
{
    HttpsResponse response;
    response.completed = true;
    response.status = 200;
    response.body = json{
        {"model", "claude-fixture"},
        {"content", json::array({{{"type", "text"}, {"text", text}}})},
        {"usage", {{"input_tokens", 120}, {"output_tokens", 40}}}}.dump();
    return response;
}

advisorSettings Settings(const std::string& dialect = "Anthropic")
{
    advisorSettings settings;
    settings.bEnabled = true;
    settings.dialect = dialect;
    settings.host = dialect == "OpenAI" ? "openrouter.ai" : "api.anthropic.com";
    settings.modelName = dialect == "OpenAI" ? "openai/gpt-fixture" : "claude-fixture";
    settings.path = dialect == "OpenAI" ? "/api/v1/chat/completions" : "";
    settings.maxTokens = 900;
    return settings;
}

conversationMessage Turn(const std::string& role, const std::string& content)
{
    conversationMessage message;
    message.role = role;
    message.content = content;
    return message;
}

void TestTheRulesDecideAndNotTheModel()
{
    const auto decide = [](const std::string& input, const std::string& escalation,
        const bool previousUncertainty = false, const bool publicAudience = false, const bool forced = false)
    {
        return DecideAdvisorConsult(input, escalation, previousUncertainty, publicAudience, forced);
    };
    Check(AsksForAdvisor("Could you ask Claude what the best index for this table is?") &&
        AsksForAdvisor("check with the advisor first") && !AsksForAdvisor("ask her what she thinks"),
        "The words that ask for the advisor were not recognised.");
    Check(decide("ask claude why this deadlocks", "ask").trigger == AdvisorTrigger::Explicit,
        "An explicit ask under \"ask\" did not consult.");
    Check(!decide("why does this deadlock when I lock it twice?", "ask").consult,
        "Under \"ask\", a hard turn nobody asked about was consulted.");
    Check(!decide("ask claude why this deadlocks", "never").consult &&
        !decide("ask claude why this deadlocks", "auto", false, true).consult,
        "\"never\" or a public audience consulted.");
    Check(decide("hi", "auto", false, false, true).trigger == AdvisorTrigger::Forced,
        "A forced turn was not consulted.");
    Check(!decide("thanks a lot", "auto").consult, "A short social turn went to the advisor.");
    Check(decide("Here is my code:\n```cpp\nint main() {}\n```\nwhy does it warn?", "auto").trigger ==
        AdvisorTrigger::Code, "A fenced listing was not routed as code.");
    std::string listing = "why does this leak\n";
    for (int index = 0; index < 9; ++index) listing += "    auto value" + std::to_string(index) + " = make();\n";
    Check(decide(listing, "auto").trigger == AdvisorTrigger::Code,
        "Nine lines of code without a fence were not routed as code.");
    Check(decide("what changed in the latest version of the Vulkan SDK?", "auto").trigger ==
            AdvisorTrigger::Recency &&
        decide("what did the 2027 rules say about this?", "auto").trigger == AdvisorTrigger::Recency,
        "A question about something newer than the model was not routed.");
    Check(decide("compare SQLite and DuckDB for my use case here", "auto").trigger ==
        AdvisorTrigger::MultiStep, "A comparison was not routed as a plan.");
    Check(decide("so what does that mean for the second part", "auto", true).trigger ==
        AdvisorTrigger::Uncertainty, "A follow-up to a doubtful answer was not routed.");
    Check(decide(std::string(1600, 'x') + " what is this", "auto").trigger == AdvisorTrigger::Length,
        "A long input was not routed as document work.");
    Check(!decide("tell me about the garden you like", "auto").consult,
        "An ordinary turn was sent to the advisor under \"auto\".");
}

void TestTheBriefCarriesOnlyWhatMayLeave()
{
    const std::vector<conversationMessage> turns = {
        Turn("user", "my token is ghp_abcdefghijklmnopqrstuvwxyz0123456789ABCD keep it"),
        Turn("assistant", "I will not keep that."),
        Turn("user", "the log is at C:\\Users\\quentin\\Documents\\app.log"),
        Turn("assistant", "Noted.")};
    const AdvisorBrief question = BuildAdvisorBrief("why does the log fill up?", turns, "Today is 2026-09-29.", "question", 6000);
    Check(!question.refused && question.context.empty() && question.question == "why does the log fill up?" &&
        question.facts == "Today is 2026-09-29." && question.redactions.empty(),
        "The question-only share carried more than the question.");
    const AdvisorBrief conversation = BuildAdvisorBrief("why does the log fill up?", turns, "", "conversation", 6000);
    Check(!conversation.refused && conversation.context.find("ghp_") == std::string::npos &&
        conversation.context.find("[a line withheld: it carried a GitHub") != std::string::npos &&
        conversation.context.find("Revia: I will not keep that.") != std::string::npos,
        "A line carrying a token was sent, or the withholding was not said: " + conversation.context);
    Check(conversation.context.find("C:\\Users\\<user>\\Documents\\app.log") != std::string::npos &&
        conversation.context.find("quentin") == std::string::npos,
        "The account name in a profile path left the machine: " + conversation.context);
    Check(conversation.redactions.size() == 2 && conversation.Render().find("Recent conversation") != std::string::npos,
        "The redactions were not both recorded.");
    const AdvisorBrief refused = BuildAdvisorBrief("is sk-ant-api03-abcdefghijklmnopqrstuvwxyz0123456789 still valid?", {}, "", "question", 6000);
    Check(refused.refused && refused.refusal.find("stays on this machine") != std::string::npos &&
        refused.refusal.find("sk-ant") == std::string::npos,
        "A question carrying a key was not refused, or the refusal repeated it.");
    std::vector<conversationMessage> many;
    for (int index = 0; index < 20; ++index)
    {
        many.push_back(Turn("user", "line " + std::to_string(index) + " " + std::string(300, 'u')));
    }
    const AdvisorBrief bounded = BuildAdvisorBrief("short question", many, "", "conversation", 1000);
    Check(bounded.Characters() <= 1000 && bounded.context.find("line 19") != std::string::npos &&
        bounded.context.find("line 0 ") == std::string::npos,
        "The budget did not keep the newest turns and drop the oldest.");
    Check(BuildAdvisorBrief("   ", {}, "", "question", 6000).refused, "An empty question was sent.");
}

void TestTheClientSpeaksBothDialectsAndReadsBothAnswers()
{
    auto recorder = std::make_shared<Recorder>();
    recorder->reply = AnthropicReply("Facts: a mutex locked twice by one thread deadlocks unless recursive.\nUnsure: nothing.");
    AdvisorClient anthropic(std::make_unique<FakeTransport>(recorder));
    anthropic.Configure(Settings(), "test-key");
    const AdvisorBrief brief = BuildAdvisorBrief("why does locking twice deadlock?", {}, "Today is 2026-09-29.", "question", 6000);
    const AdvisorNotes notes = anthropic.Consult(brief, {});
    Check(notes.succeeded && notes.notes.find("recursive") != std::string::npos && notes.model == "claude-fixture" &&
        notes.inputTokens == 120 && notes.outputTokens == 40, "The Anthropic answer was not read: " + notes.reason);
    {
        std::lock_guard lock(recorder->mutex);
        Check(recorder->requests.size() == 1, "The consult did not send exactly one request.");
        const HttpsRequest& request = recorder->requests.front();
        Check(request.host == "api.anthropic.com" && request.port == 443 && request.path == "/v1/messages",
            "The Anthropic request did not go to the Messages endpoint.");
        bool key = false;
        bool version = false;
        for (const auto& [name, value] : request.headers)
        {
            if (name == "x-api-key" && value == "test-key") key = true;
            if (name == "anthropic-version" && !value.empty()) version = true;
        }
        Check(key && version, "The Anthropic headers were not set.");
        const json body = json::parse(request.body);
        Check(body["model"] == "claude-fixture" && body["max_tokens"] == 900 &&
            body["system"].get<std::string>().find("advisor to Revia") != std::string::npos &&
            body["messages"].size() == 1 && body["messages"][0]["role"] == "user" &&
            body["messages"][0]["content"].get<std::string>().find("why does locking twice deadlock?") != std::string::npos,
            "The Anthropic body did not carry the brief as one user message under the advisor system prompt.");
    }
    const std::string block = AdvisorClient::RenderNotesForPrompt(notes);
    Check(block.rfind("# Advisor notes", 0) == 0 && block.find("claude-fixture") != std::string::npos &&
        block.find("input, not your words") != std::string::npos && block.find("recursive") != std::string::npos,
        "The notes were not framed as input to check.");

    auto openaiRecorder = std::make_shared<Recorder>();
    openaiRecorder->reply.completed = true;
    openaiRecorder->reply.status = 200;
    openaiRecorder->reply.body = json{
        {"model", "openai/gpt-fixture"},
        {"choices", json::array({{{"message", {{"role", "assistant"}, {"content", "Facts: fine."}}}}})},
        {"usage", {{"prompt_tokens", 50}, {"completion_tokens", 5}}}}.dump();
    AdvisorClient openai(std::make_unique<FakeTransport>(openaiRecorder));
    openai.Configure(Settings("OpenAI"), "or-key");
    const AdvisorNotes openaiNotes = openai.Consult(brief, {});
    Check(openaiNotes.succeeded && openaiNotes.notes == "Facts: fine." && openaiNotes.inputTokens == 50,
        "The OpenAI answer was not read: " + openaiNotes.reason);
    {
        std::lock_guard lock(openaiRecorder->mutex);
        const HttpsRequest& request = openaiRecorder->requests.front();
        bool bearer = false;
        for (const auto& [name, value] : request.headers)
            if (name == "Authorization" && value == "Bearer or-key") bearer = true;
        const json body = json::parse(request.body);
        Check(request.host == "openrouter.ai" && request.path == "/api/v1/chat/completions" && bearer &&
            body["messages"].size() == 2 && body["messages"][0]["role"] == "system",
            "The OpenAI request did not carry the bearer key and the system message.");
    }

    // Refusals before and after the wire.
    AdvisorClient keyless(std::make_unique<FakeTransport>(recorder));
    keyless.Configure(Settings(), "");
    const AdvisorNotes noKey = keyless.Consult(brief, {});
    std::size_t sent = 0;
    {
        std::lock_guard lock(recorder->mutex);
        sent = recorder->requests.size();
    }
    Check(!noKey.succeeded && noKey.reason.find("No advisor key") != std::string::npos && sent == 1,
        "A client without a key touched the transport.");
    recorder->reply = HttpsResponse{};
    recorder->reply.completed = true;
    recorder->reply.status = 401;
    recorder->reply.body = R"({"error":{"type":"authentication_error","message":"invalid x-api-key"}})";
    const AdvisorNotes rejected = anthropic.Consult(brief, {});
    Check(!rejected.succeeded && rejected.reason.find("401") != std::string::npos &&
        rejected.reason.find("invalid x-api-key") != std::string::npos,
        "A rejected key was not reported with the service's reason.");
    recorder->available = false;
    const AdvisorNotes offline = anthropic.Consult(brief, {});
    Check(!offline.succeeded && offline.reason == "fake transport", "An unavailable transport was still used.");
    recorder->available = true;
    Check(!anthropic.Consult(BuildAdvisorBrief("   ", {}, "", "question", 6000), {}).succeeded,
        "A refused brief reached the transport.");
}

void TestTheKeyLivesInTheStoreOrTheEnvironmentAndNowhereElse()
{
    ScopedTestDirectory directory;
    // A codec that only reverses, so the store's own behaviour is what is tested.
    SecretStore::Codec reversing;
    reversing.protect = [](const std::vector<unsigned char>& bytes, std::string&)
    {
        return std::vector<unsigned char>(bytes.rbegin(), bytes.rend());
    };
    reversing.unprotect = reversing.protect;
    const SecretStore store(directory.root / "Secrets", reversing);
    std::string error;
    Check(store.Store("advisor", "sk-test-value", error), "The secret was not stored: " + error);
    Check(store.Has("advisor") && store.PathOf("advisor").filename() == "advisor.dpapi",
        "The stored secret is not where the writer script puts it.");
    std::ifstream raw(store.PathOf("advisor"), std::ios::binary);
    const std::string onDisk((std::istreambuf_iterator<char>(raw)), std::istreambuf_iterator<char>());
    Check(onDisk.find("sk-test-value") == std::string::npos, "The secret is on disk in the clear.");
    const auto loaded = store.Load("advisor", error);
    Check(loaded && *loaded == "sk-test-value", "The secret did not round-trip: " + error);
    Check(!store.Load("missing", error) && error.find("No secret named") != std::string::npos,
        "A missing secret was not reported.");
    Check(!store.Store("../escape", "x", error) && !store.Store("", "x", error) && !store.Store("advisor", "", error),
        "A bad name or an empty value was stored.");
    Check(store.Forget("advisor") && !store.Has("advisor"), "The secret was not forgotten.");

    const SecretStore platform(directory.root / "PlatformSecrets");
    if (SecretStore::SystemCodecAvailable())
    {
        Check(platform.Store("advisor", "round", error) && platform.Load("advisor", error) == "round",
            "The platform codec did not round-trip: " + error);
    }
    else
    {
        Check(!platform.Store("advisor", "round", error) && error.find("DPAPI") != std::string::npos,
            "Without DPAPI the store pretended to protect a secret.");
    }
    Check(std::string(SecretStore::Entropy()) == "revia.secret-store.v1",
        "The entropy the PowerShell writer binds to has changed.");
}

// A local brain that answers every turn the same way and keeps the last request.
class Brain
{
public:
    Brain()
    {
        server.Get("/health", [](const auto&, auto& response)
        { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response)
        { response.set_content(R"({"data":[{"id":"fixture-main"}]})", "application/json"); });
        server.Get("/props", [](const auto&, auto& response)
        {
            response.set_content(R"({"total_slots":1,"default_generation_settings":{"n_ctx":8192}})",
                "application/json");
        });
        server.Post("/v1/chat/completions", [this](const auto& request, auto& response)
        {
            {
                std::lock_guard lock(mutex);
                lastRequest = request.body;
            }
            const std::string answer = "A mutex locked twice by the same thread deadlocks unless it is recursive.";
            const json body = json::parse(request.body);
            if (body.value("stream", false))
            {
                const json chunk = {{"choices", json::array({{{"delta", {{"content", answer}}}, {"finish_reason", "stop"}}})}};
                response.set_content("data: " + chunk.dump() + "\n\ndata: [DONE]\n\n", "text/event-stream");
            }
            else
            {
                response.set_content(json{{"choices", json::array({{
                    {"message", {{"role", "assistant"}, {"content", answer}}}, {"finish_reason", "stop"}}})}}.dump(),
                    "application/json");
            }
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the advisor fixture brain.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        Check(server.is_running(), "The advisor fixture brain did not start.");
    }
    ~Brain() { server.stop(); thread.join(); }
    std::string LastRequest() { std::lock_guard lock(mutex); return lastRequest; }
    int port = 0;
private:
    httplib::Server server;
    std::mutex mutex;
    std::string lastRequest;
    std::jthread thread;
};

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
    Check(!output.fail(), "Could not write the advisor fixture.");
}

void Configure(const std::filesystem::path& root, const int port)
{
    Write(root / "Config/settings.json", {
        {"activeProfile", "fixture"},
        {"llm", {{"backend", "LLamaCpp"}, {"host", "127.0.0.1"}, {"port", port},
            {"modelName", "fixture-main"}, {"autoStartServer", false}, {"visionEnabled", false},
            {"maxTokens", 256}, {"autoMaxTokens", false},
            {"modelPath", (root / "absent.gguf").string()},
            {"mediaPath", (root / "RuntimeData/Vision").string()}}},
        {"intelligence", {{"enabled", false}, {"advisor", {
            {"enabled", true}, {"dialect", "Anthropic"}, {"host", "advisor.fixture"},
            {"modelName", "claude-fixture"}, {"escalation", "ask"}, {"share", "conversation"},
            {"keyEnvironmentVariable", "REVIA_TEST_ADVISOR_KEY"}, {"sessionBudgetTurns", 2}}}}},
        {"embedding", {{"enabled", false}, {"autoStartServer", false}}},
        {"speech", {{"enabled", false}, {"backend", "WindowsSapi"}, {"speakGreeting", false},
            {"voiceDataPath", (root / "RuntimeData/Voices").string()}}},
        {"speechRecognition", {{"enabled", false}}},
        {"presence", {{"enabled", false}, {"avatarBridgeEnabled", false},
            {"externalAdaptersEnabled", false},
            {"statePath", (root / "Presence/state.json").string()},
            {"eventPath", (root / "Presence/events.jsonl").string()},
            {"inboxPath", (root / "Presence/Inbox").string()},
            {"outboxPath", (root / "Presence/Outbox").string()}}},
        {"vision", {{"enabled", false}}},
        {"perception", {{"enabled", false}}}, {"initiative", {{"enabled", false}}},
        {"bargeIn", {{"enabled", false}}},
        {"conversation", {{"archiveEnabled", false}, {"selfInquiryEnabled", false}}},
        {"image", {{"enabled", false}}}, {"resources", {{"startupSampleSeconds", 0}}},
        {"responseFilter", {{"aiReviewEnabled", false}}}
    });
    Write(root / "Config/Profiles/fixture.json", {
        {"id", "fixture"}, {"displayName", "Fixture"},
        {"systemPrompt", "You are Revia. Advisor fixture."},
        {"shouldSpeak", false}, {"memoryEnabled", false}
    });
}

void SetEnvironment(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void TestASessionConsultsWhenAskedAndHandsTheNotesToHerPrompt()
{
    ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    Brain brain;
    Configure(directory.root, brain.port);
    SetEnvironment("REVIA_TEST_ADVISOR_KEY", "env-key");
    ReviaSession session;
    Check(session.Start(), "The advisor fixture session did not start.");
    auto recorder = std::make_shared<Recorder>();
    recorder->reply = AnthropicReply("Facts: FIXTURE_NOTE a mutex locked twice deadlocks unless recursive.\nUnsure: nothing.");
    Access::Conversation(session).SetAdvisorTransport(std::make_unique<FakeTransport>(recorder));

    const auto before = session.Submit("/advisor status");
    Check(before.succeeded && before.text.find("claude-fixture") != std::string::npos &&
        before.text.find("present, from the environment variable REVIA_TEST_ADVISOR_KEY") != std::string::npos &&
        before.text.find("0 of 2 consults") != std::string::npos,
        "The status did not say where the key came from: " + before.text);

    const auto plain = session.Submit("Why does locking a mutex twice deadlock?");
    Check(plain.succeeded, "The plain turn failed: " + plain.reason);
    {
        std::lock_guard lock(recorder->mutex);
        Check(recorder->requests.empty(), "A turn nobody asked about went to the advisor under \"ask\".");
    }
    Check(brain.LastRequest().find("Advisor notes") == std::string::npos,
        "Her prompt carried advisor notes for a turn that was not consulted.");

    const auto asked = session.Submit("Ask Claude: why does locking a mutex twice deadlock?");
    Check(asked.succeeded, "The consulted turn failed: " + asked.reason);
    {
        std::lock_guard lock(recorder->mutex);
        Check(recorder->requests.size() == 1, "The asked-for consult did not send one request.");
        const HttpsRequest& request = recorder->requests.front();
        Check(request.host == "advisor.fixture" && request.path == "/v1/messages",
            "The consult did not go to the configured advisor.");
        bool keyed = false;
        for (const auto& [name, value] : request.headers)
            if (name == "x-api-key" && value == "env-key") keyed = true;
        Check(keyed, "The key from the environment did not reach the request.");
        const json body = json::parse(request.body);
        const std::string content = body["messages"][0]["content"].get<std::string>();
        Check(content.find("Ask Claude: why does locking a mutex twice deadlock?") != std::string::npos &&
            content.find("Recent conversation") != std::string::npos &&
            content.find("Person: Why does locking a mutex twice deadlock?") != std::string::npos &&
            content.find("Revia: A mutex locked twice") != std::string::npos,
            "The brief did not carry the question and the earlier turns: " + content);
        Check(content.find("You are Revia. Advisor fixture.") == std::string::npos,
            "Her own system prompt left the machine.");
    }
    const std::string prompt = brain.LastRequest();
    Check(prompt.find("# Advisor notes") != std::string::npos && prompt.find("FIXTURE_NOTE") != std::string::npos &&
        prompt.find("input, not your words") != std::string::npos,
        "The notes did not reach her prompt as input to check.");

    const auto status = session.Submit("/advisor status");
    Check(status.text.find("1 of 2 consults") != std::string::npos &&
        status.text.find("120 tokens in, 40 out") != std::string::npos &&
        status.text.find("notes from claude-fixture") != std::string::npos,
        "The status did not account for the consult: " + status.text);

    const auto off = session.Submit("/advisor off");
    Check(off.succeeded && session.Submit("ask claude again, why?").succeeded, "The advisor could not be turned off.");
    {
        std::lock_guard lock(recorder->mutex);
        Check(recorder->requests.size() == 1, "A consult was made with the advisor off.");
    }
    Check(session.Submit("/advisor on").succeeded && session.Submit("ask claude once more, why?").succeeded,
        "The advisor could not be turned back on.");
    Check(session.Submit("ask claude a third time, why?").succeeded, "The turn past the budget failed.");
    {
        std::lock_guard lock(recorder->mutex);
        Check(recorder->requests.size() == 2, "The session budget of two consults was not kept.");
    }
}
} // namespace

void RunAdvisorTests()
{
    TestTheRulesDecideAndNotTheModel();
    TestTheBriefCarriesOnlyWhatMayLeave();
    TestTheClientSpeaksBothDialectsAndReadsBothAnswers();
    TestTheKeyLivesInTheStoreOrTheEnvironmentAndNowhereElse();
    TestASessionConsultsWhenAskedAndHandsTheNotesToHerPrompt();
    std::cout << "A rule decides when a frontier model is consulted, a redacted brief is all that "
                 "leaves, its notes reach her prompt as input, and the key lives in the store or "
                 "the environment.\n";
}
