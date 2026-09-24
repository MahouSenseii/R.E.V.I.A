#include "testSupport.h"

#include "Computer/experienceRecorder.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

using namespace revia::computer;
using revia::tests::Check;

// The opt-in dataset, exercised on controlled fixture content.
//
// Nothing here records anything a person did. Every value below is invented for the
// test, which is the same rule the implementation is written to: a dataset is built
// from demonstrations somebody opened a session for, never from whatever happened to be
// on screen.

CaptureConsent Consent(
    const ExperienceProvenance provenance = ExperienceProvenance::HumanDemonstration,
    const CaptureDepth depth = CaptureDepth::Structure)
{
    CaptureConsent consent;
    consent.application = "notepad.exe";
    consent.provenance = provenance;
    consent.depth = depth;
    return consent;
}

ExperienceRecord Row(const std::string& application = "notepad.exe")
{
    ExperienceRecord record;
    record.goalId = "goal-1";
    record.subgoalId = "subgoal-1";
    record.application = application;
    record.intent = SubgoalIntent::InteractWithControl;
    record.provider = "routine";
    record.decision = ComputerDecisionKind::ProposeAction;
    record.action = revia::actions::ActionType::InvokeControl;
    record.target = "Send";
    record.executed = true;
    record.outcome = revia::goals::VerificationOutcome::Verified;
    record.checkedBy = revia::goals::PostconditionKind::ControlValueIs;
    return record;
}

ExperienceRecord::Candidate Candidate(
    const std::string& name, const std::string& value = {})
{
    ExperienceRecord::Candidate candidate;
    candidate.name = name;
    candidate.role = "edit";
    candidate.mayEdit = true;
    candidate.value = value;
    return candidate;
}

// Off means off. Not "off in the configuration file": with no session open there is
// nothing to write to and nothing is written.
void TestNothingIsRecordedWithoutACaptureSession()
{
    revia::tests::ScopedTestDirectory directory;
    ComputerExperienceRecorder recorder(directory.root / "dataset");

    Check(!recorder.Capturing(), "The recorder started capturing on its own.");
    Check(!recorder.Record(Row()),
        "A record was written with no capture session open.");
    Check(recorder.Status().lastRefusal == CaptureRefusal::NotCapturing,
        "The refusal did not say why nothing was recorded.");
    Check(recorder.Sessions().empty(),
        "A dataset file appeared for a session that was never opened.");
}

// A session has to name what it is recording and what the rows will be evidence of.
void TestACaptureSessionMustNameItsScopeAndItsMeaning()
{
    revia::tests::ScopedTestDirectory directory;
    ComputerExperienceRecorder recorder(directory.root / "dataset");

    CaptureConsent unnamed;
    unnamed.provenance = ExperienceProvenance::HumanDemonstration;
    Check(!recorder.Begin(unnamed),
        "A capture session with no named application was opened, which is a request to "
        "record the whole desktop.");

    CaptureConsent meaningless;
    meaningless.application = "notepad.exe";
    Check(!recorder.Begin(meaningless),
        "A capture session was opened without saying what its rows demonstrate.");

    Check(recorder.Begin(Consent()), "A well-formed capture session was refused.");
    Check(recorder.Capturing() && !recorder.Status().sessionId.empty(),
        "An opened session did not report itself as capturing.");
}

// The window the session was opened for, and nothing else.
void TestRecordingStaysInsideTheConsentedApplication()
{
    revia::tests::ScopedTestDirectory directory;
    ComputerExperienceRecorder recorder(directory.root / "dataset");
    Check(recorder.Begin(Consent()), "The capture session did not open.");

    Check(recorder.Record(Row("notepad.exe")),
        "A record inside the consented application was refused.");
    Check(!recorder.Record(Row("keepass.exe")),
        "A demonstration in one application was treated as consent to record whatever "
        "the person switched to.");
    Check(recorder.Status().lastRefusal == CaptureRefusal::OutsideConsentedWindow,
        "Recording outside the consented window was refused for the wrong reason.");
    Check(recorder.Status().recorded == 1,
        "The wrong number of rows was written.");
}

// Enabling recording does not reach backwards.
void TestEnablingRecordingDoesNotCaptureWhatCameBefore()
{
    revia::tests::ScopedTestDirectory directory;
    ComputerExperienceRecorder recorder(directory.root / "dataset");

    // Work happening while recording is off.
    for (int index = 0; index < 5; ++index)
    {
        static_cast<void>(recorder.Record(Row()));
    }
    Check(recorder.Sessions().empty(), "Rows were kept while recording was off.");

    Check(recorder.Begin(Consent()), "The capture session did not open.");
    Check(recorder.Status().recorded == 0,
        "Opening a capture session flushed earlier activity into it, which is exactly "
        "what an explicit opt-in must not do.");
}

// Structural metadata and the words in a box are different things and need different
// consent.
void TestControlValuesNeedTheirOwnOptIn()
{
    revia::tests::ScopedTestDirectory directory;
    {
        ComputerExperienceRecorder recorder(directory.root / "structure");
        Check(recorder.Begin(Consent(ExperienceProvenance::HumanDemonstration,
                CaptureDepth::Structure)),
            "The structure-only session did not open.");
        ExperienceRecord record = Row();
        record.candidates.push_back(Candidate("Message", "dinner at eight"));
        Check(recorder.Record(record), "A structural record was refused.");

        const auto rows = recorder.Read(recorder.Status().sessionId);
        Check(rows.size() == 1 && rows[0].candidates.size() == 1,
            "The structural row did not round-trip.");
        Check(rows[0].candidates[0].value.empty(),
            "A control's text was written into a session that only consented to "
            "structure.");
        Check(rows[0].candidates[0].name == "Message" && rows[0].candidates[0].mayEdit,
            "The structural half of the row was lost along with the value.");
    }
    {
        ComputerExperienceRecorder recorder(directory.root / "values");
        Check(recorder.Begin(Consent(ExperienceProvenance::HumanDemonstration,
                CaptureDepth::ControlValues)),
            "The values session did not open.");
        ExperienceRecord record = Row();
        record.candidates.push_back(Candidate("Message", "dinner at eight"));
        Check(recorder.Record(record), "A values record was refused.");
        const auto rows = recorder.Read(recorder.Status().sessionId);
        Check(rows.size() == 1 && rows[0].candidates[0].value == "dinner at eight",
            "A consented value was not recorded.");
    }
}

// A credential is never written down, whatever the session consented to.
void TestACredentialIsNeverRecorded()
{
    revia::tests::ScopedTestDirectory directory;
    ComputerExperienceRecorder recorder(directory.root / "dataset");
    Check(recorder.Begin(Consent(ExperienceProvenance::HumanDemonstration,
            CaptureDepth::ControlValues)),
        "The capture session did not open.");

    ExperienceRecord record = Row();
    record.candidates.push_back(Candidate("Password", "hunter2"));
    record.candidates.push_back(Candidate("API key", "sk-abcdef"));
    record.candidates.push_back(Candidate("Message", "dinner at eight"));
    Check(recorder.Record(record), "The record was refused entirely.");

    const auto rows = recorder.Read(recorder.Status().sessionId);
    Check(rows.size() == 1 && rows[0].candidates.size() == 3,
        "Redacting a value dropped the candidate, which changes what the row means.");
    Check(rows[0].candidates[0].value.empty() && rows[0].candidates[0].redacted,
        "A password field's contents reached the dataset.");
    Check(rows[0].candidates[1].value.empty() && rows[0].candidates[1].redacted,
        "An API key reached the dataset.");
    Check(rows[0].candidates[2].value == "dinner at eight",
        "Redaction removed a value it had no reason to remove.");

    // And nothing resembling the secret is anywhere in the file, not merely absent from
    // the field it belonged in.
    const auto path = recorder.SessionPath(recorder.Status().sessionId);
    std::ifstream file(path);
    const std::string contents(
        (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Check(contents.find("hunter2") == std::string::npos &&
            contents.find("sk-abcdef") == std::string::npos,
        "A redacted secret is still present somewhere in the dataset file.");
}

// What makes a row usable as a positive label, and the four ways it is not.
void TestOnlyVerifiedTypedEvidenceQualifiesAsALabel()
{
    ExperienceRecord good = Row();
    good.provenance = ExperienceProvenance::HumanDemonstration;
    Check(good.QualifiesAsPositiveLabel(),
        "A verified, typed, executed demonstration was rejected as a label.");

    ExperienceRecord proposal = good;
    proposal.executed = false;
    Check(!proposal.QualifiesAsPositiveLabel(),
        "A proposal that never ran was admitted as a demonstration of what works.");

    ExperienceRecord uncertain = good;
    uncertain.outcome = revia::goals::VerificationOutcome::Unknown;
    Check(!uncertain.QualifiesAsPositiveLabel(),
        "An effect nobody could confirm was admitted as a verified positive, which is "
        "how a policy learns that an unconfirmable action is a good action.");

    ExperienceRecord weak = good;
    weak.checkedBy = revia::goals::PostconditionKind::TextObserved;
    Check(!weak.QualifiesAsPositiveLabel(),
        "A substring match was allowed to support a training label.");

    ExperienceRecord anonymous = good;
    anonymous.provenance = ExperienceProvenance::Unknown;
    Check(!anonymous.QualifiesAsPositiveLabel(),
        "A row nobody can say what it demonstrates was admitted as a label.");
}

// An API success code is not a verified label, and a teacher's proposal is not a
// demonstration of the right answer.
void TestProvenanceIsCarriedRatherThanAssumed()
{
    revia::tests::ScopedTestDirectory directory;
    ComputerExperienceRecorder recorder(directory.root / "dataset");
    Check(recorder.Begin(Consent(ExperienceProvenance::TeacherProposal)),
        "The teacher session did not open.");

    ExperienceRecord claimed = Row();
    // What a row would say about itself if it could.
    claimed.provenance = ExperienceProvenance::HumanDemonstration;
    Check(recorder.Record(claimed), "The record was refused.");

    const auto rows = recorder.Read(recorder.Status().sessionId);
    Check(rows.size() == 1 &&
            rows[0].provenance == ExperienceProvenance::TeacherProposal,
        "A row relabelled itself as a human demonstration; provenance is a fact about "
        "the session, not a claim a row may make.");
}

// Optional recording is bounded, and stops rather than growing.
void TestRecordingIsBounded()
{
    revia::tests::ScopedTestDirectory directory;
    ComputerExperienceRecorder recorder(directory.root / "dataset");
    CaptureConsent consent = Consent();
    consent.maximumRecords = 3;
    Check(recorder.Begin(consent), "The capture session did not open.");

    int written = 0;
    for (int index = 0; index < 10; ++index)
    {
        if (recorder.Record(Row())) ++written;
    }
    Check(written == 3,
        "An optional dataset grew past its own ceiling; " + std::to_string(written) +
            " rows were written.");
    Check(recorder.Status().lastRefusal == CaptureRefusal::QuotaReached,
        "Reaching the ceiling was not reported as reaching the ceiling.");

    // And the disk quota is its own bound.
    ComputerExperienceRecorder small(directory.root / "small");
    small.SetQuotaBytes(1);
    Check(small.Begin(Consent()), "The quota session did not open.");
    Check(small.Record(Row()), "The first row was refused before any space was used.");
    Check(!small.Record(Row()),
        "An optional dataset kept writing past its disk quota.");
}

// A closed session writes nothing, and closing twice is closing.
void TestEndingASessionStopsRecording()
{
    revia::tests::ScopedTestDirectory directory;
    ComputerExperienceRecorder recorder(directory.root / "dataset");
    Check(recorder.Begin(Consent()), "The capture session did not open.");
    Check(recorder.Record(Row()), "A record inside the session was refused.");
    recorder.End();
    recorder.End();
    Check(!recorder.Capturing() && !recorder.Record(Row()),
        "Recording continued after the capture session was closed.");
}

// Deleting rows is not unlearning them, and the difference is reported.
void TestDeletionNamesTheArtifactsItCannotUndo()
{
    revia::tests::ScopedTestDirectory directory;
    ComputerExperienceRecorder recorder(directory.root / "dataset");
    Check(recorder.Begin(Consent()), "The capture session did not open.");
    for (int index = 0; index < 4; ++index)
    {
        Check(recorder.Record(Row()), "A record was refused.");
    }
    const std::string session = recorder.Status().sessionId;
    recorder.End();

    const auto deletion = recorder.Forget(session);
    Check(deletion.deleted && deletion.recordsRemoved == 4,
        "The rows were not removed, or the deletion could not say how many it removed.");
    Check(!deletion.affectedArtifacts.empty(),
        "A deletion claimed to have removed data without naming what is now downstream "
        "of it, which is machine unlearning claimed on the strength of a file removal.");
    Check(recorder.Read(session).empty() && recorder.Sessions().empty(),
        "The session survived its own deletion.");

    const auto missing = recorder.Forget("capture-never-existed");
    Check(!missing.deleted && missing.affectedArtifacts.empty(),
        "Deleting a session that does not exist reported success.");
}

// A crash mid-append leaves a truncated last line. One unreadable row must not make the
// rows before it unreachable.
void TestATruncatedRowDoesNotLoseTheOnesBeforeIt()
{
    revia::tests::ScopedTestDirectory directory;
    ComputerExperienceRecorder recorder(directory.root / "dataset");
    Check(recorder.Begin(Consent()), "The capture session did not open.");
    Check(recorder.Record(Row()) && recorder.Record(Row()), "The rows were refused.");
    const std::string session = recorder.Status().sessionId;
    recorder.End();

    {
        std::ofstream file(recorder.SessionPath(session), std::ios::app);
        file << "{\"schema\": 1, \"record_id\": \"exp-trunc";
    }
    const auto rows = recorder.Read(session);
    Check(rows.size() == 2,
        "A truncated final row made the complete rows before it unreadable; " +
            std::to_string(rows.size()) + " came back.");
}

// The record is built from what the runtime already knows, with redaction on the way in.
void TestARecordIsBuiltFromTheSharedObservation()
{
    ComputerTaskContext context;
    context.iteration = 3;
    context.observation.screen.succeeded = true;
    context.observation.screen.foregroundApplication = "notepad.exe";
    context.observation.screen.omittedControls = 12;

    ObservedCandidate send;
    send.id = "send-1";
    send.name = "Send";
    send.role = "button";
    send.mayInvoke = true;
    context.observation.candidates.push_back(send);

    ObservedCandidate password;
    password.id = "pw-1";
    password.name = "Password";
    password.role = "edit";
    password.maySetText = true;
    context.observation.candidates.push_back(password);

    ComputerSubgoal subgoal;
    subgoal.id = "subgoal-9";
    subgoal.goalId = "goal-9";
    subgoal.intent = SubgoalIntent::InteractWithControl;

    ComputerDecisionRecord decision;
    decision.provider = "routine";
    decision.kind = ComputerDecisionKind::ProposeAction;

    const ExperienceRecord record =
        BuildExperienceRecord(context, subgoal, decision, CaptureDepth::Structure);
    Check(record.candidates.size() == 2 && record.omittedCandidates == 12,
        "The candidate mask did not carry what the decision chose from.");
    Check(!record.candidates[0].redacted && record.candidates[1].redacted,
        "A password field was not marked as withheld in the candidate mask.");
    Check(record.iteration == 3 && record.subgoalId == "subgoal-9" &&
            record.provider == "routine",
        "The record did not carry what joins it back to the run.");

    // The machine's temporary handles are never features.
    for (const auto& candidate : record.candidates)
    {
        Check(candidate.name != "send-1" && candidate.name != "pw-1",
            "An automation id reached the dataset, which is a policy learning this "
            "machine rather than the task.");
    }
}

} // namespace

void RunExperienceRecorderTests()
{
    TestNothingIsRecordedWithoutACaptureSession();
    TestACaptureSessionMustNameItsScopeAndItsMeaning();
    TestRecordingStaysInsideTheConsentedApplication();
    TestEnablingRecordingDoesNotCaptureWhatCameBefore();
    TestControlValuesNeedTheirOwnOptIn();
    TestACredentialIsNeverRecorded();
    TestOnlyVerifiedTypedEvidenceQualifiesAsALabel();
    TestProvenanceIsCarriedRatherThanAssumed();
    TestRecordingIsBounded();
    TestEndingASessionStopsRecording();
    TestDeletionNamesTheArtifactsItCannotUndo();
    TestATruncatedRowDoesNotLoseTheOnesBeforeIt();
    TestARecordIsBuiltFromTheSharedObservation();

    std::cout << "The experience recorder stays off, stays inside its consent, keeps "
                 "credentials out, and never calls deletion unlearning.\n";
}
