#pragma once

#include "Computer/computerController.h"
#include "Computer/computerSubgoal.h"
#include "Computer/computerTypes.h"
#include "Goals/goalTypes.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace revia::computer
{

// What a recorded decision is evidence *of*.
//
// The single most important field in a record, and the one a training pipeline is most
// tempted to lose. A model's proposal is not a demonstration of the right answer; an
// action that ran without error is not an action that achieved anything; and a step
// that only worked because a person fixed it afterwards is a negative example wearing a
// positive outcome. Conflating these is how a policy learns to repeat the mistakes that
// happened to get corrected.
enum class ExperienceProvenance
{
    Unknown,
    // A person drove this, inside an authorized capture session, on the task and window
    // the session was opened for. The only category that is evidence of what a person
    // would do.
    HumanDemonstration,
    // The existing model-driven path proposed it. Evidence of what the teacher does,
    // which is not the same as evidence of what is correct.
    TeacherProposal,
    // A cheaper policy proposed it while a teacher or a person was available to judge.
    StudentAction,
    // A proposal that replaced one that was wrong, with what it replaced recorded
    // alongside it.
    Correction
};

[[nodiscard]] std::string ToString(ExperienceProvenance value);
[[nodiscard]] ExperienceProvenance ExperienceProvenanceFromString(const std::string& value);

// How sensitive the content of a record is, and therefore what consent it needs.
//
// Structural metadata -- that there is a button named "Send" in a window belonging to
// an approved application -- is a different kind of thing from the words a person typed
// into the box beside it. They are separated here so the second can require stronger
// opt-in than the first, rather than one switch enabling both.
enum class CaptureDepth
{
    // Which controls existed, what they afford, which one was chosen. No values, no
    // text content, no payloads.
    Structure,
    // As above, plus observed text values of non-sensitive controls. Requires its own
    // opt-in.
    ControlValues
};

[[nodiscard]] std::string ToString(CaptureDepth value);

// What a capture session is allowed to record, and for how long.
//
// Everything here is off or narrow by default. A recorder that defaults to on is a
// keylogger with a configuration file.
struct CaptureConsent
{
    // The one application this session may record. Empty records nothing: a capture
    // session with no named window is a request to record the whole desktop, which is
    // not something this type will express.
    std::string application;
    // Narrowed further when the window matters -- one conversation rather than every
    // conversation.
    std::string windowTitle;
    CaptureDepth depth = CaptureDepth::Structure;
    // What the records in this session are evidence of. Set when the session is opened,
    // because whether a person is driving is a fact about the session and not something
    // an individual record should be able to claim for itself.
    ExperienceProvenance provenance = ExperienceProvenance::Unknown;
    // A hard stop. A capture session that never ends is a capture session nobody
    // remembers is running.
    std::uint64_t maximumDurationMs = 15ULL * 60ULL * 1000ULL;
    std::uint32_t maximumRecords = 500;

    [[nodiscard]] bool Usable() const
    {
        return !application.empty() && provenance != ExperienceProvenance::Unknown;
    }
};

// One decision, as evidence.
//
// Deliberately not the ComputerDecision and deliberately not the audit record. The
// audit log answers "what was done, by whose authority" and must never be traded away
// for a training convenience; this answers "what was seen, what was chosen, and did it
// work", which is a different question with a different retention policy and a
// different consent basis.
struct ExperienceRecord
{
    // Versioned separately, because the observation features and the action vocabulary
    // change at different times and a dataset has to be able to say which pair it was
    // written under.
    std::uint32_t schemaVersion = 0;
    std::uint32_t featureVersion = 0;
    std::uint32_t subgoalSchema = 0;

    std::string recordId;
    std::string sessionId;
    std::string goalId;
    std::string subgoalId;
    std::uint32_t iteration = 0;

    // Who decided, and what they were. A dataset that cannot say which policy produced
    // a row cannot be split by policy, and a policy trained on its own output without
    // knowing it is a policy training on its own output.
    std::string provider;
    std::string artifactId;
    ExperienceProvenance provenance = ExperienceProvenance::Unknown;
    RequestOrigin origin = RequestOrigin::Unknown;

    // The bounded situation, already redacted. Never the raw observation: redaction
    // happens on the way in, so an unredacted value is never written down and then
    // cleaned up afterwards.
    SubgoalIntent intent = SubgoalIntent::Unspecified;
    std::string application;
    // What was on offer, and what each could afford. This is the candidate mask: a
    // ranker needs to know not only what was chosen but what it was chosen from.
    struct Candidate
    {
        std::string name;
        std::string role;
        // What UI Automation infers labels this control, and the nearest named ancestor.
        //
        // Both were missing, and their absence is why the first trained ranker learned
        // position and nothing else: every admissible row was an unnamed field, so
        // `name` was empty on every candidate and the only column that varied across
        // them was where they sat in the list. These are the evidence a person actually
        // uses to tell one unlabelled box from the next.
        //
        // Neither is an identifier. An automation id is this machine's temporary handle
        // and never appears in a row; a container name is what the application calls a
        // panel, which is the same fact on any machine that runs it.
        std::string inferredLabel;
        std::string container;
        bool nameless = false;
        bool mayInvoke = false;
        bool mayEdit = false;
        // Whether this is the one the decision chose.
        bool chosen = false;
        // Withheld because the control is a password field or was otherwise refused by
        // redaction. Recorded as withheld rather than omitted, because a candidate
        // silently missing from the mask changes what the row means.
        bool redacted = false;
        // Only ever populated at ControlValues depth, and never for a redacted control.
        std::string value;
    };
    std::vector<Candidate> candidates;
    std::size_t omittedCandidates = 0;
    bool observationAvailable = false;
    bool observationWithheld = false;

    // What was decided, and what actually happened afterwards.
    ComputerDecisionKind decision = ComputerDecisionKind::CannotHandle;
    ComputerReasonCode reason = ComputerReasonCode::None;
    actions::ActionType action = actions::ActionType::Unknown;
    // The control the action aimed at, by name. Never an automation id or a runtime id:
    // those are this machine's temporary handles, and a policy that learned one learned
    // the machine rather than the task.
    //
    // This is the *answer*. Everything about the chosen candidate belongs on this side
    // of the line and nothing on this side may reach a feature -- see below.
    std::string target;

    // What the subgoal asked for: the question, recorded separately from the answer.
    //
    // This was the defect that explains the first trained ranker. The dataset builder
    // had no descriptor to read, so it reconstructed one from the chosen candidate --
    // `target_name` was the chosen control's name and `target_role` was the chosen
    // control's role. Every name and role feature was therefore 1 for the chosen
    // candidate by construction, on every named row: the label was inside the features.
    // On the unnamed rows there was no name to leak, so the only column left that
    // varied was position, and position is what the model learned.
    //
    // A ranker is only meaningful when the question and the answer are separate objects.
    // These three are the question. `target` and `candidates[i].chosen` are the answer.
    std::string requestedName;
    std::string requestedRole;
    std::string requestedContainer;
    // Whether a payload was placed, never which payload. The user's words are not
    // training data.
    bool carriedPayload = false;

    bool executed = false;
    goals::VerificationOutcome outcome = goals::VerificationOutcome::Unknown;
    goals::PostconditionKind checkedBy = goals::PostconditionKind::TextObserved;
    // Structured, so failures can be counted. Free text would be a second unreviewed
    // channel and would end up as a label.
    std::string failureCategory;

    // What this replaced, when it is a correction.
    std::string correctedRecordId;

    // What this row was collected under, when a harness said.
    //
    // Protocol metadata, not a label and not a feature: it says which task and which
    // arrangement of the window produced the row, so a split can hold out whole
    // variants instead of whole sessions. Holding out sessions was not enough -- every
    // session ran every task, so the same task shape appeared on both sides of the
    // split and the held-out score was measuring recall of a task it had already seen.
    std::string taskVariant;
    std::string layoutVariant;

    std::uint64_t decisionMicroseconds = 0;
    std::uint64_t executionMicroseconds = 0;
    std::uint64_t recordedAtMs = 0;

    // Whether the row is admissible as a positive training label.
    //
    // Computed rather than asserted. An unverified effect is the case this exists for:
    // it means "it may have worked", and admitting it as a verified positive is how a
    // policy learns that an action nobody could confirm is a good action.
    [[nodiscard]] bool QualifiesAsPositiveLabel() const;
};

// Versioning for the record above. Bumped when the meaning of a field changes, not when
// a field is added at the end.
inline constexpr std::uint32_t CurrentExperienceSchema = 1;
// 2 -- adds container, inferred label and namelessness to the candidate, which is what
//      a decision about an unlabelled field is actually made on. See
//      Tools/Computer/features.py for the measurement that forced it.
inline constexpr std::uint32_t CurrentFeatureVersion = 2;

// Why a record was not written. Counted rather than logged, so that "recording is on
// and nothing is appearing" has an answer.
enum class CaptureRefusal
{
    None,
    NotCapturing,
    OutsideConsentedWindow,
    QuotaReached,
    SessionExpired,
    QueueFull,
    StorageFailed,
    SensitiveContent
};

[[nodiscard]] std::string ToString(CaptureRefusal value);

struct CaptureStatus
{
    bool capturing = false;
    std::string sessionId;
    std::string application;
    CaptureDepth depth = CaptureDepth::Structure;
    ExperienceProvenance provenance = ExperienceProvenance::Unknown;
    std::uint32_t recorded = 0;
    std::uint32_t refused = 0;
    std::uint64_t startedAtMs = 0;
    std::uint64_t expiresAtMs = 0;
    CaptureRefusal lastRefusal = CaptureRefusal::None;
    std::uintmax_t bytesWritten = 0;
};

// The opt-in record of what was decided and whether it worked.
//
// Separate from memory, from identity storage and from the action audit, and separate
// on purpose. The audit log is mandatory and must never be dropped; this is optional
// and must be dropped the moment keeping it would delay anything that matters. Memory
// is Revia's; this is a dataset. Running them through one mechanism would mean one
// retention policy, one consent basis and one deletion story for three things that
// need three.
//
// It is off. Not off-by-default-in-the-config: off, with no capture session open, and
// nothing is written until one is explicitly opened for a named application. Enabling
// it does not reach backwards -- there is no buffer of earlier activity to flush,
// because nothing was kept.
class ComputerExperienceRecorder
{
public:
    explicit ComputerExperienceRecorder(std::filesystem::path datasetRoot);

    ComputerExperienceRecorder(const ComputerExperienceRecorder&) = delete;
    ComputerExperienceRecorder& operator=(const ComputerExperienceRecorder&) = delete;

    // Open a capture session. Refused unless the consent names an application and says
    // what the records will be evidence of.
    [[nodiscard]] bool Begin(CaptureConsent consent);
    // Close it. Idempotent, because a stop that arrives twice is a stop.
    void End();

    [[nodiscard]] bool Capturing() const;
    [[nodiscard]] CaptureStatus Status() const;

    // What the harness driving this collection says the current rows are about.
    //
    // Protocol metadata and nothing else: which task shape and which arrangement of the
    // window is being exercised right now. It exists so a split can hold out complete
    // variants rather than complete sessions, which is the difference between measuring
    // generalisation and measuring recall.
    //
    // Only a collection harness sets this. Ordinary use leaves it empty, and an empty
    // variant is simply a row no variant-based split can place -- which those splits
    // then say, rather than guessing.
    void SetProtocol(std::string taskVariant, std::string layoutVariant);

    // Offer one decision for recording.
    //
    // Returns whether it was written. A false is ordinary and not an error: outside a
    // capture session, outside the consented window, or past a quota, the right
    // behaviour is to drop the row. Optional recording must never be the reason
    // something else waits.
    [[nodiscard]] bool Record(ExperienceRecord record);

    // Everything this session has written, for the export tooling. Reads from disk so
    // that a crash between the write and the read does not produce a dataset that
    // exists only in memory.
    [[nodiscard]] std::vector<ExperienceRecord> Read(const std::string& sessionId) const;

    // Forget a session's records.
    //
    // Returns the sessions whose exports and trained artifacts are now downstream of
    // deleted data. Deleting a row does not unlearn it: an artifact trained on it has
    // already absorbed whatever it taught, and the honest response is to mark that
    // artifact for retirement rather than to claim the data is gone from it.
    struct Deletion
    {
        bool deleted = false;
        std::uint32_t recordsRemoved = 0;
        // Artifact lineage that must now be retired or retrained. Never empty just
        // because the files were removed.
        std::vector<std::string> affectedArtifacts;
    };
    [[nodiscard]] Deletion Forget(const std::string& sessionId);

    [[nodiscard]] std::vector<std::string> Sessions() const;

    // Where this session's rows live. One file per session, so deletion is a file
    // removal rather than a rewrite of a shared log -- a rewrite that could fail
    // part-way and leave a dataset nobody can account for.
    [[nodiscard]] std::filesystem::path SessionPath(const std::string& sessionId) const;

    // A ceiling on how much disk an optional dataset may take. Reaching it stops
    // recording rather than growing.
    void SetQuotaBytes(std::uintmax_t bytes) { quotaBytes = bytes; }

    // Where rows are written. Refused while a capture session is open: moving the
    // destination mid-session would split one session's rows across two places and
    // leave a deletion unable to find half of them.
    [[nodiscard]] bool SetRoot(std::filesystem::path datasetRoot);
    [[nodiscard]] std::filesystem::path Root() const;

private:
    [[nodiscard]] bool WithinConsent(const ExperienceRecord& record) const;

    std::filesystem::path root;
    std::uintmax_t quotaBytes = 64ULL * 1024ULL * 1024ULL;

    mutable std::mutex mutex;
    // Collection protocol, set by a harness and stamped onto every row it then writes.
    std::string protocolTask;
    std::string protocolLayout;
    bool capturing = false;
    CaptureConsent consent;
    std::string sessionId;
    std::uint64_t startedAtMs = 0;
    std::uint32_t recorded = 0;
    std::uint32_t refused = 0;
    std::uintmax_t bytesWritten = 0;
    CaptureRefusal lastRefusal = CaptureRefusal::None;
};

// Build a record from what the controller and the runner already know.
//
// Redaction happens here, on the way in. A password field never becomes a value that is
// written and then cleaned up: it arrives at the record as withheld, and the only thing
// stored is that there was a control there and it was refused.
[[nodiscard]] ExperienceRecord BuildExperienceRecord(
    const ComputerTaskContext& context,
    const ComputerSubgoal& subgoal,
    const ComputerDecisionRecord& decision,
    CaptureDepth depth);

} // namespace revia::computer
