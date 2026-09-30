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

// Distinguishes proposals, executed actions, verified success, and human corrections.
// Execution alone or later correction must not become a positive training label.
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

// Structural metadata and content have separate consent requirements.
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

// Training evidence has separate consent and retention from the mandatory action audit.
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
        // Published label and named container are portable evidence, not automation IDs.
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
    // Chosen control name is the answer, never an input feature or machine-specific ID.
    std::string target;

    // Requested descriptor is the question; target and chosen flags are the answer.
    // Keep them separate to prevent label leakage into features.
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

    // Protocol metadata for holding out complete task and layout variants;
    // never an input feature or training label.
    std::string taskVariant;
    std::string layoutVariant;

    std::uint64_t decisionMicroseconds = 0;
    std::uint64_t executionMicroseconds = 0;
    std::uint64_t recordedAtMs = 0;

    // Computed from verified evidence; an unconfirmed effect is not a positive label.
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

// Optional recording starts only with explicit consent for a named application;
// no earlier activity is buffered. It must never delay required work or replace audit.
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

    // Collection harness supplies task/layout variants for dataset splits.
    // Ordinary use leaves them empty; splits must report unplaceable rows.
    void SetProtocol(std::string taskVariant, std::string layoutVariant);

    // Returns false outside consent, outside the approved window, or beyond quotas.
    // Optional recording must never delay execution.
    [[nodiscard]] bool Record(ExperienceRecord record);

    // Everything this session has written, for the export tooling. Reads from disk so
    // that a crash between the write and the read does not produce a dataset that
    // exists only in memory.
    [[nodiscard]] std::vector<ExperienceRecord> Read(const std::string& sessionId) const;

    // Reports downstream exports and artifacts affected by deletion.
    // Deleting records does not unlearn them; affected artifacts require retirement.
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

// Redacts before recording: password controls are withheld, never written then cleaned.
[[nodiscard]] ExperienceRecord BuildExperienceRecord(const ComputerTaskContext& context,
    const ComputerSubgoal& subgoal, const ComputerDecisionRecord& decision, CaptureDepth depth);

} // namespace revia::computer
