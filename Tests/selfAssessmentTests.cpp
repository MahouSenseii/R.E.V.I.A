#include "testSupport.h"

#include "Learning/selfAssessment.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Self-assessment, exercised the way the runtime uses it rather than the way a history
// file can be hand-written. Malformed rows below are injected between records from
// the real writer to exercise recovery from schema-invalid history.
//
// The suite that came before this one wrote its own history lines, and it wrote them
// with the category vocabulary the loader happened to look for. That is why it passed
// while a restart really did forget an open task: no test ever let Assess() name a
// category and then asked a fresh engine to read that name back. Every case here
// round-trips through the engine's own writer.
namespace
{
using revia::learning::SelfAssessmentEngine;
using revia::learning::SelfAssessmentSnapshot;
using revia::learning::SelfImprovementTask;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;

revia::runtime::RuntimeEvent Component(
    std::string component,
    std::string phase,
    const double elapsedMilliseconds = 0.0)
{
    revia::runtime::RuntimeEvent event;
    event.kind = revia::runtime::RuntimeEventKind::ComponentStatus;
    event.component = std::move(component);
    event.phase = std::move(phase);
    event.elapsedMilliseconds = elapsedMilliseconds;
    return event;
}

// Enough evidence to cross all three thresholds at once, so one helper serves every
// case and no case depends on which threshold it happened to pick.
void FeedEveryThreshold(SelfAssessmentEngine& engine)
{
    for (int index = 0; index < 12; ++index)
    {
        engine.Observe(Component("Conversation", "Ready", 9000.0));
    }
    for (int index = 0; index < 6; ++index)
    {
        engine.Observe(Component("Voice", "Error"));
    }
    for (int index = 0; index < 6; ++index)
    {
        engine.Observe(Component("Internet", "Unavailable"));
    }
}

std::size_t CountByCategory(
    const SelfAssessmentSnapshot& snapshot,
    const std::string& category)
{
    return static_cast<std::size_t>(std::count_if(
        snapshot.openTasks.begin(), snapshot.openTasks.end(),
        [&category](const SelfImprovementTask& task)
        {
            return task.category == category;
        }));
}

std::size_t HistoryLines(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::size_t lines = 0;
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty()) ++lines;
    }
    return lines;
}

// ISSUE 2. The categories a task is created with have to be the categories a restart
// recognises. Nothing here names a category itself: the engine writes them, the engine
// reads them, and a mismatch between the two shows up as a duplicate.
void TestEveryCreatedCategorySurvivesARestartWithoutDuplicating()
{
    ScopedTestDirectory directory;
    const std::filesystem::path history = directory.root / "self_improvement.jsonl";

    std::vector<std::string> categories;
    {
        SelfAssessmentEngine engine;
        std::string error;
        Check(engine.Initialize(history, error), "Initialize failed: " + error);
        FeedEveryThreshold(engine);
        const SelfAssessmentSnapshot created = engine.Assess();
        Check(created.openTasks.size() == 3,
            "Expected one task per threshold, got " +
                std::to_string(created.openTasks.size()) + ".");
        for (const SelfImprovementTask& task : created.openTasks)
        {
            categories.push_back(task.category);
        }
    }

    // A second process, reading the history the first one wrote.
    SelfAssessmentEngine restarted;
    std::string error;
    Check(restarted.Initialize(history, error), "Initialize failed: " + error);
    Check(restarted.Snapshot().openTasks.size() == 3,
        "A restart did not restore all three open tasks.");

    // The same evidence again. Each guard must already be standing.
    FeedEveryThreshold(restarted);
    const SelfAssessmentSnapshot afterRestart = restarted.Assess();
    for (const std::string& category : categories)
    {
        Check(CountByCategory(afterRestart, category) == 1,
            "A restart created a second task for the already-open category \"" +
                category + "\". The guard restored on load does not recognise the "
                "category the writer used.");
    }
    Check(afterRestart.openTasks.size() == 3,
        "A restart raised " + std::to_string(afterRestart.openTasks.size()) +
            " tasks where three were already open.");
    Check(HistoryLines(history) == 3,
        "A duplicate task reached the history file.");
}

// A resolved task stays resolved across the same round trip, with the categories the
// engine itself writes rather than hand-picked ones.
void TestAResolvedTaskStaysResolvedAcrossARestart()
{
    ScopedTestDirectory directory;
    const std::filesystem::path history = directory.root / "self_improvement.jsonl";

    std::string resolvedCategory;
    {
        SelfAssessmentEngine engine;
        std::string error;
        Check(engine.Initialize(history, error), "Initialize failed: " + error);
        FeedEveryThreshold(engine);
        const SelfAssessmentSnapshot created = engine.Assess();
        Check(!created.openTasks.empty(), "No task was created to resolve.");
        resolvedCategory = created.openTasks.front().category;
        Check(engine.ResolveTask(created.openTasks.front().id, error),
            "ResolveTask failed: " + error);
    }

    SelfAssessmentEngine restarted;
    std::string error;
    Check(restarted.Initialize(history, error), "Initialize failed: " + error);
    Check(CountByCategory(restarted.Snapshot(), resolvedCategory) == 0,
        "A resolved task came back as open after a restart.");

    // Resolved is not the same as never raised: the threshold is still crossed, so the
    // problem may be recorded again. What must not happen is two of them.
    FeedEveryThreshold(restarted);
    const SelfAssessmentSnapshot afterRestart = restarted.Assess();
    Check(CountByCategory(afterRestart, resolvedCategory) <= 1,
        "A resolved category produced more than one open task after a restart.");
}

// ISSUE 3. Resolution mutates durable state, so it must not mutate before the write
// that makes it durable has succeeded.
void TestAFailedResolutionLeavesTheTaskOpen()
{
    ScopedTestDirectory directory;
    const std::filesystem::path history = directory.root / "self_improvement.jsonl";

    SelfAssessmentEngine engine;
    std::string error;
    Check(engine.Initialize(history, error), "Initialize failed: " + error);
    FeedEveryThreshold(engine);
    const SelfAssessmentSnapshot created = engine.Assess();
    Check(!created.openTasks.empty(), "No task was created to resolve.");
    const SelfImprovementTask target = created.openTasks.front();
    const std::size_t openBefore = created.openTasks.size();
    const std::size_t linesBefore = HistoryLines(history);

    // A history that cannot be appended to. The task is on disk and open; the
    // resolution cannot be written.
    std::error_code permissionError;
    std::filesystem::permissions(
        history,
        std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
            std::filesystem::perms::others_write,
        std::filesystem::perm_options::remove,
        permissionError);
    Check(!permissionError, "Could not make the history read-only for the test.");

    std::string resolveError;
    const bool resolved = engine.ResolveTask(target.id, resolveError);
    Check(!resolved, "ResolveTask reported success although the write failed.");
    Check(!resolveError.empty(), "A failed resolution gave no reason.");

    const SelfAssessmentSnapshot after = engine.Snapshot();
    Check(after.openTasks.size() == openBefore,
        "A resolution that could not be written removed the task from this process "
        "anyway. It is still open on disk, so it reappears at the next start.");
    Check(CountByCategory(after, target.category) == 1,
        "The task that failed to resolve is no longer listed under its category.");
    Check(!engine.LastPersistenceError().empty(),
        "A failed resolution left no persistence error for a caller to read.");

    std::error_code restore;
    std::filesystem::permissions(
        history, std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add, restore);
    Check(HistoryLines(history) == linesBefore,
        "The history changed although the resolution failed.");
}

// The same rule for creation: a task the panel lists must be a task the next start
// finds. If it could not be written, it is not durable and must not be presented as if
// it were -- and its guard must not be left standing, because that would suppress the
// problem forever on the strength of a record that does not exist.
void TestATaskThatCouldNotBePersistedIsNotPresentedAsRecorded()
{
    ScopedTestDirectory directory;
    // Make the initialized history path a directory so subsequent appends fail.
    const std::filesystem::path history = directory.root / "self_improvement.jsonl";

    SelfAssessmentEngine engine;
    std::string error;
    Check(engine.Initialize(history, error), "Initialize failed: " + error);
    std::filesystem::create_directories(history);
    FeedEveryThreshold(engine);
    const SelfAssessmentSnapshot created = engine.Assess();

    Check(!engine.LastPersistenceError().empty(),
        "A task that could not be written reported no persistence error.");
    Check(created.openTasks.empty(),
        "A task that could not be written is still listed as open. The next start has "
        "never heard of it, so the panel and the disk disagree.");
    Check(created.conclusion.find("could not be saved") != std::string::npos,
        "The conclusion did not say that a task could not be saved.");

    // The guard must not have been left standing by the failed attempt: once the
    // history is writable again, the same evidence has to be able to raise the task.
    std::filesystem::remove_all(history);
    SelfAssessmentEngine recovered;
    Check(recovered.Initialize(history, error), "Initialize failed: " + error);
    FeedEveryThreshold(recovered);
    Check(!recovered.Assess().openTasks.empty(),
        "A failed write suppressed the problem permanently.");
}

void TestMalformedTypedRecordsDoNotInterruptRecovery()
{
    using nlohmann::json;
    ScopedTestDirectory directory;
    const auto history = directory.root / "self_improvement.jsonl";
    SelfAssessmentEngine writer;
    std::string error;
    Check(writer.Initialize(history, error), "Could not initialize typed-history fixture.");
    FeedEveryThreshold(writer);
    const auto original = writer.Assess();
    std::vector<json> records;
    {
        std::ifstream file(history);
        std::string line;
        while (std::getline(file, line)) records.push_back(json::parse(line));
    }
    Check(records.size() == 3, "Writer did not produce the expected history fixture.");
    std::vector<json> malformed = {json::array(), json{{"id", 42}}, json{{"id", nullptr}},
        json{{"id", ""}}};
    const char* stringFields[] = {"id", "category", "observedProblem", "evidence"};
    const char* boolFields[] = {"resolved", "researchAllowed", "researchCompleted"};
    const char* numberFields[] = {"confidence", "expectedBenefit", "estimatedRisk"};
    const char* arrayFields[] = {"relatedMetrics", "relatedComponents"};
    const auto addMalformed = [&](const char* field, const json& value)
    {
        json record = records.front();
        record[field] = value;
        malformed.push_back(std::move(record));
    };
    for (const char* field : stringFields)
        for (const json& value : {json(42), json(nullptr)}) addMalformed(field, value);
    for (const char* field : boolFields)
        for (const json& value : {json("false"), json(1), json(nullptr)}) addMalformed(field, value);
    for (const char* field : numberFields)
        for (const json& value : {json("0.5"), json::array(), json(nullptr)}) addMalformed(field, value);
    for (const char* field : arrayFields)
        for (const json& value : {json("metric"), json::array({"valid", 42}), json(nullptr)})
            addMalformed(field, value);
    for (const char* field : {"id", "category", "observedProblem"})
    {
        json record = records.front();
        record.erase(field);
        malformed.push_back(std::move(record));
    }
    // Invalid resolution data must not retire an otherwise valid task.
    json invalidResolution = records.front();
    invalidResolution["resolved"] = true;
    invalidResolution["category"] = 42;
    malformed.push_back(std::move(invalidResolution));
    {
        std::ofstream file(history, std::ios::trunc);
        file << records.front().dump() << '\n';
        for (const auto& record : malformed) file << record.dump() << '\n';
        file << records[1].dump() << '\n';
        // Missing optional fields use their defaults; required identity/problem remain.
        file << R"({"id":"minimal","category":"custom","observedProblem":"minimal task"})" << '\n';
        file << records[2].dump() << '\n';
        file << json{{"id", original.openTasks[2].id}, {"resolved", true}}.dump() << '\n';
        file << "{\"id\":\"truncated";
    }
    SelfAssessmentEngine recovered;
    bool initialized = false;
    try { initialized = recovered.Initialize(history, error); }
    catch (const std::exception& exception)
    {
        Check(false, "Schema-invalid history escaped Initialize: " + std::string(exception.what()));
    }
    Check(initialized, "Valid rows could not be recovered: " + error);
    const auto snapshot = recovered.Snapshot();
    Check(snapshot.openTasks.size() == 3, "Malformed records lost valid neighboring tasks.");
    Check(snapshot.openTasks[0].id == original.openTasks[0].id &&
        snapshot.openTasks[0].evidence == original.openTasks[0].evidence &&
        snapshot.openTasks[1].id == original.openTasks[1].id,
        "A malformed duplicate partially overwrote an earlier valid task.");
    const auto& minimal = snapshot.openTasks.back();
    Check(minimal.id == "minimal" && minimal.evidence.empty() && minimal.confidence == 0.0 &&
        minimal.relatedMetrics.empty() && minimal.relatedComponents.empty() &&
        !minimal.researchAllowed && !minimal.researchCompleted,
        "Missing optional fields did not use their defaults.");
    Check(recovered.MalformedHistoryRecords() == malformed.size() + 1,
        "Malformed typed history rows were not counted exactly once.");
}

void TestFailedReloadPreservesSnapshotAndPersistencePath()
{
    ScopedTestDirectory directory;
    const auto history = directory.root / "self_improvement.jsonl";
    SelfAssessmentEngine engine;
    std::string error;
    Check(engine.Initialize(history, error), "Could not initialize reload fixture.");
    FeedEveryThreshold(engine);
    const auto before = engine.Assess();
    const auto invalidHistory = directory.root / "directory-instead-of-history";
    std::filesystem::create_directory(invalidHistory);
    Check(!engine.Initialize(invalidHistory, error) && !error.empty(),
        "A directory was treated as a missing journal during reload.");
    const auto after = engine.Snapshot();
    Check(after.openTasks.size() == before.openTasks.size() &&
        after.openTasks.front().id == before.openTasks.front().id &&
        after.conversationTurns == before.conversationTurns,
        "A failed reload destroyed the current assessment snapshot.");
    Check(engine.ResolveTask(before.openTasks.front().id, error),
        "A failed reload replaced the working persistence path: " + error);
    Check(HistoryLines(history) == 4, "Resolution did not use the original journal after failed reload.");
}

} // namespace

void RunSelfAssessmentTests()
{
    TestMalformedTypedRecordsDoNotInterruptRecovery();
    TestFailedReloadPreservesSnapshotAndPersistencePath();
    TestEveryCreatedCategorySurvivesARestartWithoutDuplicating();
    TestAResolvedTaskStaysResolvedAcrossARestart();
    TestAFailedResolutionLeavesTheTaskOpen();
    TestATaskThatCouldNotBePersistedIsNotPresentedAsRecorded();
    std::cout << "Self-assessment categories round-trip through the engine's own "
                 "writer, and neither a resolution nor a new task mutates state a "
                 "failed write never made durable.\n";
}
