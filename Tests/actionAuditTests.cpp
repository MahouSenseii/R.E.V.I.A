#include "reviaSessionTestAccess.h"

#include "Goals/goalRunner.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <Windows.h>
#undef CreateDirectory
#endif

namespace
{

using namespace revia::actions;
using namespace revia::goals;
using revia::tests::Check;
using revia::runtime::ReviaSessionTestAccess;

std::vector<nlohmann::json> Records(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::vector<nlohmann::json> records;
    std::string line;
    while (std::getline(file, line)) records.push_back(nlohmann::json::parse(line));
    return records;
}

struct AuditFixture
{
    revia::tests::ScopedTestDirectory directory;
    std::filesystem::path audit = directory.root / "audit.jsonl";
    ActionRuntime runtime;

    AuditFixture()
    {
        const nlohmann::json settings = {
            {"mode", "supervised"},
            {"approvedRoots", {PathToUtf8(directory.root)}},
            {"autoApproveRiskThrough", "read_only"},
            {"createMissingApprovedRoots", false}};
        {
            std::ofstream file(directory.root / "capabilities.json");
            file << settings.dump();
            Check(file.good(), "Could not write audit fixture policy.");
        }
        std::string error;
        const bool initialized = runtime.Initialize(
            directory.root / "capabilities.json", audit, error);
        Check(initialized, "Audit fixture initialization failed: " + error);
    }

    ActionRequest Request() const
    {
        ActionRequest request;
        request.id = NewActionId();
        request.type = ActionType::CreateDirectory;
        request.source = directory.root / "created";
        return request;
    }

    ActionOutcome Execute(const bool scoped)
    {
        const revia::policy::CapabilityPolicy scope(runtime.Settings());
        return scoped ? runtime.ExecuteScoped(Request(), scope, true) : runtime.Execute(Request(), true);
    }
};

void BreakCompletionAudit(const std::filesystem::path& audit)
{
    const auto intent = Records(audit);
    Check(intent.size() == 1 && intent.front().at("record_type") == "intent",
        "Execution did not have its intent record before the completion callback.");
    std::filesystem::rename(audit, audit.string() + ".intent");
    std::filesystem::create_directory(audit);
}

void TestUnavailableAuditPreventsExecution(const bool scoped)
{
    AuditFixture fixture;
    std::filesystem::create_directory(fixture.audit);
    const auto result = fixture.Execute(scoped);
    Check(!result.result.attempted && !result.result.succeeded &&
        !std::filesystem::exists(fixture.Request().source),
        "ActionRuntime executed despite an unavailable required audit.");
    Check(!result.Succeeded() && !result.auditError.empty() &&
        result.Message().find("not executed") != std::string::npos,
        "ActionRuntime hid a failed audit admission.");
}

void TestIntentAndResultAreRecorded(const bool scoped)
{
    AuditFixture fixture;
    bool sawIntentBeforeCompletion = false;
    fixture.runtime.SetDispatchObserver([&](const ActionRequest&, const bool beginning)
    {
        if (beginning) return;
        const auto intent = Records(fixture.audit);
        sawIntentBeforeCompletion = intent.size() == 1 &&
            intent.front().at("record_type") == "intent" &&
            !intent.front().contains("attempted") && !intent.front().contains("succeeded");
    });
    const auto result = fixture.Execute(scoped);
    const auto records = Records(fixture.audit);
    Check(result.Succeeded() && result.auditError.empty() && sawIntentBeforeCompletion,
        "A normal audited action failed or lacked its pre-completion intent.");
    Check(records.size() == 2 && records[1].at("record_type") == "result" &&
        records[1].at("attempted") == true && records[1].at("succeeded") == true &&
        !records[0].at("audit_transaction").get<std::string>().empty() &&
        records[0].at("audit_transaction") == records[1].at("audit_transaction"),
        "Intent and actual executor result were not durably correlated.");
}

void TestCompletionFailurePreservesExecutionTruth(const bool scoped)
{
    AuditFixture fixture;
    fixture.runtime.SetDispatchObserver([&](const ActionRequest&, const bool beginning)
    {
        if (!beginning) BreakCompletionAudit(fixture.audit);
    });
    const auto result = fixture.Execute(scoped);
    Check(result.result.attempted && result.result.succeeded &&
        std::filesystem::is_directory(fixture.Request().source),
        "Completion audit failure changed the truth about the executed action.");
    Check(!result.Succeeded() && !result.auditError.empty() &&
        result.Message().find("completion audit") != std::string::npos &&
        result.Message().find("not executed") == std::string::npos,
        "Completion audit failure was hidden or misreported as prevented execution.");
}

void TestPartialAuditIsNotSilentlyExtended()
{
    AuditFixture fixture;
    const std::string partial = "{\"interrupted_record\":";
    {
        std::ofstream file(fixture.audit, std::ios::binary);
        file << partial;
    }
    const auto result = fixture.Execute(false);
    std::ifstream file(fixture.audit, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Check(!result.result.attempted && !result.auditError.empty() && after == partial,
        "A truncated audit tail was silently extended or allowed execution.");
}

void TestLegacyResultRecordsRemainIntact()
{
    AuditFixture fixture;
    const nlohmann::json historical = {
        {"action_id", "legacy"}, {"action", "list_directory"},
        {"attempted", true}, {"succeeded", true}};
    {
        std::ofstream file(fixture.audit);
        file << historical.dump() << '\n';
    }
    const auto result = fixture.Execute(false);
    const auto records = Records(fixture.audit);
    Check(result.Succeeded() && records.size() == 3 && records.front() == historical &&
        records[1].at("record_type") == "intent" && records[2].at("record_type") == "result",
        "New audit recording rejected or rewrote a valid historical result record.");
}

void TestSessionReportsTheAuditBoundary(const bool afterExecution)
{
    AuditFixture fixture;
    const auto audit = fixture.directory.root / "session-audit.jsonl";
    revia::runtime::ReviaSession session;
    ReviaSessionTestAccess::PrepareActions(session, fixture.directory.root);
    session.SetConfirmationHandler([](const ActionRequest&, const PolicyDecision&) { return true; });
    if (afterExecution)
    {
        ReviaSessionTestAccess::ObserveActions(session, [&](const ActionRequest&, const bool beginning)
        {
            if (!beginning) BreakCompletionAudit(audit);
        });
    }
    else std::filesystem::create_directory(audit);
    const auto result = ReviaSessionTestAccess::Execute(session, fixture.Request());
    Check(!result.succeeded && result.text.find("Audit error:") != std::string::npos &&
        result.reason.find("Audit error:") != std::string::npos,
        "The production session ignored action audit failure.");
    Check(std::filesystem::exists(fixture.Request().source) == afterExecution &&
        (result.text.find("Action succeeded:") != std::string::npos) == afterExecution,
        "Session reporting disagrees with the action that actually occurred.");
}

void TestGoalStopsWithoutVerificationOrRetry(const bool afterExecution)
{
    AuditFixture fixture;
    const GoalStore store((fixture.directory.root / "goals.db").string());
    GoalRunner runner(fixture.runtime, store);
    runner.SetConfirmationHandler([](const ActionRequest&, const PolicyDecision&) { return true; });
    int dispatches = 0;
    fixture.runtime.SetDispatchObserver([&](const ActionRequest&, const bool beginning)
    {
        if (beginning) ++dispatches;
        else if (afterExecution) BreakCompletionAudit(fixture.audit);
    });
    if (!afterExecution) std::filesystem::create_directory(fixture.audit);
    Goal goal;
    goal.title = "Create an audited fixture";
    goal.scope = fixture.runtime.Settings();
    GoalStep step;
    step.action = fixture.Request();
    step.check.type = ActionType::ListDirectory;
    step.check.source = fixture.directory.root;
    step.expected = "created";
    goal.steps.push_back(step);
    const auto result = runner.Run(goal);
    Check(result.status == GoalStatus::Failed && result.stopReason == StopReason::StoreError &&
        dispatches == 1 && result.spend.retries == 0,
        "The production goal continued or retried after action audit failure.");
    const auto saved = store.Load(result.id);
    Check(saved && saved->steps.front().attempts.size() == 1 &&
        saved->steps.front().attempts.front().executed == afterExecution &&
        !saved->steps.front().attempts.front().verified &&
        saved->steps.front().attempts.front().failure.find("Audit error:") != std::string::npos,
        "The durable goal record lost execution/audit failure evidence.");
}

void TestAnExclusiveWriterBlocksAdmission()
{
#ifdef _WIN32
    AuditFixture fixture;
    const HANDLE blocker = CreateFileW(fixture.audit.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(blocker != INVALID_HANDLE_VALUE, "Could not establish the audit writer-lock fixture.");
    const auto refused = fixture.Execute(false);
    CloseHandle(blocker);
    Check(!refused.result.attempted && !refused.auditError.empty(),
        "An exclusively locked audit did not prevent action execution.");
    const auto retried = fixture.Execute(false);
    Check(retried.Succeeded(), "Releasing an empty audit file's lock did not restore normal admission.");
#endif
}

} // namespace

void RunActionAuditTests()
{
    for (const bool scoped : {false, true})
    {
        TestUnavailableAuditPreventsExecution(scoped);
        TestIntentAndResultAreRecorded(scoped);
        TestCompletionFailurePreservesExecutionTruth(scoped);
    }
    TestPartialAuditIsNotSilentlyExtended();
    TestLegacyResultRecordsRemainIntact();
    TestAnExclusiveWriterBlocksAdmission();
    for (const bool afterExecution : {false, true})
    {
        TestSessionReportsTheAuditBoundary(afterExecution);
        TestGoalStopsWithoutVerificationOrRetry(afterExecution);
    }
    std::cout << "Required audit intent/result recording crosses real dispatch, session "
        "reporting and goal stopping; completed actions remain truthful.\n";
}
