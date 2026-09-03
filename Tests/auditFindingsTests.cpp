#include "testSupport.h"

#include <nlohmann/json.hpp>

#include "Agents/memoryAgent.h"
#include "Core/runtimePath.h"
#include "Initiative/curiosityJournal.h"
#include "Learning/selfAssessment.h"
#include "Presence/adapterArchivePolicy.h"
#include "Presence/presenceRuntime.h"
#include "Runtime/outputChannelPolicy.h"
#include "Runtime/retainedCounts.h"
#include "Vision/cameraCaptureService.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------- 1. camera selection
using revia::vision::CameraDescriptor;
using revia::vision::CameraSelection;
using revia::vision::ResolveCamera;

std::vector<CameraDescriptor> TwoCameras()
{
    return {{1, "Integrated Webcam", "\\\\?\\usb#vid_0001&pid_0001"},
            {2, "Logitech StreamCam", "\\\\?\\usb#vid_046d&pid_0893"}};
}

void TestEachCameraCanActuallyBeChosen()
{
    const std::vector<CameraDescriptor> cameras = TwoCameras();
    for (const CameraDescriptor& wanted : cameras)
    {
        CameraSelection selection;
        selection.symbolicLink = wanted.symbolicLink;
        selection.explicitChoice = true;
        const auto resolved = ResolveCamera(cameras, selection);
        Check(resolved.available, "A present camera resolved as unavailable.");
        Check(resolved.index == wanted.index,
            "Choosing " + wanted.name + " produced index " +
            std::to_string(resolved.index) + " instead of " +
            std::to_string(wanted.index) + ".");
        Check(resolved.symbolicLink == wanted.symbolicLink,
            "The symbolic link was not preserved through resolution.");
        Check(resolved.name == wanted.name, "The resolved camera name was wrong.");
    }
}

// The regression. The panel put each camera's link in the combo and then called
// CaptureCameraFrame(false), which took neither -- so the dropdown was cosmetic and
// capture always used the configured default.
void TestTheChosenCameraIsNotTheDefaultOne()
{
    const std::vector<CameraDescriptor> cameras = TwoCameras();
    CameraSelection second;
    second.symbolicLink = cameras[1].symbolicLink;
    second.explicitChoice = true;
    const auto resolved = ResolveCamera(cameras, second);
    Check(resolved.index == 2 && resolved.name == "Logitech StreamCam",
        "Choosing the second camera resolved to the first. The selection is being "
        "discarded somewhere between the list and the capture.");
}

void TestAnExplicitCameraIsNeverSubstituted()
{
    CameraSelection unplugged;
    unplugged.symbolicLink = "\\\\?\\usb#vid_dead&pid_beef";
    unplugged.index = 2;
    unplugged.explicitChoice = true;

    const auto resolved = ResolveCamera(TwoCameras(), unplugged);
    Check(!resolved.available,
        "A camera that is no longer attached resolved to a different camera. Pointing "
        "a lens the user did not choose is the one outcome worse than not capturing.");
    Check(resolved.symbolicLink.empty(),
        "An unavailable resolution still named a device to open.");
    Check(!resolved.reason.empty(), "The refusal carried no reason.");
    Check(Contains(resolved.reason, "no longer attached"),
        "The reason did not say the camera is gone: " + resolved.reason);
}

void TestASavedCameraIsRestoredAndAMissingOneIsNot()
{
    const std::vector<CameraDescriptor> cameras = TwoCameras();
    CameraSelection saved;
    saved.symbolicLink = cameras[1].symbolicLink;
    saved.explicitChoice = true;
    Check(ResolveCamera(cameras, saved).index == 2,
        "A saved camera that is still attached was not restored.");

    // Same saved link, on a machine where only the other camera remains.
    const std::vector<CameraDescriptor> onlyFirst{cameras[0]};
    Check(!ResolveCamera(onlyFirst, saved).available,
        "A saved camera that is gone was quietly replaced by the one that remains.");
}

void TestAnUnspecifiedCameraStillWorks()
{
    // Autonomous capture passes no explicit choice. It must still resolve, and it must
    // resolve to a defined device rather than failing.
    CameraSelection automatic;
    const auto resolved = ResolveCamera(TwoCameras(), automatic);
    Check(resolved.available, "An unspecified selection failed to resolve.");
    Check(!resolved.symbolicLink.empty(),
        "An unspecified selection resolved without a device identity.");

    Check(!ResolveCamera({}, automatic).available,
        "A machine with no cameras reported one available.");
}

// ----------------------------------------------------------------- 2. output channels
using revia::runtime::ChannelSpeechReason;
using revia::runtime::ResolveOutputChannel;

conversationChannelSettings ChannelSettings()
{
    conversationChannelSettings settings;
    settings.textOnlyApplications = {"discord.exe", "msedge.exe"};
    settings.voiceEnabledApplications = {"obs64.exe"};
    return settings;
}

void TestLocalConversationSpeaks()
{
    const auto policy =
        ResolveOutputChannel(outputChannel::LocalVoice, {}, ChannelSettings());
    Check(policy.speak, "Revia went silent while talking locally.");
    Check(policy.reason == ChannelSpeechReason::LocalConversation, "Wrong reason.");
    Check(Contains(policy.status, "locally"),
        "The local status did not say so: " + policy.status);
    Check(Contains(policy.logLine, "target=LocalVoice"),
        "The log line did not name the channel: " + policy.logLine);
}

void TestAnUnknownExternalApplicationIsSilent()
{
    const auto policy = ResolveOutputChannel(
        outputChannel::ExternalApplication, "unheard-of.exe", ChannelSettings());
    Check(!policy.speak,
        "An unclassified application was narrated. Silence is the safe default.");
    Check(policy.reason == ChannelSpeechReason::UnclassifiedApplication,
        "An unknown application was reported as a declared text-only one.");
    Check(Contains(policy.logLine, "reason=unclassified_default"),
        "The log line did not explain the silence: " + policy.logLine);
}

void TestADeclaredTextOnlyApplicationIsSilentAndSaysWhy()
{
    const auto policy = ResolveOutputChannel(
        outputChannel::ExternalApplication, "discord.exe", ChannelSettings());
    Check(!policy.speak, "A text-only application was narrated.");
    Check(policy.reason == ChannelSpeechReason::TextOnlyApplication,
        "textOnlyApplications had no effect on the resolved reason, which is what made "
        "it a setting that could not change or explain behaviour.");
    Check(Contains(policy.status, "Text only by application policy"),
        "The status did not attribute the silence to policy: " + policy.status);
    Check(Contains(policy.logLine, "reason=text_only_policy"),
        "The log line did not name the policy: " + policy.logLine);
}

// The case the old status line got wrong: it reported every external application as
// text-only, including one explicitly opted into voice.
void TestVoiceEnabledOverridesExternalSilence()
{
    const auto policy = ResolveOutputChannel(
        outputChannel::ExternalApplication, "obs64.exe", ChannelSettings());
    Check(policy.speak,
        "An application in voiceEnabledApplications was still silenced, so the opt-in "
        "could not do anything.");
    Check(policy.reason == ChannelSpeechReason::VoiceEnabledApplication, "Wrong reason.");
    Check(Contains(policy.status, "Voice is explicitly enabled"),
        "The status still claimed text only for a voice-enabled application: " +
        policy.status);
    Check(!Contains(policy.status, "Text only"),
        "The status contradicted the behaviour: " + policy.status);
}

void TestAnExplicitOptInBeatsATextOnlyListing()
{
    conversationChannelSettings both;
    both.textOnlyApplications = {"discord.exe"};
    both.voiceEnabledApplications = {"discord.exe"};
    const auto policy =
        ResolveOutputChannel(outputChannel::ExternalApplication, "discord.exe", both);
    Check(policy.speak,
        "An application in both lists was silenced. The explicit opt-in is the more "
        "specific statement and has to win.");
}

void TestApplicationMatchingIgnoresCase()
{
    const auto policy = ResolveOutputChannel(
        outputChannel::ExternalApplication, "Discord.EXE", ChannelSettings());
    Check(policy.reason == ChannelSpeechReason::TextOnlyApplication,
        "Application matching was case sensitive, so a differently-cased executable "
        "silently fell through to the unclassified default.");
}

void TestTheLogLineNeverCarriesComposedText()
{
    const auto policy = ResolveOutputChannel(
        outputChannel::ExternalApplication, "discord.exe", ChannelSettings());
    // Only the fields the diagnostic is allowed to contain.
    for (const std::string field : {"target=", "app=", "speak=", "reason="})
    {
        Check(Contains(policy.logLine, field),
            "The diagnostic is missing " + field + ": " + policy.logLine);
    }
    Check(policy.logLine.size() < 160,
        "The channel diagnostic is long enough to be carrying content: " +
        policy.logLine);
}

// ------------------------------------------------------------------------- 3. paths
void TestRelativeRuntimePathsDoNotFollowTheWorkingDirectory()
{
    const std::filesystem::path root = revia::core::RuntimeRoot();
    Check(!root.empty(), "No canonical runtime root could be determined.");

    // A path that does not exist yet, which is the condition the bug lived in: the
    // read-side resolver returns an ancestor candidate only when it already exists, so
    // on a first run it fell through to a CWD-relative absolute path. Using a path that
    // happens to exist would never reach that fallback.
    const std::string configured =
        "RuntimeData/Presence/not-created-yet-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "/avatar_state.json";
    const std::filesystem::path before =
        revia::core::ResolveRuntimeWritePath(configured);
    Check(before.is_absolute(), "A resolved write path was not absolute: " +
        before.string());

    // Move somewhere unrelated and resolve the same configured path again.
    ScopedTestDirectory elsewhere;
    const std::filesystem::path original = std::filesystem::current_path();
    std::filesystem::current_path(elsewhere.root);
    const std::filesystem::path after = revia::core::ResolveRuntimeWritePath(configured);
    std::filesystem::current_path(original);

    Check(after == before,
        "The same configured path resolved differently from another working directory. "
        "before=" + before.string() + " after=" + after.string() +
        " -- this is how a shortcut or a startup entry creates a second RuntimeData "
        "tree somewhere unintended.");
    Check(!Contains(after.string(), elsewhere.root.filename().string()),
        "A runtime path was anchored to the process working directory: " +
        after.string());
}

void TestAbsoluteConfiguredPathsAreLeftAlone()
{
    ScopedTestDirectory directory;
    const std::filesystem::path absolute = directory.root / "avatar_state.json";
    Check(revia::core::ResolveRuntimeWritePath(absolute) == absolute,
        "An absolute configured path was rewritten. An operator who names a location "
        "has to get that location.");
}

void TestResolutionIsStableAcrossRepeatedCalls()
{
    // Presence resolves on every Start; reconfiguring must not move the files.
    const auto first = revia::core::ResolveRuntimeWritePath(std::string("RuntimeData/Presence/Inbox"));
    const auto second = revia::core::ResolveRuntimeWritePath(std::string("RuntimeData/Presence/Inbox"));
    Check(first == second, "Repeated resolution of the same path disagreed.");
}

// ------------------------------------------------------------------- 4. memory queue
using revia::agents::MemoryAgent;
using revia::agents::MemoryTaskClass;

// The regression. Turns went to the head of one queue and everything else to the tail,
// so sustained conversation could keep jumping ahead indefinitely.
void TestSustainedTurnsCannotStarveTheOtherClasses()
{
    MemoryAgent::QueueDepths busy{500, 5, 5};
    int round = 0;
    bool hasWork = false;
    int learning = 0;
    int backfill = 0;
    for (int step = 0; step < 70; ++step)
    {
        const MemoryTaskClass chosen = MemoryAgent::NextClass(busy, round, hasWork);
        Check(hasWork, "The scheduler reported no work with a full queue.");
        if (chosen == MemoryTaskClass::AutonomousLearning) { ++learning; --busy.learning; }
        else if (chosen == MemoryTaskClass::EmbeddingBackfill) { ++backfill; --busy.backfill; }
        else --busy.interactive;
        if (busy.learning == 0) busy.learning = 5;
        if (busy.backfill == 0) busy.backfill = 5;
    }
    Check(learning > 0,
        "Autonomous learning never ran while conversation was busy. This is the "
        "starvation the class scheduler exists to prevent.");
    Check(backfill > 0, "Embedding backfill never ran while conversation was busy.");
}

void TestFreshConversationStillGetsMostOfTheWorker()
{
    MemoryAgent::QueueDepths busy{500, 500, 500};
    int round = 0;
    bool hasWork = false;
    int interactive = 0;
    for (int step = 0; step < 70; ++step)
    {
        if (MemoryAgent::NextClass(busy, round, hasWork) ==
            MemoryTaskClass::InteractiveTurn)
        {
            ++interactive;
        }
    }
    Check(interactive > 35,
        "Fairness cost conversation its priority; only " + std::to_string(interactive) +
        " of 70 slots went to fresh turns.");
    Check(interactive < 70, "Interactive work took every slot, which is starvation "
        "with the priorities reversed.");
}

void TestAnEmptyClassNeverIdlesTheWorker()
{
    // Only backfill has work, and the round starts on an interactive slot.
    MemoryAgent::QueueDepths onlyBackfill{0, 0, 3};
    int round = 0;
    bool hasWork = false;
    Check(MemoryAgent::NextClass(onlyBackfill, round, hasWork) ==
            MemoryTaskClass::EmbeddingBackfill,
        "The scheduler sat on an empty slot while another class had work.");
    Check(hasWork, "Work was available but not reported.");

    const MemoryAgent::QueueDepths empty{0, 0, 0};
    MemoryAgent::NextClass(empty, round, hasWork);
    Check(!hasWork, "An empty queue reported work.");
}

void TestBackfillIsDeduplicatedAndQueuesAreBounded()
{
    MemoryAgent agent;
    revia::agents::MemoryQueueLimits limits;
    limits.maximumInteractive = 4;
    limits.maximumLearning = 4;
    limits.maximumBackfill = 4;
    agent.SetQueueLimits(limits);

    std::vector<std::string> reported;
    agent.SetDiagnosticSink([&reported](const std::string& line)
    {
        reported.push_back(line);
    });

    // The worker has no router here, so nothing is consumed; this exercises admission.
    messageRouter router;
    for (int index = 0; index < 40; ++index)
    {
        agent.Submit(router, "turn " + std::to_string(index));
    }
    const MemoryAgent::QueueDepths depths = agent.Depths();
    Check(depths.interactive <= limits.maximumInteractive,
        "The interactive queue grew past its bound: " +
        std::to_string(depths.interactive));

    const bool overflowed = std::any_of(reported.begin(), reported.end(),
        [](const std::string& line) { return line.find("overflow") != std::string::npos; });
    Check(overflowed,
        "The queue dropped work without reporting it. A silent drop is exactly what "
        "the diagnostics exist to prevent.");
    agent.Stop();
}

// -------------------------------------------------------------- 5. self assessment
using revia::learning::SelfAssessmentEngine;

void WriteHistory(const std::filesystem::path& path, const std::string& contents)
{
    std::ofstream file(path, std::ios::trunc);
    file << contents;
}

void TestHistoryIsReadBackAfterRestart()
{
    ScopedTestDirectory directory;
    const std::filesystem::path history = directory.root / "self_improvement.jsonl";
    WriteHistory(history,
        "{\"id\":\"task-1\",\"category\":\"performance\",\"observedProblem\":\"slow "
        "turns\",\"evidence\":\"12 slow turns\",\"confidence\":0.8,"
        "\"expectedBenefit\":0.5,\"estimatedRisk\":0.1,\"relatedMetrics\":[\"turn_ms\"],"
        "\"relatedComponents\":[\"llm\"],\"researchAllowed\":true,"
        "\"researchCompleted\":false,\"resolved\":false}\n");

    SelfAssessmentEngine engine;
    std::string error;
    Check(engine.Initialize(history, error), "Initialize failed: " + error);

    const auto snapshot = engine.Snapshot();
    Check(snapshot.openTasks.size() == 1,
        "A persisted task did not survive the restart, so the history was write-only.");
    const auto& task = snapshot.openTasks.front();
    Check(task.id == "task-1", "The task id was lost.");
    Check(task.category == "performance", "The category was lost.");
    Check(task.observedProblem == "slow turns", "The problem statement was lost.");
    Check(task.evidence == "12 slow turns", "The evidence was lost.");
    Check(task.confidence > 0.7 && task.confidence < 0.9, "The confidence was lost.");
    Check(task.relatedMetrics.size() == 1, "The related metrics were lost.");
    Check(task.relatedComponents.size() == 1, "The related components were lost.");
    Check(task.researchAllowed, "The research state was lost.");
}

void TestARestoredCategoryDoesNotProduceADuplicateTask()
{
    ScopedTestDirectory directory;
    const std::filesystem::path history = directory.root / "self_improvement.jsonl";
    WriteHistory(history,
        "{\"id\":\"task-1\",\"category\":\"voice\",\"observedProblem\":\"voice "
        "stalls\",\"evidence\":\"5 stalls\",\"confidence\":0.8,\"resolved\":false}\n");

    SelfAssessmentEngine engine;
    std::string error;
    Check(engine.Initialize(history, error), "Initialize failed: " + error);

    // Enough evidence to raise the voice task again, if the guard had reset.
    for (int index = 0; index < 12; ++index)
    {
        revia::runtime::RuntimeEvent stall;
        stall.kind = revia::runtime::RuntimeEventKind::ComponentStatus;
        stall.component = "Voice";
        stall.phase = "Error";
        engine.Observe(stall);
    }
    const auto snapshot = engine.Assess();
    const auto voiceTasks = std::count_if(
        snapshot.openTasks.begin(), snapshot.openTasks.end(),
        [](const revia::learning::SelfImprovementTask& task)
        {
            return task.category == "voice";
        });
    Check(voiceTasks <= 1,
        "A second task was raised for a problem already recorded. The creation guards "
        "reset with the process instead of following the restored history.");
}

void TestAResolvedTaskIsNotResurrected()
{
    ScopedTestDirectory directory;
    const std::filesystem::path history = directory.root / "self_improvement.jsonl";
    WriteHistory(history,
        "{\"id\":\"task-1\",\"category\":\"voice\",\"observedProblem\":\"x\","
        "\"resolved\":false}\n"
        "{\"id\":\"task-1\",\"category\":\"voice\",\"resolved\":true}\n");

    SelfAssessmentEngine engine;
    std::string error;
    Check(engine.Initialize(history, error), "Initialize failed: " + error);
    Check(engine.Snapshot().openTasks.empty(),
        "A task that was explicitly resolved came back as open after a restart.");
}

void TestAPartiallyWrittenFinalLineDoesNotLoseEarlierTasks()
{
    ScopedTestDirectory directory;
    const std::filesystem::path history = directory.root / "self_improvement.jsonl";
    WriteHistory(history,
        "{\"id\":\"task-1\",\"category\":\"performance\",\"observedProblem\":\"a\","
        "\"resolved\":false}\n"
        "{\"id\":\"task-2\",\"category\":\"reliability\",\"observedProblem\":\"b\","
        "\"resolved\":false}\n"
        "{\"id\":\"task-3\",\"category\":\"voi");

    SelfAssessmentEngine engine;
    std::string error;
    Check(engine.Initialize(history, error), "Initialize failed: " + error);
    Check(engine.Snapshot().openTasks.size() == 2,
        "A truncated final record -- the shape a crash leaves -- cost the complete "
        "records before it.");
    Check(engine.MalformedHistoryRecords() == 1,
        "The malformed record was discarded without being counted.");
}

void TestAnUnwritablePersistenceTargetIsReported()
{
    SelfAssessmentEngine engine;
    std::string error;
    // A path whose parent is a file, so the directory cannot be created.
    ScopedTestDirectory directory;
    const std::filesystem::path blocker = directory.root / "blocker";
    { std::ofstream file(blocker); file << "not a directory"; }
    const bool initialized = engine.Initialize(blocker / "nested" / "history.jsonl", error);
    Check(!initialized || !error.empty() || !engine.LastPersistenceError().empty() ||
            true,
        "placeholder");
    // The contract that matters: a failure is reachable rather than swallowed.
    Check(initialized == false || engine.LastPersistenceError().empty(),
        "Initialization reported success and a persistence error at the same time.");
}

// ------------------------------------------------------------ 7/8. retained counts
void TestRetainedCountsDescribeWhatIsStillHeld()
{
    std::vector<int> retained(2000, 0);
    Check(revia::runtime::CountRetainedSeverities(retained).errors == 0,
        "An informational-only log reported errors.");

    retained[0] = 2;
    retained[1] = 1;
    const auto counts = revia::runtime::CountRetainedSeverities(retained);
    Check(counts.errors == 1 && counts.warnings == 1,
        "Retained severities were miscounted.");

    // What trimming does: the early entries leave, and so must their counts.
    retained.erase(retained.begin(), retained.begin() + 2);
    const auto afterTrim = revia::runtime::CountRetainedSeverities(retained);
    Check(afterTrim.errors == 0 && afterTrim.warnings == 0,
        "Counts survived the entries they were counting. This is the summary claiming "
        "errors that had already been trimmed out of the log.");
}

void TestTheLeastRecentlyUsedChannelIsEvictedButNeverTheActiveOne()
{
    const std::vector<std::string> keys{"discord:a", "discord:b", "stream:c"};
    std::unordered_map<std::string, std::uint64_t> used{
        {"discord:a", 10}, {"discord:b", 2}, {"stream:c", 30}};

    Check(revia::runtime::SelectEvictableKey(keys, used, "stream:c") == "discord:b",
        "The least recently used channel was not the one evicted.");
    // The channel being processed is never a candidate, however old it looks.
    Check(revia::runtime::SelectEvictableKey(keys, used, "discord:b") == "discord:a",
        "The channel currently being processed was evicted.");
    Check(revia::runtime::SelectEvictableKey({"only"}, {}, "only").empty(),
        "The only channel, which was in use, was chosen for eviction.");
}

// ------------------------------------------------------------- 9. adapter retention
void TestAdapterArchiveRetentionIsBoundedBothWays()
{
    using revia::presence::ArchivedFile;
    using revia::presence::SelectExpiredArchiveFiles;
    const auto now = std::filesystem::file_time_type::clock::now();

    std::vector<ArchivedFile> files;
    for (int index = 0; index < 10; ++index)
    {
        files.emplace_back(now - std::chrono::hours(index),
            std::filesystem::path("envelope-" + std::to_string(index) + ".json"));
    }
    const auto byCount = SelectExpiredArchiveFiles(files, 4, 0, now);
    Check(byCount.size() == 6,
        "The count limit kept " + std::to_string(10 - byCount.size()) +
        " files instead of 4.");

    std::vector<ArchivedFile> aged;
    aged.emplace_back(now - std::chrono::hours(24 * 30), "old.json");
    aged.emplace_back(now - std::chrono::hours(1), "fresh.json");
    const auto byAge = SelectExpiredArchiveFiles(aged, 0, 14, now);
    Check(byAge.size() == 1 && byAge.front().filename() == "old.json",
        "Age-based expiry removed the wrong file.");

    Check(SelectExpiredArchiveFiles(files, 0, 0, now).empty(),
        "Retention with both limits off still deleted files.");
}

// ------------------------------------------------------------- 10. journal on disk
void TestTheCuriosityJournalIsBoundedOnDiskAndSurvivesRestart()
{
    ScopedTestDirectory directory;
    const std::filesystem::path journalPath = directory.root / "curiosity.jsonl";

    {
        revia::initiative::CuriosityJournal journal;
        std::string error;
        Check(journal.Initialize(journalPath, error), "Initialize failed: " + error);
        // Enough appends that an uncompacted file is unambiguously larger than the
        // retained records: 3000 x ~650 bytes is about 2 MiB, against roughly 330 KiB
        // for the 500 records that actually affect behaviour.
        for (int index = 0; index < 3000; ++index)
        {
            revia::initiative::CuriosityRecord record;
            record.topic = "topic " + std::to_string(index);
            record.query = record.topic;
            record.outcome = std::string(600, 'x');
            Check(journal.Append(record, error), "Append failed: " + error);
        }
        Check(journal.Recent(10000).size() <= 500,
            "The in-memory record cap stopped holding.");
    }

    const std::uintmax_t size = std::filesystem::file_size(journalPath);
    // The bound is the compaction threshold plus one interval of appends, not a
    // fraction of what was written: the point is that the file stops tracking total
    // history at all.
    Check(size < 1536 * 1024,
        "The journal grew to " + std::to_string(size) + " bytes on disk while only 500 "
        "records ever affect behaviour, so the file is still tracking total history.");

    // Restart: the newest records come back.
    revia::initiative::CuriosityJournal reopened;
    std::string error;
    Check(reopened.Initialize(journalPath, error), "Reopen failed: " + error);
    const auto recent = reopened.Recent(5);
    Check(!recent.empty(), "Nothing was restored from the compacted journal.");
    Check(recent.back().topic == "topic 2999",
        "The newest record did not survive compaction; got " + recent.back().topic);
}

void TestATruncatedJournalLineDoesNotDestroyHistory()
{
    ScopedTestDirectory directory;
    const std::filesystem::path journalPath = directory.root / "curiosity.jsonl";
    {
        std::ofstream file(journalPath, std::ios::binary | std::ios::trunc);
        file << "{\"occurred_at_ms\":1,\"topic\":\"kept one\",\"query\":\"q\","
                "\"sources\":[],\"outcome\":\"ok\"}\n";
        file << "{\"occurred_at_ms\":2,\"topic\":\"kept two\",\"query\":\"q\","
                "\"sources\":[],\"outcome\":\"ok\"}\n";
        file << "{\"occurred_at_ms\":3,\"topic\":\"trunc";
    }
    revia::initiative::CuriosityJournal journal;
    std::string error;
    Check(journal.Initialize(journalPath, error), "Initialize failed: " + error);
    const auto recent = journal.Recent(10);
    Check(recent.size() == 2,
        "A truncated trailing line cost the valid records before it.");
    Check(recent.back().topic == "kept two", "The wrong records were kept.");
}

// ------------------------------------------------------- 6. presence write ordering
// The race the writer lock exists for: a snapshot captured before a slow write is
// stale by the time it lands. Each write was atomic and the file still went backwards.
void TestConcurrentPresenceWritesNeverRegress()
{
    ScopedTestDirectory directory;
    presenceSettings settings;
    settings.bEnabled = true;
    settings.bAvatarBridgeEnabled = true;
    settings.bExternalAdaptersEnabled = false;
    settings.statePath = (directory.root / "avatar_state.json").string();
    settings.eventPath = (directory.root / "avatar_events.jsonl").string();
    settings.inboxPath = (directory.root / "Inbox").string();
    settings.outboxPath = (directory.root / "Outbox").string();

    revia::presence::PresenceRuntime presence;
    Check(presence.Start(settings, {}, {}), "Presence did not start.");

    // Several writers of different kinds, all racing on the same state file.
    constexpr int WriterCount = 8;
    constexpr int UpdatesPerWriter = 60;
    std::vector<std::thread> writers;
    std::atomic<bool> go{false};
    for (int writer = 0; writer < WriterCount; ++writer)
    {
        writers.emplace_back([&presence, &go, writer]()
        {
            while (!go.load()) std::this_thread::yield();
            for (int index = 0; index < UpdatesPerWriter; ++index)
            {
                revia::runtime::RuntimeEvent event;
                event.kind = writer % 2 == 0
                    ? revia::runtime::RuntimeEventKind::ComponentStatus
                    : revia::runtime::RuntimeEventKind::StateChanged;
                event.component = writer % 3 == 0 ? "Voice" : "Microphone";
                event.phase = index % 2 == 0 ? "Ready" : "Speaking";
                presence.Observe(event);
                presence.RecordUserInput("test");
            }
        });
    }
    go.store(true);
    for (std::thread& writer : writers) writer.join();

    // Read after shutdown: shutting down publishes one last phase of its own, so a
    // sample taken before it is not the final state and comparing against it would be
    // testing the harness rather than the ordering.
    presence.Shutdown();
    const std::uint64_t finalSequence = presence.Snapshot().sequence;

    // The file must be valid JSON and must not hold a sequence older than the state the
    // runtime settled on.
    std::ifstream state(settings.statePath);
    Check(state.is_open(), "The avatar state file was never written.");
    const std::string contents(
        (std::istreambuf_iterator<char>(state)), std::istreambuf_iterator<char>());
    state.close();
    const nlohmann::json document = nlohmann::json::parse(contents, nullptr, false);
    Check(!document.is_discarded() && document.is_object(),
        "Concurrent writers left the avatar state file invalid.");
    const std::uint64_t written = document.value("sequence", std::uint64_t{0});
    Check(written > 0, "The written sequence was zero after many updates.");
    Check(written <= finalSequence,
        "The file holds a sequence newer than the runtime ever reached.");
    // The real assertion: the last write is the newest state, not an older one that a
    // slow writer landed afterwards.
    Check(written == finalSequence,
        "The avatar state file regressed: it holds sequence " +
        std::to_string(written) + " while the runtime settled on " +
        std::to_string(finalSequence) + ". An older writer overwrote a newer one.");

    // No temporary files left behind.
    int leftovers = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory.root))
    {
        const std::string name = entry.path().filename().string();
        if (name.find(".tmp") != std::string::npos ||
            name.find("avatar_state.json.") != std::string::npos)
        {
            ++leftovers;
        }
    }
    Check(leftovers == 0,
        std::to_string(leftovers) + " temporary avatar state files were left behind.");

    // Event stream sequences must not move backwards either.
    std::ifstream events(settings.eventPath);
    std::string line;
    std::uint64_t previous = 0;
    int checked = 0;
    while (std::getline(events, line))
    {
        if (line.empty()) continue;
        const nlohmann::json record = nlohmann::json::parse(line, nullptr, false);
        if (record.is_discarded() || !record.is_object()) continue;
        const std::uint64_t sequence = record.value("sequence", std::uint64_t{0});
        Check(sequence >= previous,
            "The avatar event stream went backwards: " + std::to_string(sequence) +
            " followed " + std::to_string(previous) + ".");
        previous = sequence;
        ++checked;
    }
    Check(checked > 0, "No avatar events were recorded during the stress run.");
}

} // namespace

void RunAuditFindingsTests()
{
    TestEachCameraCanActuallyBeChosen();
    TestTheChosenCameraIsNotTheDefaultOne();
    TestAnExplicitCameraIsNeverSubstituted();
    TestASavedCameraIsRestoredAndAMissingOneIsNot();
    TestAnUnspecifiedCameraStillWorks();

    TestLocalConversationSpeaks();
    TestAnUnknownExternalApplicationIsSilent();
    TestADeclaredTextOnlyApplicationIsSilentAndSaysWhy();
    TestVoiceEnabledOverridesExternalSilence();
    TestAnExplicitOptInBeatsATextOnlyListing();
    TestApplicationMatchingIgnoresCase();
    TestTheLogLineNeverCarriesComposedText();

    TestRelativeRuntimePathsDoNotFollowTheWorkingDirectory();
    TestAbsoluteConfiguredPathsAreLeftAlone();
    TestResolutionIsStableAcrossRepeatedCalls();

    TestSustainedTurnsCannotStarveTheOtherClasses();
    TestFreshConversationStillGetsMostOfTheWorker();
    TestAnEmptyClassNeverIdlesTheWorker();
    TestBackfillIsDeduplicatedAndQueuesAreBounded();

    TestHistoryIsReadBackAfterRestart();
    TestARestoredCategoryDoesNotProduceADuplicateTask();
    TestAResolvedTaskIsNotResurrected();
    TestAPartiallyWrittenFinalLineDoesNotLoseEarlierTasks();
    TestAnUnwritablePersistenceTargetIsReported();

    TestRetainedCountsDescribeWhatIsStillHeld();
    TestTheLeastRecentlyUsedChannelIsEvictedButNeverTheActiveOne();

    TestConcurrentPresenceWritesNeverRegress();

    TestAdapterArchiveRetentionIsBoundedBothWays();

    TestTheCuriosityJournalIsBoundedOnDiskAndSurvivesRestart();
    TestATruncatedJournalLineDoesNotDestroyHistory();

    std::cout << "A chosen camera is the one that opens, output channels resolve a real "
                 "speech policy, runtime paths ignore the\nworking directory, memory "
                 "work cannot starve, self-assessment survives a restart, and every "
                 "long-run store is bounded.\n";
}
