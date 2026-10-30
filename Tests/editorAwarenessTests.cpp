#include "reviaSessionTestAccess.h"
#include "Audit/contentDigest.h"

#include <nlohmann/json.hpp>
#include <cstdlib>

namespace
{
using revia::tests::Check;
using Access = revia::runtime::ReviaSessionTestAccess;

struct Fixture
{
    revia::tests::ScopedTestDirectory temporary;
    revia::runtime::ReviaSession session{revia::runtime::CompanionPaths(temporary.root, {"editor-tests", "Revia", "assistant", false})};
    std::filesystem::path project = temporary.root / "approved";
    std::filesystem::path file = project / "review.cpp";

    Fixture()
    {
        std::filesystem::create_directories(project);
        std::ofstream(temporary.root / "capabilities.json") << nlohmann::json{
            {"mode", "approved_scope"}, {"approvedRoots", {revia::actions::PathToUtf8(project)}},
            {"autoApproveRiskThrough", "read_only"}, {"createMissingApprovedRoots", false}}.dump();
        std::string error;
        Check(Access::Actions(session).Initialize(temporary.root / "capabilities.json", temporary.root / "actions.jsonl", error), error);
        Access::ConfigureEditorFixture(session);
    }
    ~Fixture() { Access::MarkStudioStarted(session, false); }
    revia::presence::EditorObservationRequest Save(const std::string& source)
    {
        std::ofstream(file, std::ios::binary | std::ios::trunc) << source;
        return {revia::actions::PathToUtf8(project), revia::actions::PathToUtf8(file), revia::audit::ContentDigest(source),
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
    }
};

void TestFocusDoesNotReviveAnOldNomination()
{
    Fixture fixture;
    const auto request = fixture.Save("int RetainedSource() { return 11; }\n");
    Check(fixture.session.ObserveEditorFile(request).accepted, "The mode-transition control could not admit its source.");
    const auto cues = Access::EditorCues(fixture.session);
    Check(cues.size() == 1 && cues.front().admission(), "The mode-transition control was not initially current.");
    Check(fixture.session.SetPreference("initiative.mode", "focus").succeeded, "Focus preference failed.");
    Check(fixture.session.UserPreferences().initiativeMode == "focus", "Focus was saved without being applied to the running session.");
    Check(fixture.session.SetPreference("initiative.mode", "normal").succeeded, "Normal preference failed.");
    Check(!cues.front().admission(), "Returning from Focus revived a pre-Focus source nomination.");
    Check(Access::EditorSourceAvailable(fixture.session) &&
              Access::EditorContext(fixture.session).find("RetainedSource") != std::string::npos,
        "Focus discarded code that should remain available to direct conversation.");
}

void TestChangingFilesRetiresAnOptionalAttemptEvenWithIdenticalBytes()
{
    Fixture fixture;
    auto request = fixture.Save("int CurrentValue() { return 12; }\n");
    Check(fixture.session.ObserveEditorFile(request).accepted, "The file-switch control could not admit its first source.");
    const auto token = Access::ArmOptionalEditorAttempt(fixture.session);
    const auto duplicate = fixture.session.ObserveEditorFile(request);
    Check(!duplicate.accepted && duplicate.retryable && !token.stop_requested(),
        "An unchanged source notification interrupted its own optional generation.");
    const auto nextFile = fixture.project / "other.cpp";
    std::filesystem::copy_file(fixture.file, nextFile);
    request.path = revia::actions::PathToUtf8(nextFile);
    const auto result = fixture.session.ObserveEditorFile(request);
    Check(!result.accepted && result.retryable, "A busy source notification was not safely deferred.");
    Check(token.stop_requested(), "Changing to another saved file with identical bytes did not retire the old optional attempt.");
    Access::ReleaseOptionalEditorAttempt(fixture.session);
}

void TestAuthorizedSourceRevisionsAndSuppression()
{
    Fixture fixture;
    Check(!Access::EditorSourceAvailable(fixture.session), "A session without a saved snapshot reported current source availability.");
    auto request = fixture.Save("const char* Label() { return \"ready\"; }\n");
    const auto received = fixture.session.ObserveEditorFile(request);
    Check(received.accepted, "An approved saved source event was refused: " + received.reason);
    auto cues = Access::EditorCues(fixture.session);
    Check(cues.size() == 1 && cues.front().optionalObservation && cues.front().admission(),
        "A saved source receipt did not reach the actual initiative cue owner with current admission.");
    const auto guard = cues.front().admission;
    Check(Access::EditorSourceAvailable(fixture.session) && Access::EditorContext(fixture.session).find("Label") != std::string::npos,
        "The current source was unavailable for a coherent follow-up.");
    Check(fixture.session.ObserveEditorFile(request).accepted && Access::EditorCues(fixture.session).empty(),
        "An unchanged revision scheduled another model call.");
    std::ofstream(fixture.file) << "const char* Label() { return \"changed\"; }\n";
    Check(!guard() && !Access::EditorSourceAvailable(fixture.session) &&
              Access::EditorContext(fixture.session).find("no longer current") != std::string::npos,
        "A changed file kept an old comment or a falsely current follow-up source.");
    request = fixture.Save("const char* Label() { return \"new\"; }\n");
    Check(fixture.session.ObserveEditorFile(request).accepted, "A new stable revision was refused.");
    request = fixture.Save("const char* Label() { return \"newest\"; }\n");
    Check(fixture.session.ObserveEditorFile(request).accepted, "The newest stable revision was refused.");
    cues = Access::EditorCues(fixture.session);
    Check(cues.size() == 1 && cues.front().evidence.find("newest") != std::string::npos,
        "Related rapid revisions were not combined into the newest nomination.");
    fixture.session.SetPerceptionPaused(true);
    Check(!cues.front().admission() && !fixture.session.ObserveEditorFile(request).accepted &&
              !Access::EditorSourceAvailable(fixture.session) && Access::EditorContext(fixture.session).find("paused") != std::string::npos,
        "Pausing left source context or a queued comment active.");
}

void TestScopeFreshnessAndIncompleteTyping()
{
    Fixture fixture;
    auto request = fixture.Save("int Value() { return 7; }\n");
    auto denied = request;
    denied.path = revia::actions::PathToUtf8(fixture.temporary.root / "outside.cpp");
    std::ofstream(fixture.temporary.root / "outside.cpp") << "int f() { return 8; }\n";
    Check(!fixture.session.ObserveEditorFile(denied).accepted, "An unapproved source file was admitted.");
    denied = request; denied.projectRoot = revia::actions::PathToUtf8(fixture.temporary.root);
    Check(!fixture.session.ObserveEditorFile(denied).accepted, "An unapproved project root was admitted.");
    denied = request; denied.revision = std::string(64, '0');
    Check(!fixture.session.ObserveEditorFile(denied).accepted, "A stale source digest was admitted.");
    denied = request; denied.occurredAtUnixMs -= 121000;
    Check(!fixture.session.ObserveEditorFile(denied).accepted, "An expired source observation was admitted.");
    request = fixture.Save("int BeingTyped() {\n");
    Check(!fixture.session.ObserveEditorFile(request).accepted && Access::EditorCues(fixture.session).empty(),
        "Incomplete saved typing caused a criticism nomination.");
    request = fixture.Save("int Value() { return 9; }\n");
    Check(fixture.session.ObserveEditorFile(request).accepted, "Healthy source admission failed after negative controls.");
    const auto cues = Access::EditorCues(fixture.session);
    Access::RetireEditorInput(fixture.session);
    Check(!cues.front().admission(), "Direct input did not retire a pending source remark.");
}

void TestTransientSourceRetention()
{
    Fixture fixture;
    const auto request = fixture.Save("int PrivateFunctionLabel() { return 10; }\n");
    Check(fixture.session.ObserveEditorFile(request).accepted, "The retention control could not admit its source.");
    auto cues = Access::EditorCues(fixture.session);
    const auto considered = Access::ReserveEditorCue(fixture.session, cues.front());
    Check(considered.hasProposal && considered.proposal.evidence.find("PrivateFunctionLabel") != std::string::npos,
        "The actual transient nomination lost its source grounding.");
    for (const auto& pending : fixture.session.PendingProposals())
        Check(pending.evidence.find("PrivateFunctionLabel") == std::string::npos &&
            pending.subject.find("PrivateFunctionLabel") == std::string::npos && !pending.admission,
            "The outcome ledger retained raw source or its owning admission closure.");
    Access::ExpireEditorSnapshot(fixture.session);
    Check(!cues.front().admission() && !Access::EditorSourceAvailable(fixture.session) && Access::EditorContext(fixture.session).empty(),
        "Expired source remained available or alive through a captured speech/cue guard.");
}

void TestRevokedSourceAndFocusAdmission()
{
    Fixture fixture;
    auto request = fixture.Save("int Value() { return 10; }\n");
    Check(fixture.session.ObserveEditorFile(request).accepted, "The revocation control could not admit its source.");
    auto cues = Access::EditorCues(fixture.session);
    const auto guard = Access::EditorDeliveryAdmission(fixture.session, cues.front());
    Access::EditorFocusMode(fixture.session);
    Check(!guard(), "Focus mode did not reject an already captured optional opening.");
    std::ofstream(fixture.temporary.root / "revoked.json") << nlohmann::json{
        {"mode", "approved_scope"}, {"approvedRoots", nlohmann::json::array()}, {"createMissingApprovedRoots", false}}.dump();
    std::string error;
    Check(Access::Actions(fixture.session).Initialize(fixture.temporary.root / "revoked.json", fixture.temporary.root / "revoked-audit.jsonl", error), error);
    Check(!cues.front().admission() && !Access::EditorSourceAvailable(fixture.session),
        "Revoked file access retained source admission or current source availability.");
    fixture.session.ForgetActivity();
    Check(!Access::EditorSourceAvailable(fixture.session) && Access::EditorContext(fixture.session).empty() &&
              Access::EditorCues(fixture.session).empty(),
        "Forget retained a saved source snapshot or nomination.");
}

void TestCapturedAttentionKeepsNativeHardGatesInBothLoadPhases()
{
    Fixture fixture;
    const auto request = fixture.Save("int AttentionControl() { return 6; }\n");
    Check(fixture.session.ObserveEditorFile(request).accepted, "The attention control could not admit its saved source.");
    const auto cues = Access::EditorCues(fixture.session);
    Check(cues.size() == 1, "The attention control had no saved-source cue.");
    Access::SetEditorLoadAdjustment(fixture.session, revia::resources::LoadState::Normal, true);
    const auto idleStarting = Access::EditorDeliveryAdmission(fixture.session, cues.front());
    const auto idleRunning = Access::EditorDeliveryAdmission(fixture.session, cues.front(), false);
    Check(idleStarting() && idleRunning(), "An idle attention control was not admitted in both load phases.");
    for (const bool inCall : {true, false})
    {
        revia::initiative::AttentionContext blocked;
        blocked.inCall = inCall;
        blocked.foregroundIsExcluded = !inCall;
        const auto starting = Access::EditorDeliveryAdmission(fixture.session, cues.front(), true, blocked);
        const auto running = Access::EditorDeliveryAdmission(fixture.session, cues.front(), false, blocked);
        Check(!starting() && !running(), "A captured call or excluded application bypassed an admission hard gate.");
    }
    Check(idleStarting() && idleRunning(), "A blocked attention sample changed another captured admission guard.");
}

void TestOptionalOpeningLoadAdmissionDistinguishesStartingFromRunning()
{
    using revia::resources::LoadState;
    Fixture fixture;
    const auto request = fixture.Save("int ResidentReview() { return 3; }\n");
    Check(fixture.session.ObserveEditorFile(request).accepted, "The load control could not admit its saved source.");
    const auto cues = Access::EditorCues(fixture.session);
    Check(cues.size() == 1, "The load control had no saved-source cue.");
    const auto starting = Access::EditorDeliveryAdmission(fixture.session, cues.front());
    const auto running = Access::EditorDeliveryAdmission(fixture.session, cues.front(), false);
    Access::SetEditorLoadAdjustment(fixture.session, LoadState::Normal, false);
    Check(!starting() && running(), "Busy compute either started new optional work or cancelled its own running inference.");
    Access::SetEditorLoadAdjustment(fixture.session, LoadState::Throttled, false);
    Check(!starting() && !running(), "Throttling did not retire optional work in both phases.");
}

void TestRunningLoadPhasePreservesTheOriginalAdmissionBoundaries()
{
    using revia::resources::LoadState;
    for (const int denial : {0, 1, 2, 3})
    {
        Fixture fixture;
        const auto request = fixture.Save("int OriginalAdmission() { return 4; }\n");
        Check(fixture.session.ObserveEditorFile(request).accepted, "The running-admission control could not admit its source.");
        const auto cues = Access::EditorCues(fixture.session);
        Check(cues.size() == 1, "The running-admission control had no saved-source cue.");
        const auto starting = Access::EditorDeliveryAdmission(fixture.session, cues.front());
        const auto running = Access::EditorDeliveryAdmission(fixture.session, cues.front(), false);
        Access::SetEditorLoadAdjustment(fixture.session, LoadState::Normal, true);
        Check(starting() && running(), "The running-admission control was not initially allowed.");
        if (denial == 0)
            Access::EditorFocusMode(fixture.session);
        else if (denial == 1)
            Access::RetireEditorInput(fixture.session);
        else if (denial == 2)
            std::ofstream(fixture.file, std::ios::binary | std::ios::trunc) << "int ChangedSource() { return 5; }\n";
        else
            fixture.session.Authority()->SetEmergencyStopped(fixture.session.Stamp().companionId, true);
        Check(!starting() && !running(), "The running load phase bypassed Focus, direct input, source revision or authority.");
    }
}

void TestExpiryAndFreshSavePreserveTheCurrentSnapshotInEitherOrder()
{
    for (const bool expireFirst : {true, false})
    {
        Fixture fixture;
        const auto now = std::chrono::system_clock::now();
        auto old = fixture.Save("int OldSavedValue() { return 1; }\n");
        old.occurredAtUnixMs -= 100000;
        Check(fixture.session.ObserveEditorFile(old).accepted, "The ordered expiry control could not admit its old source.");
        const auto oldCues = Access::EditorCues(fixture.session);
        Check(oldCues.size() == 1, "The ordered expiry control lost its old nomination.");
        const auto oldDeadline = now + std::chrono::seconds{21};
        if (expireFirst)
        {
            Access::ExpireEditorSnapshotAt(fixture.session, oldDeadline);
            Check(!Access::EditorSourceAvailable(fixture.session) && Access::EditorContext(fixture.session).empty() &&
                      Access::EditorCues(fixture.session).empty(),
                "Expired source or its queued bytes survived retirement.");
        }

        const auto fresh = fixture.Save("int FreshSavedValue() { return 2; }\n");
        Check(fixture.session.ObserveEditorFile(fresh).accepted, "A fresh source could not replace the old snapshot.");
        const auto token = Access::ArmOptionalEditorAttempt(fixture.session);
        Access::ExpireEditorSnapshotAt(fixture.session, oldDeadline);
        const auto freshCues = Access::EditorCues(fixture.session);
        Check(!token.stop_requested() && freshCues.size() == 1 && freshCues.front().admission() &&
                  freshCues.front().evidence.find("FreshSavedValue") != std::string::npos &&
                  Access::EditorSourceAvailable(fixture.session) &&
                  Access::EditorContext(fixture.session).find("FreshSavedValue") != std::string::npos,
            "The old expiry deadline cleared or cancelled the fresh saved revision.");
        Check(!oldCues.front().admission(), "Replacement or expiry revived the old source nomination.");
        Access::ReleaseOptionalEditorAttempt(fixture.session);
    }
}

void TestFreshEditorOpeningSurvivesTheFirstPrivateSubmit()
{
    Fixture fixture;
    const auto request = fixture.Save("int CurrentValue() { return 12; }\n");
    Check(fixture.session.ObserveEditorFile(request).accepted, "The fresh continuity control could not admit its saved source.");
    Check(Access::CorrectionScope(fixture.session).participantId == "local:user",
        "The fresh continuity control unexpectedly had an attributed participant.");
    Check(Access::PrepareEditorOpeningContext(fixture.session), "A fresh local private opening could not bind transient context.");
    const std::string opening = "The saved CurrentValue function returns twelve.";
    Access::RecordDeliveredEditorOpening(fixture.session, opening);
    const auto followup = fixture.session.Submit("/help");
    Check(followup.succeeded, "The first private command could not traverse normal input admission: " + followup.reason);
    const auto history = Access::WorkingContext(fixture.session);
    Check(history.size() == 1 && history.front().role == "assistant" && history.front().content == opening,
        "The first private Submit erased its already delivered optional opening.");

    Check(!Access::IntroduceTaskSpeaker(fixture.session, "Another Participant").empty(),
        "The continuity control could not switch participants.");
    Check(!Access::PrepareEditorOpeningContext(fixture.session, false) && Access::WorkingContext(fixture.session).size() == 1,
        "A refused context preparation changed the previous participant's context.");
    Check(Access::PrepareEditorOpeningContext(fixture.session) && Access::WorkingContext(fixture.session).empty(),
        "Changing participants retained another participant's optional opening.");
}

void TestOptionalOpeningRetirementSettlesOnlyItsOwnedTransientState()
{
    using revia::runtime::RuntimeState;
    for (const auto transient : {RuntimeState::Thinking, RuntimeState::Responding})
    {
        Fixture fixture;
        const auto origin = fixture.session.Stamp();
        const auto audience = fixture.session.Audience();
        const auto token = Access::BeginEditorRetirementFixture(fixture.session, transient);
        unsigned idleEvents = 0;
        fixture.session.Events().Subscribe([&](const revia::runtime::RuntimeEvent& event)
        {
            if (event.kind == revia::runtime::RuntimeEventKind::StateChanged && event.state == RuntimeState::Idle)
            {
                Check(fixture.session.Audience().revision == audience.revision,
                    "Optional retirement published under the wrong audience or held its audience lock.");
                ++idleEvents;
            }
        });
        Access::RetireEditorOpening(fixture.session, token, origin, audience);
        Check(!token.stop_requested() && fixture.session.State() == RuntimeState::Idle && idleEvents == 1,
            "A guard-retired optional operation left its owned transient state active.");
        Access::RetireEditorOpening(fixture.session, token, origin, audience);
        Check(idleEvents == 1, "An already settled optional operation published duplicate Idle events.");
    }
    for (const auto stable : {RuntimeState::Error, RuntimeState::Acting, RuntimeState::Offline})
    {
        Fixture fixture;
        const auto token = Access::BeginEditorRetirementFixture(fixture.session, stable);
        Access::RetireEditorOpening(fixture.session, token, fixture.session.Stamp(), fixture.session.Audience());
        Check(fixture.session.State() == stable, "Optional retirement overwrote a state it did not own.");
    }
}

void TestOptionalOpeningRetirementRejectsSupersededOwnership()
{
    using revia::runtime::RuntimeState;
    Fixture superseded;
    const auto origin = superseded.session.Stamp();
    const auto audience = superseded.session.Audience();
    const auto oldToken = Access::BeginEditorRetirementFixture(superseded.session, RuntimeState::Thinking);
    const auto currentToken = Access::BeginEditorRetirementFixture(superseded.session, RuntimeState::Responding);
    Access::RetireEditorOpening(superseded.session, oldToken, origin, audience);
    Check(superseded.session.State() == RuntimeState::Responding, "An old optional operation reset a newer operation's state.");
    Access::RetireEditorOpening(superseded.session, currentToken, origin, audience);
    Check(superseded.session.State() == RuntimeState::Idle, "The current operation could not settle after the old one was refused.");

    Fixture changedAudience;
    const auto oldAudience = changedAudience.session.Audience();
    const auto audienceToken = Access::BeginEditorRetirementFixture(changedAudience.session, RuntimeState::Thinking);
    Access::ChangeEditorRetirementAudience(changedAudience.session);
    Access::RetireEditorOpening(changedAudience.session, audienceToken, changedAudience.session.Stamp(), oldAudience);
    Check(changedAudience.session.State() == RuntimeState::Thinking, "An old audience's optional operation reset current state.");

    Fixture changedSession;
    const auto oldSession = changedSession.session.Stamp();
    const auto sessionAudience = changedSession.session.Audience();
    const auto sessionToken = Access::BeginEditorRetirementFixture(changedSession.session, RuntimeState::Thinking);
    Access::RestartEditorRetirementSession(changedSession.session);
    Access::RetireEditorOpening(changedSession.session, sessionToken, oldSession, sessionAudience);
    Check(changedSession.session.State() == RuntimeState::Thinking, "An old companion session's optional operation reset current state.");
}
}

void RunEditorAwarenessTests()
{
    if (std::getenv("REVIA_EDITOR_FILE_SWITCH_ONLY"))
    {
        TestChangingFilesRetiresAnOptionalAttemptEvenWithIdenticalBytes();
        return;
    }
    TestFocusDoesNotReviveAnOldNomination();
    TestChangingFilesRetiresAnOptionalAttemptEvenWithIdenticalBytes();
    TestAuthorizedSourceRevisionsAndSuppression();
    TestScopeFreshnessAndIncompleteTyping();
    TestTransientSourceRetention();
    TestRevokedSourceAndFocusAdmission();
    TestOptionalOpeningLoadAdmissionDistinguishesStartingFromRunning();
    TestCapturedAttentionKeepsNativeHardGatesInBothLoadPhases();
    TestRunningLoadPhasePreservesTheOriginalAdmissionBoundaries();
    TestExpiryAndFreshSavePreserveTheCurrentSnapshotInEitherOrder();
    TestFreshEditorOpeningSurvivesTheFirstPrivateSubmit();
    TestOptionalOpeningRetirementSettlesOnlyItsOwnedTransientState();
    TestOptionalOpeningRetirementRejectsSupersededOwnership();
}
