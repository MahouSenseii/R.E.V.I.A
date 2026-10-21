#include "reviaSessionTestAccess.h"
#include "testSupport.h"
#include "Audit/contentDigest.h"
#include "Memory/longTermMemory.h"

#include <algorithm>
#include <future>

namespace
{
using namespace revia::runtime;
using namespace revia::memory;
using revia::tests::Check;

MemoryScope Prepare(ReviaSession& session)
{
    std::string error;
    ReviaSessionTestAccess::DisableQualityFixtureEmbedding(session);
    ReviaSessionTestAccess::MarkStudioStarted(session, true);
    Check(session.SetAudience({revia::identity::AudienceKind::Private, "correction-fixture", 0, {}}, error), error);
    Check(!ReviaSessionTestAccess::IntroduceTaskSpeaker(session, "ProposalOwner").empty(), "Fixture owner could not be identified.");
    (void)ReviaSessionTestAccess::StudioInput(session, revia::agents::InputSource::Typed);
    const auto scope = ReviaSessionTestAccess::CorrectionScope(session);
    Check(IsAttributedPrivateScope(scope), "Fixture correction needs exact private attribution.");
    return scope;
}

MemoryCorrectionProposal Proposal(longTermMemory& store, const MemoryScope& scope, const std::string& suffix)
{
    memoryDecision old;
    old.bSuccess = old.bShouldRemember = true;
    old.category = "preference";
    old.summary = "The user prefers tea " + suffix + ".";
    old.subject = ParticipantSubject(scope);
    bool added = false;
    std::string oldId, newId;
    Check(store.Save(old, added, &oldId), "Could not save old proposal fact.");
    auto corrected = old;
    corrected.summary = "The user now prefers coffee " + suffix + ".";
    Check(store.Save(corrected, added, &newId), "Could not save new proposal fact.");
    const auto entries = store.Load();
    const auto original = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.id == oldId; });
    Check(original != entries.end(), "Exact old proposal row is absent.");
    return {"proposal-" + suffix, *original, revia::audit::ContentDigest(original->summary), newId, corrected,
            "The owner said: I now prefer coffee.", scope, 7};
}

void TestReviewAcceptanceAndRejection()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"correction-review", "Correction review fixture", "assistant", false});
    ReviaSession session(paths);
    const auto scope = Prepare(session);
    longTermMemory store(paths.Resolve("Memory/revia_memory.db").string());
    const auto proposal = Proposal(store, scope, "accept");
    ReviaSessionTestAccess::QueueMemoryCorrections(session, {proposal, proposal});
    Check(session.PendingMemoryCorrections().size() == 1 && store.RevisionHistory().empty() && store.Load().size() == 2,
          "A proposal mutated facts or duplicate events duplicated pending review.");
    std::string error;
    MemoryRevisionReceipt receipt;
    Check(session.AcceptMemoryCorrectionProposal(proposal.id, receipt, error), error);
    Check(receipt.originalId == proposal.original.id && receipt.revisedId == proposal.proposedMemoryId && !receipt.wasAdded &&
              !receipt.requestId.empty() && receipt.requestId != proposal.id && store.Load().size() == 2 &&
              store.RevisionHistory().size() == 1 && session.PendingMemoryCorrections().empty(),
          "Owner acceptance did not bind the exact already-saved pair to one fresh receipt.");
    Check(!session.AcceptMemoryCorrectionProposal(proposal.id, receipt, error) && store.RevisionHistory().size() == 1,
          "A consumed proposal replayed owner authority.");
    const auto rejected = Proposal(store, scope, "reject");
    ReviaSessionTestAccess::QueueMemoryCorrections(session, {rejected});
    Check(session.RejectMemoryCorrectionProposal(rejected.id, error) && session.PendingMemoryCorrections().empty() &&
              store.Load().size() == 4 && store.RevisionHistory().size() == 1,
          "Rejecting a suggestion changed facts or wrote an owner revision.");
    auto changedReplacement = Proposal(store, scope, "replacement-changed");
    changedReplacement.corrected.summary = "The user now prefers a replacement that was never saved.";
    ReviaSessionTestAccess::QueueMemoryCorrections(session, {changedReplacement});
    const auto beforeRefusal = store.Load().size();
    Check(!session.AcceptMemoryCorrectionProposal(changedReplacement.id, receipt, error) &&
              store.Load().size() == beforeRefusal && store.RevisionHistory().size() == 1,
          "A changed proposed replacement inserted a new fact instead of refusing the exact saved pair.");
}

void TestScopeStalenessCancellationAndCapacity()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"correction-scope", "Correction scope fixture", "assistant", false});
    ReviaSession session(paths);
    auto scope = Prepare(session);
    longTermMemory store(paths.Resolve("Memory/revia_memory.db").string());
    const auto proposal = Proposal(store, scope, "scope");
    auto forged = proposal;
    forged.scope.participantId = "another-owner";
    ReviaSessionTestAccess::QueueMemoryCorrections(session, {forged});
    Check(session.PendingMemoryCorrections().empty(), "A foreign participant's proposal was disclosed.");
    auto retired = session.Stamp();
    ++retired.generation;
    ReviaSessionTestAccess::QueueMemoryCorrectionsFromOrigin(session, {proposal}, retired);
    Check(session.PendingMemoryCorrections().empty(), "A drained event was rebranded with the current session generation.");
    ReviaSessionTestAccess::QueueMemoryCorrections(session, {proposal});
    std::stop_source canceled;
    canceled.request_stop();
    MemoryRevisionReceipt receipt;
    std::string error;
    Check(!session.AcceptMemoryCorrectionProposal(proposal.id, receipt, error, canceled.get_token()) &&
              !session.RejectMemoryCorrectionProposal(proposal.id, error, canceled.get_token()) && store.RevisionHistory().empty(),
          "Canceled review wrote a receipt or performed rejection.");
    Check(session.SetAudience({revia::identity::AudienceKind::Public, "public", 0, {}}, error), error);
    Check(session.PendingMemoryCorrections().empty() && !session.AcceptMemoryCorrectionProposal(proposal.id, receipt, error),
          "A public audience disclosed or accepted a private suggestion.");
    scope = Prepare(session);
    ReviaSessionTestAccess::QueueMemoryCorrections(session, {proposal});
    Check(session.PendingMemoryCorrections().empty(), "Returning private reauthorized an old audience epoch.");
    const auto fresh = Proposal(store, scope, "capacity");
    std::vector<MemoryCorrectionProposal> many;
    for (int index = 0; index < 36; ++index)
    {
        auto value = fresh;
        value.id += std::to_string(index);
        value.original.id += std::to_string(index);
        value.proposedMemoryId += std::to_string(index);
        many.push_back(std::move(value));
    }
    ReviaSessionTestAccess::QueueMemoryCorrections(session, many);
    Check(session.PendingMemoryCorrections().size() == 32 && store.RevisionHistory().empty(),
          "Session proposals escaped their bound or gained persistence authority.");
    session.Stop();
    Check(session.PendingMemoryCorrections().empty(), "Session retirement retained disclosed correction proposals.");
}

void TestStaleChainAndCanceledForegroundWait()
{
    revia::tests::ScopedTestDirectory fixture;
    const CompanionPaths paths(fixture.root, {"correction-stale", "Correction stale fixture", "assistant", false});
    ReviaSession session(paths);
    const auto scope = Prepare(session);
    longTermMemory store(paths.Resolve("Memory/revia_memory.db").string());
    const auto stale = Proposal(store, scope, "stale");
    ReviaSessionTestAccess::QueueMemoryCorrections(session, {stale});
    MemoryRevisionRequest changed;
    changed.ownerRequestId = "separate-owner-request";
    changed.originalId = stale.original.id;
    changed.expectedSummaryDigest = stale.expectedSummaryDigest;
    changed.expectedSubject = stale.original.subject;
    changed.origin = session.Stamp();
    changed.audienceRevision = scope.audience.revision;
    changed.corrected = stale.corrected;
    changed.corrected.summary = "The user now prefers cocoa.";
    changed.reason = "The owner supplied a newer exact correction.";
    changed.evidence = "Independent owner review.";
    MemoryRevisionReceipt receipt;
    std::string error;
    Check(session.ReviseMemoryOwnerRequested(changed, receipt, error), error);
    const auto count = store.Load().size();
    Check(!session.AcceptMemoryCorrectionProposal(stale.id, receipt, error) && store.Load().size() == count &&
              store.RevisionHistory().size() == 1,
          "A stale selected predecessor gained a new revision receipt.");
    const auto pending = Proposal(store, scope, "canceled-wait");
    ReviaSessionTestAccess::QueueMemoryCorrections(session, {pending});
    auto foreground = ReviaSessionTestAccess::HoldForeground(session);
    std::stop_source stop;
    auto accepting = std::async(std::launch::async, [&] {
        return session.AcceptMemoryCorrectionProposal(pending.id, receipt, error, stop.get_token());
    });
    Check(accepting.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout,
          "Proposal acceptance bypassed the foreground mutation lock.");
    stop.request_stop();
    foreground.unlock();
    Check(!accepting.get() && store.RevisionHistory().size() == 1,
          "Canceled queued acceptance wrote after the foreground lock was released.");

    const auto changingScope = Proposal(store, scope, "scope-wait");
    ReviaSessionTestAccess::QueueMemoryCorrections(session, {changingScope});
    foreground.lock();
    auto staleScope = std::async(std::launch::async, [&] {
        return session.AcceptMemoryCorrectionProposal(changingScope.id, receipt, error);
    });
    Check(staleScope.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout,
          "Scope race fixture did not wait for the foreground mutation lock.");
    std::string audienceError;
    Check(session.SetAudience({revia::identity::AudienceKind::Public, "changed-before-lease", 0, {}}, audienceError), audienceError);
    foreground.unlock();
    Check(!staleScope.get() && store.RevisionHistory().size() == 1,
          "Acceptance failed to recheck scope when its commit lease was finally acquired.");
}
}

void RunMemoryCorrectionRuntimeTests()
{
    TestReviewAcceptanceAndRejection();
    TestScopeStalenessCancellationAndCapacity();
    TestStaleChainAndCanceledForegroundWait();
}
