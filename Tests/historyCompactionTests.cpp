#include "Agents/historyCompactor.h"
#include "Core/conversationContext.h"
#include "Memory/conversationArchive.h"
#include "Memory/observationLog.h"
#include "Memory/temporalQuery.h"
#include "promptLayoutTestSupport.h"
#include "reviaSessionTestAccess.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

// The record of a conversation's earlier part, kept as dated observations.
//
// Before this, turns leaving the window kept only their first 280 characters, and the
// prompt fitter dropped whatever did not fit without recording it at all. A first
// replacement folded them into one running summary that the model rewrote every time it
// grew, which loses detail with each pass. These cover the log that only appends and
// supersedes, the buffer that hands out the work, the observer's and reflector's
// contracts, the archive that carries the record across a restart, and the whole path
// through a real session.
namespace
{
using namespace std::chrono_literals;
using revia::agents::HistoryCompactor;
using revia::memory::Observation;
using revia::memory::ObservationLog;
using revia::runtime::ReviaSession;
using revia::tests::Check;
using Access = revia::runtime::ReviaSessionTestAccess;
using json = nlohmann::json;

Observation Note(const std::string& text, const int priority = 2, const std::string& kind = "fact")
{
    Observation observation;
    observation.text = text;
    observation.priority = priority;
    observation.kind = kind;
    observation.observedAt = revia::memory::CurrentEpoch();
    return observation;
}

void Exchange(conversationContext& context, const int index, const std::size_t size = 20)
{
    context.AddMessage("user", "user " + std::to_string(index) + " " + std::string(size, 'u'));
    context.AddMessage("assistant",
        "revia " + std::to_string(index) + " " + std::string(size, 'r'));
}

void TestTheLogOnlyAppendsAndSupersedes()
{
    ObservationLog log;
    const std::uint64_t first = log.Append(Note("The user's name is Quentin.", 3));
    const std::uint64_t second = log.Append(Note("The user is planning a garden.", 2));
    const std::uint64_t third = log.Append(Note("The user prefers tomatoes.", 1));
    Check(first == 1 && second == 2 && third == 3 && log.CurrentCount() == 3,
        "Observations were not appended with rising ids.");
    Check(log.Append(Note("")) == 0 && log.CurrentCount() == 3,
        "An empty observation was recorded.");

    Check(log.Supersede({second}, Note("a merge of one")) == 0,
        "A merge of a single observation, which is a rewrite, was accepted.");
    Check(log.Supersede({second, 99}, Note("a merge with a missing id")) == 0,
        "A merge naming an unknown observation was accepted.");
    const std::uint64_t merged =
        log.Supersede({second, third}, Note("The user is planning a tomato garden.", 1));
    Check(merged == 4 && log.CurrentCount() == 2,
        "A valid merge did not replace the observations it named.");
    const std::vector<Observation> current = log.Current();
    Check(current.size() == 2 && current.front().id == first && current.back().id == merged,
        "The current view did not show the merged observation in place of its parts.");
    Check(current.back().priority == 2,
        "A merge did not keep the highest priority of what it replaced.");
    Check(log.All().size() == 4 && log.All()[1].supersededBy == merged,
        "The superseded observations lost their link to what replaced them.");
    Check(log.Supersede({second, merged}, Note("again")) == 0,
        "A superseded observation was merged a second time.");
}

void TestRenderingFitsTheBudgetByDroppingTheLeastImportantFirst()
{
    ObservationLog log;
    const std::int64_t now = revia::memory::CurrentEpoch();
    log.Append(Note("MINOR_OLD " + std::string(100, 'a'), 1));
    log.Append(Note("IMPORTANT_OLD " + std::string(100, 'b'), 3));
    log.Append(Note("ORDINARY_OLD " + std::string(100, 'c'), 2));
    log.Append(Note("MINOR_NEW " + std::string(100, 'd'), 1));
    log.Append(Note("ORDINARY_NEW " + std::string(100, 'e'), 2));
    const std::string all = log.Render(10000, now);
    Check(all.find("MINOR_OLD") < all.find("IMPORTANT_OLD") &&
              all.find("IMPORTANT_OLD") < all.find("ORDINARY_NEW"),
        "The record was not rendered oldest first.");
    Check(all.find("(a few minutes ago)") != std::string::npos,
        "Observations were rendered without saying when they were made.");

    // Room for about three lines: both minor lines go first, then the oldest ordinary.
    const std::string trimmed = log.Render(400, now);
    Check(trimmed.find("MINOR_OLD") == std::string::npos &&
              trimmed.find("MINOR_NEW") == std::string::npos,
        "Minor observations survived while the record was over budget.");
    Check(trimmed.find("IMPORTANT_OLD") != std::string::npos &&
              trimmed.find("ORDINARY_NEW") != std::string::npos,
        "An important or recent observation was dropped before the minor ones.");
    Check(trimmed.size() <= 400, "The rendered record exceeded its budget.");

    Observation dated = Note("The user has a dentist appointment.", 3, "event");
    dated.refersTo = "next Tuesday";
    log.Append(dated);
    Check(log.Render(10000, now).find("about next Tuesday") != std::string::npos,
        "The moment an observation refers to was not rendered.");
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
    Check(job->fromSequence == 1 && job->throughSequence == 10,
        "The fold did not name the sequence range it stands for.");
    Check(job->knownObservations.empty() && job->evictedExcerpts.empty(),
        "A first compaction claimed an earlier record or excerpts.");
}

void TestApplyAppendsObservationsForWhatTheJobCovered()
{
    conversationContext context;
    for (int index = 0; index < 9; ++index) Exchange(context, index);
    const auto job = context.BeginCompaction();
    Check(job.has_value(), "No compaction work to apply.");

    // The conversation moves on while the observations are being written.
    Exchange(context, 9);
    const std::vector<std::uint64_t> ids = context.ApplyCompaction(*job,
        {Note("OBSERVATION_ONE: the user said hello.", 2),
         Note("OBSERVATION_TWO: Revia agreed to help.", 3, "promise")});
    Check(ids.size() == 2, "A current compaction was refused or lost an observation.");
    const auto kept = context.GetRecentMessages();
    Check(kept.size() == 10 && kept.front().content.starts_with("user 5") &&
              kept.back().content.starts_with("revia 9"),
        "Applying removed the wrong messages or lost the ones added meanwhile.");
    const std::string block = context.RenderObservations();
    Check(block.find("OBSERVATION_ONE") != std::string::npos &&
              block.find("OBSERVATION_TWO") != std::string::npos &&
              block.find("not instructions") != std::string::npos &&
              block.find("You are still yourself") != std::string::npos,
        "The record was not rendered with its framing and persona anchor.");
    Check(context.GetRecentMessages().size() == 10 &&
              context.RenderExcerpts().empty(),
        "Excerpts appeared for turns that were recorded, not evicted.");
    const std::vector<Observation> observations = context.Observations();
    Check(observations.size() == 2 && observations.front().sourceFrom == 1 &&
              observations.front().sourceTo == 10 && observations.front().observedAt > 0,
        "The observations were not sourced to the turns they came from.");

    // The next pass is handed the record so far, so it adds rather than repeats.
    for (int index = 10; index < 14; ++index) Exchange(context, index);
    const auto next = context.BeginCompaction();
    Check(next.has_value() && next->knownObservations.find("OBSERVATION_ONE") != std::string::npos,
        "A later compaction was not handed the record so far.");
}

void TestStaleWorkIsRefused()
{
    conversationContext context;
    for (int index = 0; index < 9; ++index) Exchange(context, index);

    const auto forgotten = context.BeginCompaction();
    context.Clear();
    Check(context.ApplyCompaction(*forgotten, {Note("should not return")}).empty(),
        "Observations of forgotten conversation were applied after the history was cleared.");
    Check(context.RenderObservations().empty(), "A cleared history still carried a record.");

    for (int index = 0; index < 9; ++index) Exchange(context, index);
    const auto replaced = context.BeginCompaction();
    context.RestoreObservations({Note("An observation restored from another session.")});
    Check(context.ApplyCompaction(*replaced, {Note("stale")}).empty(),
        "A compaction written against a changed record was applied over it.");

    const auto empty = context.BeginCompaction();
    Check(context.ApplyCompaction(*empty, {}).empty() &&
              context.ApplyCompaction(*empty, {Note("")}).empty(),
        "An empty result removed real history.");
    Check(context.GetRecentMessages().size() == 18,
        "A refused compaction still removed messages.");
}

void TestReflectionMergesOnlyCurrentObservations()
{
    conversationContext context;
    for (int index = 0; index < 9; ++index) Exchange(context, index);
    const auto job = context.BeginCompaction();
    std::vector<Observation> many;
    for (int index = 0; index < 40; ++index)
    {
        many.push_back(Note("NOTE_" + std::to_string(index) + " " + std::string(60, 'n')));
    }
    Check(context.ApplyCompaction(*job, many).size() == 40, "The observations were not applied.");
    Check(context.NeedsReflection(), "Forty observations did not ask for reflection.");

    const auto reflection = context.BeginReflection();
    Check(reflection.has_value() && reflection->observations.size() == 40,
        "The reflection job did not carry the current record.");

    conversationContext::Merge good;
    good.replaces = {1, 2};
    good.merged = Note("MERGED_ONE_TWO", 2);
    conversationContext::Merge stale;
    stale.replaces = {2, 3};
    stale.merged = Note("MERGE_NAMING_A_TAKEN_ID", 2);
    conversationContext::Merge unknown;
    unknown.replaces = {5, 999};
    unknown.merged = Note("MERGE_NAMING_A_MISSING_ID", 2);
    const auto applied = context.ApplyReflection(*reflection, {good, stale, unknown});
    Check(applied.size() == 1 && applied.front().merged.id == 41 &&
              applied.front().replaces == std::vector<std::uint64_t>{1, 2},
        "Reflection applied the wrong merges.");
    // Read from the log itself: the rendered view is budgeted and may leave out older
    // ordinary lines, which is not what this checks.
    const std::vector<Observation> after = context.Observations();
    Check(after.size() == 41 && after[0].supersededBy == 41 && after[1].supersededBy == 41 &&
              after[2].supersededBy == 0 && after[40].text == "MERGED_ONE_TWO" &&
              after[40].Current(),
        "The merge did not replace exactly what it named.");
    Check(context.RenderObservations().find("MERGED_ONE_TWO") != std::string::npos,
        "The merged observation was not rendered.");

    // A job taken before the merge is stale afterwards.
    Check(context.ApplyReflection(*reflection, {good}).empty(),
        "A reflection was applied twice.");
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
    // recorded them in time.
    for (int index = 0; index < 15; ++index) Exchange(context, index);
    Check(context.RenderExcerpts().find("user 0") != std::string::npos &&
              context.GetCompressedHistorySummary().find("user 0") != std::string::npos,
        "The fallback excerpts were lost.");
    const auto job = context.BeginCompaction();
    Check(job.has_value() && job->evictedExcerpts.find("user 0") != std::string::npos &&
              job->fromSequence == 1,
        "Excerpts cut short were not handed to the observer.");
    Check(!context.ApplyCompaction(*job, {Note("RECORDED_WITH_EXCERPTS")}).empty(),
        "The compaction covering excerpts was refused.");
    Check(context.RenderObservations().find("RECORDED_WITH_EXCERPTS") != std::string::npos &&
              context.RenderExcerpts().empty(),
        "Excerpts the record covered were kept beside it.");
}

void TestTheObserverContract()
{
    conversationContext::CompactionJob job;
    job.knownObservations = "- KNOWN_MARK\n";
    job.evictedExcerpts = "User: EXCERPT_MARK\n";
    job.messages = {{"user", "USER_TURN_MARK"}, {"assistant", "REVIA_TURN_MARK"}};
    const std::string envelope = HistoryCompactor::BuildObserverEnvelope(job);
    for (const char* mark : {"KNOWN_MARK", "EXCERPT_MARK",
             "User: USER_TURN_MARK", "Revia: REVIA_TURN_MARK"})
    {
        Check(envelope.find(mark) != std::string::npos,
            std::string("The observer was not handed ") + mark);
    }
    Check(envelope.find("KNOWN_MARK") < envelope.find("USER_TURN_MARK"),
        "The record so far did not come before the turns being folded.");

    // Bounded from the middle, so the turns closest to what stays survive.
    conversationContext::CompactionJob huge;
    for (int index = 0; index < 40; ++index)
        huge.messages.push_back({"user", "TURN_" + std::to_string(index) + std::string(900, 'x')});
    const std::string bounded = HistoryCompactor::BuildObserverEnvelope(huge);
    Check(bounded.size() <= 12000 && bounded.find("TURN_39") != std::string::npos,
        "An oversized fold was not bounded, or lost its newest turns.");

    const auto parsed = HistoryCompactor::ParseObservations(
        R"({"observations":[
            {"text":"The user said they are planning a garden.","priority":3,"kind":"fact","refers_to":""},
            {"text":"The user said they are planning a garden.","priority":3,"kind":"fact","refers_to":""},
            {"text":"Revia promised to remind them on Friday.","priority":3,"kind":"promise","refers_to":"Friday"},
            {"text":"The user said my password is hunter2.","priority":1,"kind":"fact","refers_to":""},
            {"text":"","priority":2,"kind":"fact","refers_to":""},
            {"text":"An unknown kind.","priority":9,"kind":"whatever","refers_to":""}]})");
    Check(parsed.succeeded && parsed.observations.size() == 3,
        "The observer's output was not read as three usable observations: " + parsed.reason);
    Check(parsed.observations[1].kind == "promise" && parsed.observations[1].refersTo == "Friday" &&
              parsed.observations[1].priority == 3,
        "An observation lost its kind, priority or the moment it refers to.");
    Check(parsed.observations[2].kind == "fact" && parsed.observations[2].priority == 3,
        "An unknown kind or out-of-range priority was not brought back into bounds.");
    Check(HistoryCompactor::ParseObservations(R"(Sure! {"observations":[]} Done.)").succeeded,
        "An empty list wrapped in prose was refused.");
    for (const char* broken : {"", "no json here", R"({"observations":"x"})",
             R"({"summary":"the old shape"})", R"({"observations":[{"text":"cut off)"})
    {
        Check(!HistoryCompactor::ParseObservations(broken).succeeded,
            std::string("A malformed observer result was accepted: ") + broken);
    }
}

void TestTheReflectorContract()
{
    conversationContext::ReflectionJob job;
    Observation first = Note("FIRST_MARK");
    first.id = 3;
    Observation second = Note("SECOND_MARK");
    second.id = 7;
    second.refersTo = "next week";
    job.observations = {first, second};
    const std::string envelope = HistoryCompactor::BuildReflectorEnvelope(job);
    Check(envelope.find("3. FIRST_MARK") != std::string::npos &&
              envelope.find("7. SECOND_MARK (about next week)") != std::string::npos,
        "The reflector was not handed the numbered record.");

    const auto parsed = HistoryCompactor::ParseMerges(
        R"({"merged":[
            {"text":"MERGE_A","priority":2,"kind":"fact","replaces":[3,7]},
            {"text":"MERGE_B_REUSES_7","priority":2,"kind":"fact","replaces":[7,9]},
            {"text":"MERGE_C_OF_ONE","priority":2,"kind":"fact","replaces":[11]},
            {"text":"MERGE_D","priority":1,"kind":"topic","replaces":[12,13,13]}]})");
    Check(parsed.succeeded && parsed.merges.size() == 2,
        "The reflector's output was not read as two usable merges: " + parsed.reason);
    Check(parsed.merges[0].replaces == std::vector<std::uint64_t>{3, 7} &&
              parsed.merges[1].replaces == std::vector<std::uint64_t>{12, 13},
        "Merges did not keep the ids they replace, without repeats.");
    Check(!HistoryCompactor::ParseMerges(R"({"observations":[]})").succeeded,
        "The observer's shape was accepted as a reflection.");
}

void TestTheArchiveCarriesTheRecordAndForgetsIt()
{
    revia::tests::ScopedTestDirectory directory;
    revia::memory::ConversationArchive archive((directory.root / "record.db").string());
    std::string error;
    std::string reason;
    Check(archive.BeginSession("first", error), "The first session did not open: " + error);
    Check(archive.Record("first", "user", "We are planning a garden.", reason),
        "A turn was not archived: " + reason);
    Observation one = Note("FIRST_OBSERVATION", 3);
    one.id = 1;
    Observation two = Note("SECOND_OBSERVATION", 2);
    two.id = 2;
    two.refersTo = "Friday";
    Observation secret = Note("The user said my password is hunter2.");
    secret.id = 3;
    Check(archive.SaveObservation("first", one, reason) && archive.SaveObservation("first", two, reason),
        "An observation was not archived: " + reason);
    Check(!archive.SaveObservation("first", secret, reason) &&
              reason.find("sensitive") != std::string::npos,
        "An observation holding a credential was archived.");
    Observation merged = Note("MERGED_OBSERVATION", 3);
    merged.id = 4;
    Check(archive.SaveObservation("first", merged, reason) &&
              archive.MarkObservationSuperseded("first", 1, 4) &&
              archive.MarkObservationSuperseded("first", 2, 4),
        "A merge was not recorded.");
    Check(!archive.MarkObservationSuperseded("first", 77, 4),
        "Marking an unknown observation as superseded reported success.");

    Check(archive.BeginSession("second", error), "The second session did not open: " + error);
    const std::vector<Observation> restored = archive.LoadPreviousSessionObservations("second");
    Check(restored.size() == 1 && restored.front().text == "MERGED_OBSERVATION" &&
              restored.front().priority == 3,
        "The previous session's current record did not come back after a restart.");
    Check(archive.LoadPreviousSessionObservations("first").empty(),
        "A session was handed a record with no earlier conversation behind it.");

    archive.Forget();
    Check(archive.BeginSession("third", error), "The third session did not open: " + error);
    Check(archive.LoadPreviousSessionObservations("third").empty(),
        "Forgetting the conversation left its record behind.");

    Observation third = Note("THIRD_OBSERVATION");
    third.id = 1;
    Check(archive.Record("third", "user", "Again.", reason) &&
              archive.SaveObservation("third", third, reason),
        "The archive did not work after forgetting.");
    archive.ForgetSession("third");
    Check(archive.BeginSession("fourth", error) &&
              archive.LoadPreviousSessionObservations("fourth").empty(),
        "Forgetting one session left its record behind.");
}

// The whole path: a real session fills its history, the Main model is asked to record
// the oldest turns in the background, the next turn carries the record in its stable
// prefix instead of the oldest messages, the record is tidied once it is long, and a
// restart continues from it.
class RecordBackend
{
public:
    RecordBackend()
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
            const auto answerWith = [&](const std::string& answer)
            {
                response.set_content(json{{"choices", json::array({{
                    {"message", {{"role", "assistant"}, {"content", answer}}},
                    {"finish_reason", "stop"}}})}}.dump(), "application/json");
            };
            std::lock_guard lock(mutex);
            if (system.find("You keep the record of a conversation") != std::string::npos)
            {
                ++observerRequests;
                lastObserverRequest = body;
                // Eight long observations per pass, so three passes push the record past
                // the point where it is tidied.
                json observations = json::array();
                for (int index = 0; index < 8; ++index)
                {
                    const int number = observationCounter++;
                    observations.push_back({
                        {"text", "OBSERVED_FIXTURE_" + std::to_string(number) +
                            ": the user said they are planning a garden " + std::string(160, 'g')},
                        {"priority", index == 0 ? 3 : 2}, {"kind", "fact"}, {"refers_to", ""}});
                }
                answerWith(json{{"observations", observations}}.dump());
                return;
            }
            if (system.find("You tidy the record of a conversation") != std::string::npos)
            {
                ++reflectorRequests;
                answerWith(json{{"merged", json::array({{
                    {"text", "MERGED_FIXTURE: the user is planning a garden."},
                    {"priority", 3}, {"kind", "fact"}, {"replaces", json::array({1, 2})}}})}}.dump());
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
                answerWith(answer);
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
    ~RecordBackend() { server.stop(); thread.join(); }
    int ObserverRequests() { std::lock_guard lock(mutex); return observerRequests; }
    int ReflectorRequests() { std::lock_guard lock(mutex); return reflectorRequests; }
    json LastObserverRequest() { std::lock_guard lock(mutex); return lastObserverRequest; }
    json LastReply() { std::lock_guard lock(mutex); return lastReply; }
    int port = 0;
private:
    httplib::Server server;
    std::mutex mutex;
    int observerRequests = 0;
    int reflectorRequests = 0;
    int observationCounter = 0;
    json lastObserverRequest;
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

std::string SystemMessageOf(const json& request)
{
    const auto& messages = request.at("messages");
    return !messages.empty() && messages.front().value("role", "") == "system"
        ? messages.front().value("content", "")
        : std::string{};
}

void TestASessionRecordsItsHistoryAndContinuesFromIt()
{
    revia::tests::ScopedTestDirectory directory;
    WorkingDirectory cwd(directory.root);
    RecordBackend backend;
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
        // asks for the record, and it arrives without anyone submitting anything.
        Check(Within(30s, [&]
            {
                return Access::CompressedHistory(session).find("OBSERVED_FIXTURE_0") !=
                    std::string::npos;
            }),
            "The history was never recorded after it filled.");
        Check(backend.ObserverRequests() >= 1, "The observer was never asked.");
        const std::string asked = backend.LastObserverRequest().dump();
        Check(asked.find("OLDEST_TURN_MARKER") != std::string::npos &&
                  asked.find("\"stream\":false") != std::string::npos,
            "The observer was not handed the oldest turns.");
        Check(Access::RecentMessages(session).size() == 8,
            "The oldest half was not replaced by the record.");

        // Phrased so it asks nothing of the archive: a question about what was said
        // earlier is answered by recall, which quotes the archived turns on purpose.
        Check(session.Submit("Which plant should go in first?").succeeded,
            "The turn after compaction failed.");
        const json reply = backend.LastReply();
        const std::string system = SystemMessageOf(reply);
        Check(system.find("OBSERVED_FIXTURE_0") != std::string::npos &&
                  system.find("You are still yourself") != std::string::npos,
            "The next turn did not carry the record in its stable system prefix.");
        Check(revia::tests::RuntimeBlockOf(reply.at("messages").back().value("content", ""))
                      .find("OBSERVED_FIXTURE_0") == std::string::npos,
            "The record was repeated in the per-turn block, where it would be re-evaluated every turn.");
        Check(reply.dump().find("OLDEST_TURN_MARKER") == std::string::npos,
            "The next turn still carried a message the record replaced.");
        Check(session.Submit("And after that one?").succeeded, "A further turn failed.");
        Check(SystemMessageOf(backend.LastReply()) == system,
            "The system prefix changed between two turns with no change to the record.");

        // Two more fills push the record past the tidying threshold.
        for (int index = 10; index < 20; ++index)
        {
            Check(session.Submit("Plant number " + std::to_string(index) +
                    " for the garden, please.").succeeded,
                "A later conversation turn failed.");
        }
        Check(Within(30s, [&]
            {
                return Access::CompressedHistory(session).find("MERGED_FIXTURE") !=
                    std::string::npos;
            }),
            "The record was never tidied after it grew long.");
        Check(backend.ReflectorRequests() >= 1, "The reflector was never asked.");
        const std::string tidied = Access::CompressedHistory(session);
        Check(tidied.find("OBSERVED_FIXTURE_0:") == std::string::npos &&
                  tidied.find("OBSERVED_FIXTURE_1:") == std::string::npos,
            "A superseded observation was still rendered.");
        const std::vector<Observation> all = Access::Observations(session);
        Check(all.size() >= 25 && all[0].text.starts_with("OBSERVED_FIXTURE_0") &&
                  all[0].supersededBy != 0 && all[1].supersededBy != 0 &&
                  all[2].supersededBy == 0 && all.back().text.starts_with("MERGED_FIXTURE"),
            "The merge did not replace exactly the observations it named.");
        session.Stop();
    }

    // A restart continues from the current record and the last few turns.
    {
        ReviaSession restarted;
        Check(restarted.Start(), "The restarted session did not start.");
        const std::string record = Access::CompressedHistory(restarted);
        const std::vector<Observation> carried = Access::Observations(restarted);
        const auto holds = [&carried](const std::string& prefix)
        {
            return std::any_of(carried.begin(), carried.end(),
                [&prefix](const Observation& observation)
                {
                    return observation.Current() && observation.text.starts_with(prefix);
                });
        };
        Check(record.find("MERGED_FIXTURE") != std::string::npos && holds("MERGED_FIXTURE") &&
                  holds("OBSERVED_FIXTURE_2:") && !holds("OBSERVED_FIXTURE_0:") &&
                  !holds("OBSERVED_FIXTURE_1:"),
            "The current record did not survive a restart, or a superseded line came back.");
        Check(!Access::RecentMessages(restarted).empty(),
            "The last turns were not restored beside the record.");

        // Forgetting takes the record with it, live and on disk.
        static_cast<void>(restarted.Submit("/history forget"));
        Check(Access::CompressedHistory(restarted).empty(),
            "Forgetting the conversation left its record in the live history.");
        restarted.Stop();
    }
    {
        ReviaSession afterForgetting;
        Check(afterForgetting.Start(), "The session after forgetting did not start.");
        Check(Access::CompressedHistory(afterForgetting).empty(),
            "A forgotten record came back after a restart.");
        afterForgetting.Stop();
    }
}
} // namespace

void RunHistoryCompactionTests()
{
    TestTheLogOnlyAppendsAndSupersedes();
    TestRenderingFitsTheBudgetByDroppingTheLeastImportantFirst();
    TestCompactionStartsAtThreeQuartersOfTheBudget();
    TestTheOldestHalfIsFoldedOnACompleteExchange();
    TestApplyAppendsObservationsForWhatTheJobCovered();
    TestStaleWorkIsRefused();
    TestReflectionMergesOnlyCurrentObservations();
    TestTheNewestMessagesAreNeverFolded();
    TestExcerptsCutShortAreFoldedIn();
    TestTheObserverContract();
    TestTheReflectorContract();
    TestTheArchiveCarriesTheRecordAndForgetsIt();
    TestASessionRecordsItsHistoryAndContinuesFromIt();
    std::cout << "The conversation record only appends and supersedes, starts at three "
                 "quarters of the budget, folds the oldest complete exchanges in the "
                 "background, sits in the stable prompt prefix, refuses stale work, is "
                 "tidied once long, survives a restart, and is forgotten on request.\n";
}
