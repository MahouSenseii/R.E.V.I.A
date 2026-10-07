#include "Actions/actionRuntime.h"
#include "Core/taskContract.h"
#include "Filesystem/fileSystemExecutor.h"
#include "testSupport.h"

#include <iostream>
#include <algorithm>
#include <chrono>

namespace revia::actions
{
// Exercise the ordinary policy, dispatcher, audit owner and native file executor.
struct ActionRuntimeTestAccess
{
    static void Configure(ActionRuntime& runtime, const std::filesystem::path& root)
    {
        CapabilitySettings settings;
        settings.mode = ExecutionMode::ApprovedScope;
        settings.approvedRoots = {root};
        settings.autoApproveRiskThrough = RiskLevel::ReversibleWrite;
        settings.createMissingApprovedRoots = false;
        runtime.settingsSnapshot = settings;
        runtime.policy = std::make_unique<policy::CapabilityPolicy>(settings);
        runtime.auditLogger = std::make_unique<audit::ActionAuditLogger>(root / "audit.jsonl");
        runtime.dispatcher.Register(std::make_unique<filesystem::FileSystemExecutor>(1048576, 500, 200));
    }
};
}

namespace
{
using namespace revia;
using tests::Check;

core::TaskContract Contract()
{
    core::TaskContract task;
    task.stamp = {"companion-a", "session-a", 7, "original-task", "original-attempt", 1};
    task.scope = {
        "owner-a", {identity::AudienceKind::Private, "private-owner", 4, {"owner-a"}}, identity::SpeakerSource::Platform, 8, "companion-a"};
    task.goal = "Create the requested disposable directory";
    task.negativeConstraints = {"Keep writes inside the approved fixture root"};
    task.deliverables = {"Native executor receipt"};
    task.acceptanceObligations = {"Record actual executor result; a single action does not prove whole-task completion"};
    task.cancellation.origin = task.stamp;
    task.sourceKind = "action";
    task.sourceId = "native-create";
    return task;
}

void ActualConsumerAdmissions()
{
    tests::ScopedTestDirectory directory;
    actions::ActionRuntime runtime;
    actions::ActionRuntimeTestAccess::Configure(runtime, directory.root);
    auto expected = Contract();
    auto currentScope = expected.scope;
    auto authority = std::make_shared<policy::CompanionAuthority>();
    authority->SetCompanionDefaults(expected.stamp.companionId, policy::AuthorityPermissions::WithinMachineCeiling());
    auto sessionStamp = expected.stamp;
    sessionStamp.taskId.clear();
    sessionStamp.attemptId.clear();
    Check(authority->RegisterSession(sessionStamp), "register test session");
    Check(authority->RegisterTask(expected.stamp, {}), "register existing task identity");
    expected.stamp.policyVersion = authority->Revision();
    expected.cancellation.origin = expected.stamp;
    runtime.BindAuthority(authority, expected.stamp);
    std::vector<std::string> refusals;
    runtime.BindTaskContracts(
        [&](const actions::ActionRequest& request, const runtime::RuntimeStamp&, std::stop_token)
        {
            auto task = expected;
            task.sourceId = request.id;
            return std::make_shared<const core::TaskContract>(std::move(task));
        },
        [&](const core::TaskContract& task, std::stop_token stop)
        {
            const auto validation = core::ValidateTaskAdmission(
                task, expected.stamp, currentScope, [&](const auto& stamp) { return core::SameRuntimeStamp(stamp, expected.stamp); }, stop);
            if (!validation)
            {
                refusals.push_back(validation.code);
            }
            return validation ? std::string{} : validation.code;
        });
    for (int index = 0; index < 100; ++index)
    {
        actions::ActionRequest request;
        request.id = "rejected-" + std::to_string(index);
        request.type = actions::ActionType::CreateDirectory;
        request.source = directory.root / request.id;
        auto task = expected;
        task.sourceId = request.id;
        if (index < 50)
        {
            task.stamp.generation = 6;
            task.cancellation.origin.generation = 6;
        }
        else
        {
            task.scope.audience.audienceId = "other-audience-" + std::to_string(index);
        }
        request.taskContract = std::make_shared<const core::TaskContract>(std::move(task));
        const auto outcome = runtime.Execute(request, true);
        Check(!outcome.result.succeeded && !outcome.result.attempted && !std::filesystem::exists(request.source),
            "actual native consumer admitted stale/cross-audience contract case " + std::to_string(index));
        Check(outcome.policy.verdict == actions::PolicyVerdict::Blocked, "contract rejection must be a named policy refusal");
    }
    actions::ActionRequest valid;
    valid.id = "valid-native";
    valid.type = actions::ActionType::CreateDirectory;
    valid.source = directory.root / valid.id;
    const auto admitted = runtime.Execute(valid, true);
    Check(admitted.Succeeded() && std::filesystem::is_directory(valid.source), "actual filesystem positive control");
    actions::ActionRequest changed = valid;
    changed.id = "changed-at-effect";
    changed.source = directory.root / changed.id;
    changed.beforeEffect = [&](const std::string&)
    {
        currentScope.audience.revision++;
        return std::string{};
    };
    const auto revoked = runtime.Execute(changed, true);
    Check(!revoked.result.succeeded && !std::filesystem::exists(changed.source), "scope must be rechecked after prior effect callback");
    currentScope = expected.scope;
    actions::ActionRequest unsupported = valid;
    unsupported.id = "unsupported";
    unsupported.source = directory.root / unsupported.id;
    auto unsupportedContract = expected;
    unsupportedContract.sourceId = unsupported.id;
    unsupportedContract.version.major = 2;
    unsupported.taskContract = std::make_shared<const core::TaskContract>(std::move(unsupportedContract));
    const auto refused = runtime.Execute(unsupported, true);
    Check(!refused.result.attempted && !std::filesystem::exists(unsupported.source), "unsupported major rejected before native executor");
    Check(!refusals.empty(), "host records admission reasons");
    auto journal = runtime.EvidenceJournalOwner();
    Check(static_cast<bool>(journal), "runtime exposes its shared journal owner");
    audit::JournalEvent unresolved;
    unresolved.kind = audit::JournalKind::Intent;
    unresolved.transactionId = "uncertain-original-operation";
    unresolved.evidence = {{}, "uncertain-reference", "host:original-operation", std::string(64, 'a'), "text/plain", expected.stamp,
        expected.scope, 1791360000000, "host:native-fixture"};
    Check(journal->Append(unresolved).Durable(), "persist actual unresolved intent");
    actions::ActionRequest dependent = valid;
    dependent.id = "dependent-uncertain";
    dependent.source = directory.root / dependent.id;
    const auto blocked = runtime.Execute(dependent, true);
    Check(!blocked.result.attempted && !std::filesystem::exists(dependent.source), "same task uncertainty blocks native dependency");
    auto revisionMarker = expected.stamp;
    revisionMarker.taskId = "policy-revision-marker";
    Check(authority->RegisterTask(revisionMarker, {}), "advance actual authority revision");
    expected.stamp.policyVersion = authority->Revision();
    expected.stamp.attemptId = "retry-attempt";
    expected.cancellation.origin = expected.stamp;
    runtime.BindAuthority(authority, expected.stamp);
    dependent.id = "dependent-after-policy-refresh";
    dependent.source = directory.root / dependent.id;
    Check(!runtime.Execute(dependent, true).result.attempted && !std::filesystem::exists(dependent.source),
        "same task uncertainty survives policy refresh and retry attempt");
    expected.stamp.taskId = "unrelated-task";
    Check(authority->RegisterTask(expected.stamp, {}), "register unrelated actual authority subject");
    expected.stamp.policyVersion = authority->Revision();
    expected.cancellation.origin = expected.stamp;
    runtime.BindAuthority(authority, expected.stamp);
    actions::ActionRequest unrelated = valid;
    unrelated.id = "unrelated";
    unrelated.source = directory.root / unrelated.id;
    Check(runtime.Execute(unrelated, true).Succeeded() && std::filesystem::is_directory(unrelated.source),
        "different task remains executable despite old uncertainty");
}

void StandaloneEnvelopeCannotBypassHost()
{
    tests::ScopedTestDirectory directory;
    actions::ActionRuntime runtime;
    actions::ActionRuntimeTestAccess::Configure(runtime, directory.root);
    actions::ActionRequest request;
    request.id = "standalone";
    request.type = actions::ActionType::CreateDirectory;
    request.source = directory.root / request.id;
    request.taskContract = std::make_shared<const core::TaskContract>(Contract());
    const auto refused = runtime.Execute(request, true);
    Check(!refused.result.attempted && !std::filesystem::exists(request.source), "supplied standalone envelope requires a host guard");
    request.taskContract.reset();
    const auto legacy = runtime.Execute(request, true);
    Check(legacy.Succeeded() && std::filesystem::is_directory(request.source), "standalone legacy behavior retained");
}

}

int main()
{
    try
    {
        ActualConsumerAdmissions();
        StandaloneEnvelopeCannotBypassHost();
        std::cout << "PASS 100 actual ActionRuntime stale/cross-audience refusals; native filesystem controls; final effect scope; "
                     "version/standalone gates\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
