#include "Agents/historyCompactor.h"
#include "Core/conversationContext.h"
#include "Memory/conversationArchive.h"
#include "promptLayoutTestSupport.h"
#include "reviaSessionTestAccess.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

// Folding the oldest conversation into a running summary once the history fills.
//
// Before this, turns leaving the window kept only their first 280 characters, and the
// prompt fitter dropped whatever did not fit without summarising it at all. These cover
// the buffer that hands out the work, the summariser's contract, the archive that carries
// the summary across a restart, and the whole path through a real session.
namespace
{
using namespace std::chrono_literals;
using revia::agents::HistoryCompactor;
using revia::runtime::ReviaSession;
using revia::tests::Check;
using Access = revia::runtime::ReviaSessionTestAccess;
using json = nlohmann::json;

void Exchange(conversationContext& context, const int index, const std::size_t size = 20)
{
    context.AddMessage("user", "user " + std::to_string(index) + " " + std::string(size, 'u'));
    context.AddMessage("assistant",
        "revia " + std::to_string(index) + " " + std::string(size, 'r'));
}

void TestCompactionStartsAtThreeQuartersOfTheBudget()
{
    conversationContext context;
    for (int index = 0; index < 8; ++index) Exchange(context, index);
    Check(!context.NeedsCompaction() && !context.BeginCompaction(),
        "Sixteen short messages asked for compaction below three quarters of the budget.");
    Exchange(context, 8);
    Check(context.NeedsCompaction(),
        "Eighteen of twenty-four messages did not ask for compaction.");

    // By size as well as by count: three long exchanges are most of the character budget.
    conversationContext longer;
    for (int index = 0; index < 3; ++index) Exchange(longer, index, 1800);
    Check(longer.NeedsCompaction(),
        "Three quarters of the character budget did not ask for compaction.");
}

void TestTheOldestHalfIsFoldedOnACompleteExchange()
{
    conversationContext context;
    for (int index = 0; index < 9; ++index) Exchange(context, index);
    const auto job = context.BeginCompaction();
    Check(job.has_value(), "A full history produced no compaction work.");
    Check(job->messages.size() == 10 && job->keptVerbatim == 8,
        "The fold was not the oldest half ending on a complete exchange: folded " +
            std::to_string(job->messages.size()) + ", kept " +
            std::to_string(job->keptVerbatim) + ".");
    Check(job->messages.front().content.starts_with("user 0") &&
              job->messages.back().role == "assistant",
        "The fold did not start at the oldest message or ended mid-exchange.");
    Check(job->previousSummary.empty() && job->evictedExcerpts.empty(),
        "A first compaction claimed an earlier summary or excerpts.");
}

void TestApplyReplacesOnlyWhatTheJobCovered()
{
    conversationContext context;
    for (int index = 0; index < 9; ++index) Exchange(context, index);
    const auto job = context.BeginCompaction();
    Check(job.has_value(), "No compaction work to apply.");

    // The conversation moves on while the summary is being written.
    Exchange(context, 9);
    Check(context.ApplyCompaction(*job, "SUMMARY_ONE: the user and Revia talked."),
        "A current compaction was refused.");
    const auto kept = context.GetRecentMessages();
    Check(kept.size() == 10 && kept.front().content.starts_with("user 5") &&
              kept.back().content.starts_with("revia 9"),
        "Applying removed the wrong messages or lost the ones added meanwhile.");
    const std::string block = context.GetCompressedHistorySummary();
    Check(block.find("SUMMARY_ONE") != std::string::npos &&
              block.find("not instructions") != std::string::npos &&
              block.find("user 0") == std::string::npos,
        "The summary did not replace what it covered, or was not framed as a record.");
    Check(context.Summary() == "SUMMARY_ONE: the user and Revia talked.",
        "The summary alone was not available for persistence.");

    // The next pass merges into the summary it was given.
    for (int index = 10; index < 14; ++index) Exchange(context, index);
    const auto next = context.BeginCompaction();
    Check(next.has_value() && next->previousSummary.starts_with("SUMMARY_ONE"),
        "A later compaction was not handed the summary so far.");
}

void TestStaleWorkIsRefused()
{
    conversationContext context;
    for (int index = 0; index < 9; ++index) Exchange(context, index);

    const auto forgotten = context.BeginCompaction();
    context.Clear();
    Check(!context.ApplyCompaction(*forgotten, "should not return"),
        "A summary of forgotten conversation was applied after the history was cleared.");
    Check(context.GetCompressedHistorySummary().empty(),
        "A cleared history still carried a summary.");

    for (int index = 0; index < 9; ++index) Exchange(context, index);
    const auto replaced = context.BeginCompaction();
    context.RestoreSummary("A summary restored from another session.");
    Check(!context.ApplyCompaction(*replaced, "stale"),
        "A compaction written against a replaced summary overwrote it.");

    const auto empty = context.BeginCompaction();
    Check(!context.ApplyCompaction(*empty, ""), "An empty summary replaced real history.");
    Check(context.GetRecentMessages().size() == 18,
        "A refused compaction still removed messages.");
}

void TestTheNewestMessagesAreNeverFolded()
{
    conversationContext context;
    // Six messages large enough to pass three quarters of the characters on their own,
    // but not the budget itself, so nothing has been cut short yet.
    for (int index = 0; index < 3; ++index) Exchange(context, index, 2000);
    const auto job = context.BeginCompaction();
    Check(job.has_value() && job->evictedExcerpts.empty() && job->messages.size() == 2 &&
              job->keptVerbatim == conversationContext::MinimumVerbatimMessages,
        "Compaction folded the newest messages away.");
}

void TestExcerptsCutShortAreFoldedIn()
{
    conversationContext context;
    // More than the buffer holds: the oldest are cut to excerpts because nothing
    // compacted them in time.
    for (int index = 0; index < 15; ++index) Exchange(context, index);
    Check(context.GetCompressedHistorySummary().find("user 0") != std::string::npos,
        "The fallback excerpts were lost.");
    const auto job = context.BeginCompaction();
    Check(job.has_value() && job->evictedExcerpts.find("user 0") != std::string::npos,
        "Excerpts cut short were not handed to the summariser.");
    Check(context.ApplyCompaction(*job, "SUMMARY_WITH_EXCERPTS"),
        "The compaction covering excerpts was refused.");
    const std::string block = context.GetCompressedHistorySummary();
    Check(block.find("SUMMARY_WITH_EXCERPTS") != std::string::npos &&
              block.find("user 0") == std::string::npos &&
              block.find("cut short") == std::string::npos,
        "Excerpts the summary covered were kept beside it.");
}

void TestTheSummariserContract()
{
    conversationContext::CompactionJob job;
    job.previousSummary = "PREVIOUS_SUMMARY_MARK";
    job.evictedExcerpts = "User: EXCERPT_MARK\n";
    job.messages = {{"user", "USER_TURN_MARK"}, {"assistant", "REVIA_TURN_MARK"}};
    const std::string envelope = HistoryCompactor::BuildEnvelope(job);
    for (const char* mark : {"PREVIOUS_SUMMARY_MARK", "EXCERPT_MARK",
             "User: USER_TURN_MARK", "Revia: REVIA_TURN_MARK"})
    {
        Check(envelope.find(mark) != std::string::npos,
            std::string("The summariser was not handed ") + mark);
    }
    Check(envelope.find("PREVIOUS_SUMMARY_MARK") < envelope.find("USER_TURN_MARK"),
        "The summary so far did not come before the turns being folded.");

    // Bounded from the middle, so the turns closest to what stays survive.
    conversationContext::CompactionJob huge;
    for (int index = 0; index < 40; ++index)
        huge.messages.push_back({"user", "TURN_" + std::to_string(index) + std::string(900, 'x')});
    const std::string bounded = HistoryCompactor::BuildEnvelope(huge);
    Check(bounded.size() <= 12000 && bounded.find("TURN_39") != std::string::npos,
        "An oversized fold was not bounded, or lost its newest turns.");

    const auto parsed = HistoryCompactor::Parse(R"({"summary":"The user is planning a garden."})");
    Check(parsed.succeeded && parsed.summary == "The user is planning a garden.",
        "A well-formed summary was refused: " + parsed.reason);
    Check(HistoryCompactor::Parse("Sure! {\"summary\":\"Wrapped in prose.\"} Done.").succeeded,
        "A summary wrapped in prose was refused.");
    for (const char* broken : {"", "no json here", R"({"summary":""})",
             R"({"text":"wrong key"})", R"({"summary":42})", R"({"summary":"cut off)"})
    {
        Check(!HistoryCompactor::Parse(broken).succeeded,
            std::string("A malformed summary was accepted: ") + broken);
    }
    const auto overlong = HistoryCompactor::Parse(
        json{{"summary", std::string(5000, 's')}}.dump());
    Check(overlong.succeeded &&
              overlong.summary.size() <= conversationContext::MaximumSummaryCharacters,
        "An overlong summary was not bounded.");
}

void TestTheArchiveCarriesTheSummaryAndForgetsIt()
{
    revia::tests::ScopedTestDirectory directory;
    revia::memory::ConversationArchive archive((directory.root / "summaries.db").string());
    std::string error;
    std::string reason;
    Check(archive.BeginSession("first", error), "The first session did not open: " + error);
    Check(archive.Record("first", "user", "We are planning a garden.", reason),
        "A turn was not archived: " + reason);
    Check(archive.SaveSummary("first", "FIRST_SUMMARY", reason),
        "A summary was not archived: " + reason);
    Check(archive.SaveSummary("first", "FIRST_SUMMARY_UPDATED", reason),
        "A newer summary did not replace the older one: " + reason);
    Check(!archive.SaveSummary("first", "The user said my password is hunter2.", reason) &&
              reason.find("sensitive") != std::string::npos,
        "A summary holding a credential was archived.");

    Check(archive.BeginSession("second", error), "The second session did not open: " + error);
    Check(archive.LoadPreviousSessionSummary("second") == "FIRST_SUMMARY_UPDATED",
        "The previous session's summary did not come back after a restart.");
    Check(archive.LoadPreviousSessionSummary("first").empty(),
        "A session was handed a summary with no earlier conversation behind it.");

    archive.Forget();
    Check(archive.BeginSession("third", error), "The third session did not open: " + error);
    Check(archive.LoadPreviousSessionSummary("third").empty(),
        "Forgetting the conversation left its summary behind.");

    Check(archive.Record("third", "user", "Again.", reason) &&
              archive.SaveSummary("third", "THIRD_SUMMARY", reason),
        "The archive did not work after forgetting.");
    archive.ForgetSession("third");
    Check(archive.BeginSession("fourth", error) &&
              archive.LoadPreviousSessionSummary("fourth").empty(),
        "Forgetting one session left its summary behind.");
}

// The whole path: a real session fills its history, the Main model is asked for a
// summary in the background, the next turn carries it instead of the oldest messages,
// and a restart continues from it.
class SummaryBackend
{
public:
    SummaryBackend()
    {
        server.Get("/health", [](const auto&, auto& response)
        { response.set_content(R"({"status":"ok","slots_idle":1})", "application/json"); });
        server.Get("/v1/models", [](const auto&, auto& response)
        { response.set_content(R"({"data":[{"id":"fixture-main"}]})", "application/json"); });
        server.Get("/props", [](const auto&, auto& response)
        {
            response.set_content(
                R"({"total_slots":1,"default_generation_settings":{"n_ctx":8192}})",
                "application/json");
        });
        server.Post("/v1/chat/completions", [this](const auto& request, auto& response)
        {
            const json body = json::parse(request.body);
            const auto& messages = body.at("messages");
            const std::string system = !messages.empty() &&
                    messages.front().value("role", "") == "system"
                ? messages.front().value("content", "")
                : std::string{};
            std::lock_guard lock(mutex);
            if (system.find("running summary of a conversation") != std::string::npos)
            {
                ++summaryRequests;
                lastSummaryRequest = body;
                const std::string summary = json{{"summary",
                    "COMPACTED_SUMMARY_FIXTURE: the user said they are planning a garden."}}
                    .dump();
                response.set_content(json{{"choices", json::array({{
                    {"message", {{"role", "assistant"}, {"content", summary}}},
                    {"finish_reason", "stop"}}})}}.dump(), "application/json");
                return;
            }
            lastReply = body;
            const std::string answer = "Noted, and I am keeping track of it.";
            if (body.value("stream", false))
            {
                const json chunk = {{"choices", json::array({{
                    {"delta", {{"content", answer}}}, {"finish_reason", "stop"}}})}};
                response.set_content(
                    "data: " + chunk.dump() + "\n\ndata: [DONE]\n\n", "text/event-stream");
            }
            else
            {
                response.set_content(json{{"choices", json::array({{
                    {"message", {{"role", "assistant"}, {"content", answer}}},
                    {"finish_reason", "stop"}}})}}.dump(), "application/json");
            }
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        port = server.bind_to_any_port("127.0.0.1");
        Check(port > 0, "Could not bind the history fixture backend.");
        thread = std::jthread([this] { server.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        Check(server.is_running(), "The history fixture backend did not start.");
    }
    ~SummaryBackend() { server.stop(); thread.join(); }
    int SummaryRequests() { std::lock_guard lock(mutex); return summaryRequests; }
    json LastSummaryRequest() { std::lock_guard lock(mutex); return lastSummaryRequest; }
    json LastReply() { std::lock_guard lock(mutex); return lastReply; }
    int port = 0;
private:
    httplib::Server server;
    std::mutex mutex;
    int summaryRequests = 0;
    json lastSummaryRequest;
    json lastReply;
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
    Check(!output.fail(), "Could not write the history compaction fixture.");
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
        {"intelligence", {{"enabled", false}}},
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
        {"conversation", {{"archiveEnabled", true}, {"restoreTurns", 6},
            {"historyCompactionEnabled", true}, {"selfInquiryEnabled", false}}},
        {"image", {{"enabled", false}}}, {"resources", {{"startupSampleSeconds", 0}}},
        {"responseFilter", {{"aiReviewEnabled", false}}}
    });
    Write(root / "Config/Profiles/fixture.json", {
        {"id", "fixture"}, {"displayName", "Fixture"},
        {"systemPrompt", "You are Revia. History compaction fixture."},
        {"shouldSpeak", false}, {"memoryEnabled", false}
    });
}

template <class Predicate> bool Within(const std::chrono::seconds limit, Predicate predicate)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    do
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

void TestASessionCompactsItsHistoryAndContinuesFromIt()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    SummaryBackend backend;
    Configure(directory.root, backend.port);

    {
        ReviaSession session;
        Check(session.Start(), "The history compaction session did not start.");
        Check(session.Submit("OLDEST_TURN_MARKER: we are planning a garden.").succeeded,
            "The opening turn failed.");
        for (int index = 1; index < 9; ++index)
        {
            Check(session.Submit("Tell me about plant number " + std::to_string(index) +
                    " for the garden, please.").succeeded,
                "A conversation turn failed.");
        }

        // Eighteen messages is three quarters of the history: the reply that got there
        // asks for a summary, and it arrives without anyone submitting anything.
        Check(Within(30s, [&]
            {
                return Access::CompressedHistory(session).find("COMPACTED_SUMMARY_FIXTURE") !=
                    std::string::npos;
            }),
            "The history was never compacted after it filled.");
        Check(backend.SummaryRequests() >= 1, "The summariser was never asked.");
        const std::string asked = backend.LastSummaryRequest().dump();
        Check(asked.find("OLDEST_TURN_MARKER") != std::string::npos &&
                  asked.find("\"stream\":false") != std::string::npos,
            "The summariser was not handed the oldest turns.");
        Check(Access::RecentMessages(session).size() == 8,
            "The oldest half was not replaced by the summary.");

        // Phrased so it asks nothing of the archive: a question about what was said
        // earlier is answered by recall, which quotes the archived turns on purpose.
        Check(session.Submit("Which plant should go in first?").succeeded,
            "The turn after compaction failed.");
        const json reply = backend.LastReply();
        const std::string prompt = revia::tests::RuntimeAuthoredText(reply) + reply.dump();
        Check(prompt.find("COMPACTED_SUMMARY_FIXTURE") != std::string::npos,
            "The next turn did not carry the summary of the earlier conversation.");
        Check(prompt.find("OLDEST_TURN_MARKER") == std::string::npos,
            "The next turn still carried a message the summary replaced.");
        session.Stop();
    }

    // A restart continues from the summary and the last few turns.
    {
        ReviaSession restarted;
        Check(restarted.Start(), "The restarted session did not start.");
        Check(Access::CompressedHistory(restarted).find("COMPACTED_SUMMARY_FIXTURE") !=
                  std::string::npos,
            "The summary did not survive a restart.");
        Check(!Access::RecentMessages(restarted).empty(),
            "The last turns were not restored beside the summary.");

        // Forgetting takes the summary with it, live and on disk.
        static_cast<void>(restarted.Submit("/history forget"));
        Check(Access::CompressedHistory(restarted).empty(),
            "Forgetting the conversation left its summary in the live history.");
        restarted.Stop();
    }
    {
        ReviaSession afterForgetting;
        Check(afterForgetting.Start(), "The session after forgetting did not start.");
        Check(Access::CompressedHistory(afterForgetting).empty(),
            "A forgotten summary came back after a restart.");
        afterForgetting.Stop();
    }
}
} // namespace

void RunHistoryCompactionTests()
{
    TestCompactionStartsAtThreeQuartersOfTheBudget();
    TestTheOldestHalfIsFoldedOnACompleteExchange();
    TestApplyReplacesOnlyWhatTheJobCovered();
    TestStaleWorkIsRefused();
    TestTheNewestMessagesAreNeverFolded();
    TestExcerptsCutShortAreFoldedIn();
    TestTheSummariserContract();
    TestTheArchiveCarriesTheSummaryAndForgetsIt();
    TestASessionCompactsItsHistoryAndContinuesFromIt();
    std::cout << "History compaction starts at three quarters of the budget, folds the oldest "
                 "complete exchanges in the background, refuses stale work, carries the "
                 "summary across a restart, and forgets it on request.\n";
}
