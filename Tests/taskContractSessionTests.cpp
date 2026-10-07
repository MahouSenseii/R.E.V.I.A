#include "Core/taskContract.h"
#include "Runtime/reviaSession.h"
#include "testSupport.h"
#include <algorithm>
#include <iostream>

namespace revia::runtime
{
struct ReviaSessionTestAccess
{
    static agents::InputBatch Input(ReviaSession& session, const std::string& text)
    {
        session.started.store(true);
        agents::InputBatch batch;
        batch.text = text;
        batch.context = session.CaptureInputContext(agents::InputSource::Typed);
        batch.acceptedAt = std::chrono::steady_clock::now();
        return batch;
    }

    static SessionResult Run(ReviaSession& session, const agents::InputBatch& input)
    {
        std::lock_guard lock(session.operationMutex);
        return session.RunTurnLocked(input);
    }

    static void Promote(ReviaSession& session, agents::InputBatch& batch)
    {
        tests::Check(!session.ResolveLocalSpeaker("My name is Mira", batch.context).empty(), "actual speaker promotion");
    }

    static std::shared_ptr<audit::EvidenceJournal> StartJournal(ReviaSession& session, const std::filesystem::path& root)
    {
        const auto config = root / "fixture-capabilities.json";
        {
            std::ofstream output(config);
            output << "{}";
        }
        std::string error;
        tests::Check(
            session.actionRuntime.Initialize(config, root / "fixture-journal.jsonl", error), "initialize real session audit owner");
        auto journal = session.actionRuntime.EvidenceJournalOwner();
        session.runtimeEvidenceBridge.Start(session.eventBus, journal);
        RuntimeEvent event;
        event.kind = RuntimeEventKind::Warning;
        event.stamp = session.sessionIdentity.Stamp({}, {}, session.companionAuthority->Revision());
        event.message = "private fixture content must remain absent";
        session.eventBus.Publish(event);
        return journal;
    }
};
}

namespace
{
using namespace revia;
using tests::Check;

void SessionContractPropagation()
{
    tests::ScopedTestDirectory directory;
    runtime::ReviaSession session(runtime::CompanionPaths(directory.root, {"fixture", "Fixture", "assistant", false}));
    using Access = runtime::ReviaSessionTestAccess;
    auto input = Access::Input(session, "/help");
    Access::Promote(session, input);
    input.context.stamp.taskId = "original-task-identity";
    input.context.stamp.attemptId = "original-attempt-identity";
    const auto first = Access::Run(session, input);
    Check(first.succeeded && first.taskContract, "actual admitted session result carries a contract");
    Check(core::SameRuntimeStamp(first.taskContract->stamp, input.context.stamp), "session preserves populated original identity fields");
    Check(first.taskContract->scope.participantId == input.context.participantId &&
              first.taskContract->scope.participantSource == input.context.participantSource,
        "contract captures promoted participant provenance");
    Check(!first.taskContract->acceptanceObligations.empty() && !first.taskContract->negativeConstraints.empty(),
        "session explicitly captures obligations and restrictions");
    Check(core::SameRuntimeStamp(first.stamp, first.taskContract->stamp), "outer guard cannot erase task and attempt identity");
    auto legacyInput = Access::Input(session, "/help");
    const auto legacy = Access::Run(session, legacyInput);
    Check(legacy.succeeded && legacy.taskContract && !legacy.taskContract->stamp.taskId.empty() &&
              !legacy.taskContract->stamp.attemptId.empty(),
        "legacy accepted input receives explicit host migration identities");
    Check(std::any_of(legacy.taskContract->positiveConstraints.begin(), legacy.taskContract->positiveConstraints.end(),
              [](const auto& text) { return text.find("legacy") != std::string::npos; }),
        "migration remains explicit contract provenance");
    auto staleInput = legacyInput;
    staleInput.context.audience.revision++;
    const auto stale = Access::Run(session, staleInput);
    Check(!stale.succeeded && !stale.taskContract && stale.text.empty(), "stale session input cannot publish a contract or response");
}

void SessionShutdownFlushesRedactedLifecycle()
{
    tests::ScopedTestDirectory directory;
    runtime::ReviaSession session(runtime::CompanionPaths(directory.root, {"fixture", "Fixture", "assistant", false}));
    const auto journal = runtime::ReviaSessionTestAccess::StartJournal(session, directory.root);
    auto stamp = session.Stamp();
    stamp.taskId = "runtime-observations";
    stamp.attemptId = "lifecycle";
    memory::MemoryScope systemScope;
    systemScope.companionId = stamp.companionId;
    systemScope.audience = {identity::AudienceKind::Unknown, "runtime-system", 0, {}};
    session.Stop();
    const auto observations = journal->Read({stamp, systemScope, {}, {audit::JournalKind::Observation}});
    Check(!observations.empty(), "actual session shutdown flushes lifecycle evidence");
    for (const auto& observation : observations)
        Check(observation.scope.participantId.empty() && observation.sourceId == "runtime-lifecycle" && observation.observedAtUnixMs > 0,
            "lifecycle uses original observation time and an explicit unattributed system scope");
    std::ifstream input(journal->Path());
    const std::string saved((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Check(saved.find("private fixture content") == std::string::npos, "private event text cannot enter the lifecycle journal");
}
}

int main()
{
    try
    {
        SessionContractPropagation();
        SessionShutdownFlushesRedactedLifecycle();
        std::cout << "PASS actual SessionResult propagation, promoted participant, original identity, explicit legacy migration and stale "
                     "rejection\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
