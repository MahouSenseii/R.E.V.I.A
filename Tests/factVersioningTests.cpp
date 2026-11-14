#include "testSupport.h"
#include "Audit/contentDigest.h"
#include "Memory/factSlot.h"
#include "Memory/longTermMemory.h"
#include "Memory/memoryCorrection.h"
#include <algorithm>
#include <iostream>

namespace
{
using namespace revia::memory;
using revia::tests::Check;

MemoryScope Scope()
{
    MemoryScope value;
    value.participantId = "fact-owner";
    value.companionId = "fact-companion";
    value.participantSource = revia::identity::SpeakerSource::ExplicitIntroduction;
    value.audience = {revia::identity::AudienceKind::Private, "fact-private", 1, {value.participantId}};
    return value;
}

// A saved fact as the memory worker would hand it over: the slot is kept only when its value is in
// the person's own words, and the change kind is the host's reading of those words.
std::string Say(longTermMemory& store, const std::string& input, const std::string& summary, const std::string& attribute,
    const std::string& object, const std::string& value, MemorySubject subject = ParticipantSubject(Scope()))
{
    memoryDecision decision;
    decision.bSuccess = decision.bShouldRemember = true;
    decision.summary = summary;
    decision.category = "other";
    decision.subject = subject;
    if (subject.kind == MemorySubjectKind::Participant && FactValueInOwnWords(value, input))
    {
        decision.fact = FactSlot{*CanonicalFactAttribute(attribute), NormalizeFactObject(object), value};
        decision.factChange = ClassifyFactChange(input, value, store.PriorFactValue(subject, decision.fact->attribute, decision.fact->object));
        decision.factEvidence = input;
    }
    bool added; std::string id;
    Check(store.Save(decision, added, &id), "Fact fixture save failed: " + summary);
    return id;
}

const FactView* Find(const std::vector<FactView>& views, const std::string& attribute, const std::string& object = "")
{
    const auto found = std::find_if(views.begin(), views.end(), [&](const auto& v) { return v.attribute == attribute && v.object == object; });
    return found == views.end() ? nullptr : &*found;
}

void TestReadingThePersonsOwnWords()
{
    Check(ClassifyFactChange("My Civic is green, not blue", "green", "blue") == FactChange::Correction, "A negated old value is a correction.");
    Check(ClassifyFactChange("Actually it's green", "green", "blue") == FactChange::Correction, "An explicit correction cue is a correction.");
    Check(ClassifyFactChange("My Civic is green now", "green", "blue") == FactChange::Change, "A change over time is a change.");
    Check(ClassifyFactChange("I'm moving to Porto next month", "Porto", "Lisbon") == FactChange::Future, "A plan is never current.");
    Check(ClassifyFactChange("My Civic is green", "green", "blue") == FactChange::Statement, "A bare disagreement is not a correction.");
    Check(ClassifyFactChange("My Civic is green", "green", "") == FactChange::Statement, "A new slot is a statement.");
    Check(ClassifyFactChange("My husband's name is Will, not Bill", "Will", "Bill") == FactChange::Correction,
        "A name that is also a future word was read as a plan.");
    Check(ClassifyFactChange("My partner is called Will", "Will", "") == FactChange::Statement, "The name Will made a plan.");
    Check(ClassifyFactChange("I will move to Porto, not Lisbon", "Porto", "Lisbon") == FactChange::Future,
        "A real plan stopped being a plan once its values were set aside.");
    Check(!FactValueInOwnWords("green", "My sister said \"my car is green, not blue\""), "A value in quoted speech is not the person's own.");
    Check(!FactValueInOwnWords("purple", "My Civic is green"), "A value the model invented is dropped.");
    Check(FactValueInOwnWords("green", "my civic is GREEN"), "Case does not matter.");
    Check(!CanonicalFactAttribute("shoe size") && CanonicalFactAttribute("Car Color") == std::optional<std::string>("car colour"),
        "Only the closed attribute list makes a slot.");
}

void TestResolutionRules()
{
    const auto version = [](std::int64_t seq, std::string value, FactChange change)
    { FactVersion v; v.sequence = seq; v.memoryId = "m" + std::to_string(seq); v.value = value; v.summary = value; v.change = change; return v; };
    auto corrected = ResolveFactSlot({version(1, "blue", FactChange::Statement), version(2, "green", FactChange::Correction)});
    Check(corrected.state == FactState::UnreviewedCorrection && corrected.current->value == "green" && corrected.replaced.front().value == "blue",
        "A clear correction is current, flagged unreviewed, with the old value kept.");
    auto unclear = ResolveFactSlot({version(1, "blue", FactChange::Statement), version(2, "green", FactChange::Statement)});
    Check(unclear.state == FactState::Unresolved && !unclear.current && unclear.conflicting.size() == 2,
        "Without a clear correction neither value supersedes the other.");
    auto plan = ResolveFactSlot({version(1, "Lisbon", FactChange::Statement), version(2, "Porto", FactChange::Future)});
    Check(plan.current->value == "Lisbon" && plan.planned && plan.planned->value == "Porto", "A planned change leaves the current value.");
    auto approved = version(2, "green", FactChange::Statement);
    approved.reviewedCurrent = true;
    auto retired = version(1, "blue", FactChange::Statement);
    retired.reviewedHistorical = true;
    Check(ResolveFactSlot({retired, approved}).state == FactState::Reviewed, "An owner-approved revision is reviewed.");
}

void TestWhoTheFactIsAboutAndSeparateThings()
{
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    const auto me = ParticipantSubject(Scope());
    Say(store, "My Civic is blue", "The user's Civic is blue.", "car colour", "Civic", "blue");
    Say(store, "My sister's Civic is green", "The user's sister's Civic is green.", "car colour", "Civic", "green", MemorySubject{});
    Say(store, "My truck is red", "The user's truck is red.", "car colour", "truck", "red");
    auto views = store.FactViews(me);
    Check(Find(views, "car colour", "civic") && Find(views, "car colour", "civic")->current->value == "blue",
        "Someone else's fact must not supersede the speaker's.");
    Say(store, "My Civic is green, not blue", "The user's Civic is green.", "car colour", "Civic", "green");
    views = store.FactViews(me);
    Check(Find(views, "car colour", "civic")->current->value == "green" && Find(views, "car colour", "truck")->current->value == "red" &&
              Find(views, "car colour", "truck")->state == FactState::Stated,
        "Correcting one car must leave the other car alone.");
    // Two jobs at once are not a correction: without a clear cue nothing is replaced.
    Say(store, "I work as a nurse", "The user works as a nurse.", "job", "", "nurse");
    Say(store, "I also work as a baker", "The user works as a baker.", "job", "", "baker");
    views = store.FactViews(me);
    Check(Find(views, "job")->state == FactState::Unresolved && !Find(views, "job")->current,
        "A second job must not silently replace the first.");
}

void TestFutureChangeVersusCorrection()
{
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    const auto me = ParticipantSubject(Scope());
    Say(store, "I live in Lisbon", "The user lives in Lisbon.", "home city", "", "Lisbon");
    Say(store, "I'm moving to Porto next month", "The user is moving to Porto next month.", "home city", "", "Porto");
    auto view = *Find(store.FactViews(me), "home city");
    Check(view.current->value == "Lisbon" && view.planned && view.planned->value == "Porto", "A move next month is not yet current.");
    Say(store, "I've moved, I live in Porto now", "The user lives in Porto.", "home city", "", "Porto");
    view = *Find(store.FactViews(me), "home city");
    Check(view.current->value == "Porto" && view.state == FactState::UnreviewedChange, "Once it happened, the change is current.");
}

void TestPromptLookupAndReviewFirstThroughRestart()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto path = (fixture.root / "memory.db").string();
    const auto me = ParticipantSubject(Scope());
    std::string blue, green;
    {
        longTermMemory store(path);
        blue = Say(store, "My Civic is blue", "The user's Civic is blue.", "car colour", "Civic", "blue");
        green = Say(store, "Sorry, my Civic is green, not blue", "The user's Civic is green.", "car colour", "Civic", "green");
    }
    longTermMemory reopened(path);  // a restart: a new store object on the same database
    const auto scope = Scope();
    const auto block = reopened.BuildPromptBlock("", 6, {}, "", 0, &scope);
    Check(block.find("Current: The user's Civic is green.") != std::string::npos && block.find("corrected this from blue") != std::string::npos,
        "The prompt must give the corrected value as current and say what it replaced: " + block);
    Check(block.find("] The user's Civic is blue.") == std::string::npos, "The old value must not appear as a separate equal fact.");
    // Review-first: the old row is untouched and still active until the owner decides.
    const auto rows = reopened.Load();
    const auto old = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.id == blue; });
    Check(old != rows.end() && old->currentRevisionId.empty(), "Before review the old value must stay on record, not historical.");
    const auto proposals = ProposeFactCorrections(reopened, scope);
    Check(proposals.size() == 1 && proposals[0].original.id == blue && proposals[0].proposedMemoryId == green,
        "After a restart the correction must still be offered for review.");
    MemoryRevisionRequest request;
    request.ownerRequestId = "owner-approves-green";
    request.originalId = blue;
    request.expectedSummaryDigest = proposals[0].expectedSummaryDigest;
    request.priorReceiptId = proposals[0].original.revisionReceiptId;
    request.expectedReplacementId = green;
    request.expectedSubject = me;
    request.corrected = proposals[0].corrected;
    request.origin = {scope.companionId, "fact-session", 1, {}, {}, 1};
    request.audienceRevision = 1;
    request.reason = "Owner approved.";
    request.evidence = proposals[0].evidence;
    MemoryRevisionReceipt receipt; std::string error;
    Check(reopened.SaveOwnerRevision(request, receipt, error), "The owner's approval must still go through the revision path: " + error);
    Check(Find(reopened.FactViews(me), "car colour", "civic")->state == FactState::Reviewed && ProposeFactCorrections(reopened, scope).empty(),
        "After approval the fact is reviewed and no longer pending.");
}

// The prompt states only the most recently touched facts. A corrected fact older than those was
// then no longer resolved at all: a question about it retrieved the old row and the new row and
// listed them as two equal memories.
void TestACorrectionOutsideTheRecentFactsIsStillResolved()
{
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    const auto scope = Scope();
    Say(store, "My Civic is blue", "The user's Civic is blue.", "car colour", "Civic", "blue");
    Say(store, "Sorry, my Civic is green, not blue", "The user's Civic is green.", "car colour", "Civic", "green");
    for (int pet = 1; pet <= 14; ++pet)
    {
        const std::string name = "Pebble" + std::string(1, static_cast<char>('a' + pet));
        const std::string animal = "hamster" + std::to_string(pet);
        Say(store, "My " + animal + " is called " + name, "The user's " + animal + " is called " + name + ".", "pet name", animal, name);
    }
    Check(store.FactViews(ParticipantSubject(scope)).size() == 12 && !Find(store.FactViews(ParticipantSubject(scope)), "car colour", "civic"),
        "The fixture must push the corrected fact outside the default twelve slots.");

    const auto asked = store.BuildPromptBlock("what colour is my Civic", 6, {}, "", 0, &scope);
    Check(asked.find("Current: The user's Civic is green.") != std::string::npos && asked.find("corrected this from blue") != std::string::npos,
        "A retrieved fact outside the recent slots must still be stated as resolved: " + asked);
    Check(asked.find("] The user's Civic is blue.") == std::string::npos && asked.find("] The user's Civic is green.") == std::string::npos,
        "The old and the corrected value were listed as two equal memories: " + asked);

    // With nothing asked, the recent facts are stated and the older one is simply not raised.
    const auto idle = store.BuildPromptBlock("", 6, {}, "", 0, &scope);
    Check(idle.find("] The user's Civic is blue.") == std::string::npos && idle.find("] The user's Civic is green.") == std::string::npos,
        "An older fact's versions leaked into the recent list as separate memories: " + idle);
    std::size_t statedFacts = 0;
    for (std::size_t at = idle.find("- [fact: "); at != std::string::npos; at = idle.find("- [fact: ", at + 1)) ++statedFacts;
    Check(statedFacts == 12, "Resolving every slot must not state every slot: " + std::to_string(statedFacts));
}

// Live, the original "my car is a blue Civic" got no slot; only the correction did. The correction
// names the old value, which links the one earlier memory carrying it as the previous version.
void TestACorrectionLinksAnUnslottedOriginal()
{
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    const auto me = ParticipantSubject(Scope());
    const auto plain = [&](const std::string& summary)
    {
        memoryDecision d; d.bSuccess = d.bShouldRemember = true; d.summary = summary; d.category = "identity"; d.subject = me;
        bool added; std::string id; Check(store.Save(d, added, &id), "Plain save failed."); return id;
    };
    const auto blue = plain("The user's car is a blue Civic.");
    plain("The user's bike is blue.");  // also blue, but not the Civic: must not be linked
    const std::string input = "sorry, it's green, not blue. been staring at it for years lol";
    Check(CorrectedAwayValue(input) == "blue", "The corrected-away value was not read from the person's words.");
    Check(store.LinkCorrectedAwayFact(me, "car colour", "civic", "blue") == "blue", "The one matching earlier memory was not linked.");
    memoryDecision green; green.bSuccess = green.bShouldRemember = true; green.summary = "The user's car is green."; green.category = "other";
    green.subject = me; green.fact = FactSlot{"car colour", "civic", "green"};
    green.factChange = ClassifyFactChange(input, "green", store.PriorFactValue(me, "car colour", "civic"));
    green.factEvidence = input;
    bool added; std::string greenId; Check(store.Save(green, added, &greenId), "Correction save failed.");
    const auto view = *Find(store.FactViews(me), "car colour", "civic");
    Check(view.state == FactState::UnreviewedCorrection && view.current->value == "green" && view.replaced.front().memoryId == blue,
        "The linked original was not treated as the corrected-away value.");
    const auto scope = Scope();
    const auto block = store.BuildPromptBlock("", 6, {}, "", 0, &scope);
    Check(block.find("] The user's car is a blue Civic.") == std::string::npos && block.find("The user's bike is blue.") != std::string::npos,
        "The old car row still appears as an equal fact, or the unrelated bike was hidden: " + block);
    const auto proposals = ProposeFactCorrections(store, scope);
    Check(proposals.size() == 1 && proposals[0].original.id == blue && proposals[0].sameFactSlot &&
              proposals[0].original.category != proposals[0].corrected.category,
        "A correction across category labels of one slot was not offered for review.");
    // Ambiguous: two earlier rows carry the old value and no object narrows them, so nothing links.
    revia::tests::ScopedTestDirectory second;
    longTermMemory other((second.root / "memory.db").string());
    const auto plainOther = [&](const std::string& summary)
    { memoryDecision d; d.bSuccess = d.bShouldRemember = true; d.summary = summary; d.category = "identity"; d.subject = me;
      bool a; std::string id; Check(other.Save(d, a, &id), "Plain save failed."); };
    plainOther("The user works as a nurse three days a week.");
    plainOther("The user volunteers as a nurse at weekends.");
    Check(other.LinkCorrectedAwayFact(me, "job", "", "nurse").empty(), "An ambiguous earlier value must not be linked.");
}

MemoryRevisionRequest Approve(const MemoryCorrectionProposal& proposal, const std::string& requestId)
{
    MemoryRevisionRequest request;
    request.ownerRequestId = requestId;
    request.originalId = proposal.original.id;
    request.expectedSummaryDigest = proposal.expectedSummaryDigest;
    request.priorReceiptId = proposal.original.revisionReceiptId;
    request.expectedReplacementId = proposal.proposedMemoryId;
    request.expectedSubject = proposal.original.subject;
    request.corrected = proposal.corrected;
    request.origin = {Scope().companionId, "fact-session", 1, {}, {}, 1};
    request.audienceRevision = 1;
    request.reason = "Owner approved.";
    request.evidence = proposal.evidence;
    return request;
}

// "Blue after all": the return saves the same text as the original, which the store deduplicates
// onto the original row. That must count as the newest version, even after the owner had approved
// blue -> green and blue was historical.
void TestReturningToAnEarlierValue()
{
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    const auto me = ParticipantSubject(Scope());
    const auto scope = Scope();
    const auto blue = Say(store, "My Civic is blue", "The user's Civic is blue.", "car colour", "Civic", "blue");
    const auto green = Say(store, "Actually my Civic is green, not blue", "The user's Civic is green.", "car colour", "Civic", "green");
    auto proposals = ProposeFactCorrections(store, scope);
    MemoryRevisionReceipt receipt; std::string error;
    Check(proposals.size() == 1 && store.SaveOwnerRevision(Approve(proposals[0], "approve-green"), receipt, error),
        "The first correction could not be approved: " + error);
    const auto back = Say(store, "Sorry, my Civic is blue after all, not green", "The user's Civic is blue.", "car colour", "Civic", "blue");
    Check(back == blue, "The fixture expected the store to deduplicate onto the original blue row.");
    auto view = *Find(store.FactViews(me), "car colour", "civic");
    Check(view.current && view.current->value == "blue" && view.state == FactState::UnreviewedCorrection &&
              view.replaced.front().memoryId == green,
        "Returning to an earlier value was ignored: green is still current.");
    proposals = ProposeFactCorrections(store, scope);
    Check(proposals.size() == 1 && proposals[0].original.id == green && proposals[0].proposedMemoryId == blue,
        "The return to blue was not offered for review.");
    Check(store.SaveOwnerRevision(Approve(proposals[0], "approve-blue"), receipt, error), "Approving the return failed: " + error);
    view = *Find(store.FactViews(me), "car colour", "civic");
    Check(view.state == FactState::Reviewed && view.current->value == "blue", "After approval blue must be the reviewed value.");
}

// "Four, not three" must not adopt an unrelated earlier memory that happens to say "three".
void TestUnrelatedOldFactsAreNotLinked()
{
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    const auto me = ParticipantSubject(Scope());
    const auto plain = [&](const std::string& summary)
    { memoryDecision d; d.bSuccess = d.bShouldRemember = true; d.summary = summary; d.category = "identity"; d.subject = me;
      bool added; std::string id; Check(store.Save(d, added, &id), "Plain save failed."); return id; };
    const auto kids = plain("The user has three kids.");
    Check(store.LinkCorrectedAwayFact(me, "pet age", "", "three").empty(), "An unrelated fact was linked as the pet's earlier age.");
    const auto dog = plain("The user's dog Biscuit is three years old.");
    Check(store.LinkCorrectedAwayFact(me, "pet age", "", "three") == "three", "The dog's earlier age was not linked.");
    const auto view = *Find(store.FactViews(me), "pet age");
    Check(view.versions.size() == 1 && view.versions[0].memoryId == dog && view.versions[0].memoryId != kids,
        "The wrong earlier memory became the slot's previous version.");
    const auto scope = Scope();
    Check(store.BuildPromptBlock("", 6, {}, "", 0, &scope).find("The user has three kids.") != std::string::npos,
        "The unrelated fact disappeared from the prompt.");
}

// A rejected slot review is rebuilt from the store unless the rejection is stored too.
void TestARejectedReviewStaysRejected()
{
    revia::tests::ScopedTestDirectory fixture;
    const auto path = (fixture.root / "memory.db").string();
    const auto me = ParticipantSubject(Scope());
    const auto scope = Scope();
    std::string blue, green;
    {
        longTermMemory store(path);
        blue = Say(store, "My Civic is blue", "The user's Civic is blue.", "car colour", "Civic", "blue");
        green = Say(store, "Actually my Civic is green, not blue", "The user's Civic is green.", "car colour", "Civic", "green");
        Check(ProposeFactCorrections(store, scope).size() == 1, "The correction was not offered for review.");
        Check(!store.RecordFactCorrectionRejection(blue, "not-a-version", me), "A rejection outside the slot was recorded.");
        Check(store.RecordFactCorrectionRejection(blue, green, me), "The rejection was not recorded.");
    }
    longTermMemory reopened(path);  // a restart
    Check(ProposeFactCorrections(reopened, scope).empty(), "A rejected review came back after it was rebuilt from the store.");
    auto view = *Find(reopened.FactViews(me), "car colour", "civic");
    Check(view.state == FactState::Unresolved && !view.current, "After rejection the newer value must not be shown as the correction.");
    Say(reopened, "No, it's red now, not green", "The user's Civic is red.", "car colour", "Civic", "red");
    const auto later = ProposeFactCorrections(reopened, scope);
    Check(later.size() == 1 && later[0].original.id == green, "A later, different correction must still be offered for review.");
}

struct TestLease final : MemoryRevisionScopeLease {};

// The owner's deletion must not complete for a conversation that changed while it ran.
void TestDeletionStopsWhenTheConversationChanges()
{
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    const auto me = ParticipantSubject(Scope());
    const auto blue = Say(store, "My Civic is blue", "The user's Civic is blue.", "car colour", "Civic", "blue");
    const auto green = Say(store, "Actually my Civic is green, not blue", "The user's Civic is green.", "car colour", "Civic", "green");
    std::size_t deleted = 0; std::string error; int asked = 0;
    const MemoryRevisionScopeAdmission changed = [&]() -> std::unique_ptr<MemoryRevisionScopeLease> { ++asked; return {}; };
    Check(!store.DeleteOldFactVersions(green, me, deleted, error, changed) && deleted == 0 && asked == 1,
        "A deletion went ahead after the conversation changed.");
    const auto rows = store.Load();
    Check(std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return r.id == blue; }), "A refused deletion still removed the old version.");
    const MemoryRevisionScopeAdmission same = [&]() -> std::unique_ptr<MemoryRevisionScopeLease> { ++asked; return std::make_unique<TestLease>(); };
    Check(store.DeleteOldFactVersions(green, me, deleted, error, same) && deleted == 1 && asked == 2,
        "With the conversation unchanged the deletion should complete: " + error);
}

void TestOwnerDeletesOldVersions()
{
    revia::tests::ScopedTestDirectory fixture;
    longTermMemory store((fixture.root / "memory.db").string());
    const auto me = ParticipantSubject(Scope());
    Say(store, "I work as a nurse", "The user works as a nurse.", "job", "", "nurse");
    const auto baker = Say(store, "I'm a baker", "The user is a baker.", "job", "", "baker");
    std::size_t deleted = 0; std::string error;
    Check(!store.DeleteOldFactVersions(baker, me, deleted, error) && deleted == 0,
        "Deleting is refused while the fact is unresolved.");
    const auto blue = Say(store, "My Civic is blue", "The user's Civic is blue.", "car colour", "Civic", "blue");
    const auto green = Say(store, "Actually my Civic is green", "The user's Civic is green.", "car colour", "Civic", "green");
    MemorySubject other{MemorySubjectKind::Participant, "someone-else"};
    Check(!store.DeleteOldFactVersions(green, other, deleted, error), "Only the fact's own person can delete its versions.");
    Check(!store.DeleteOldFactVersions(blue, me, deleted, error), "Only the current value's old versions can be deleted.");
    Check(store.DeleteOldFactVersions(green, me, deleted, error) && deleted == 1, "The owner can delete the older version: " + error);
    const auto rows = store.Load();
    Check(std::none_of(rows.begin(), rows.end(), [&](const auto& r) { return r.id == blue; }) &&
              std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return r.id == green; }),
        "The old version is gone and the current one stays.");
    const auto view = *Find(store.FactViews(me), "car colour", "civic");
    Check(view.versions.size() == 1 && view.current->value == "green", "The fact now has one version.");
}
} // namespace

void RunFactVersioningTests()
{
    TestReadingThePersonsOwnWords();
    TestResolutionRules();
    TestWhoTheFactIsAboutAndSeparateThings();
    TestFutureChangeVersusCorrection();
    TestPromptLookupAndReviewFirstThroughRestart();
    TestACorrectionOutsideTheRecentFactsIsStillResolved();
    TestACorrectionLinksAnUnslottedOriginal();
    TestReturningToAnEarlierValue();
    TestUnrelatedOldFactsAreNotLinked();
    TestARejectedReviewStaysRejected();
    TestDeletionStopsWhenTheConversationChanges();
    TestOwnerDeletesOldVersions();
    std::cout << "Fact versioning checks passed: own words, who, separate things, plans, review-first, restart and owner delete.\n";
}
