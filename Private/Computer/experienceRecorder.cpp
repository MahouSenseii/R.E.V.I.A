#include "Computer/experienceRecorder.h"

#include "Memory/sensitiveContent.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>
#include <utility>

namespace revia::computer
{

namespace
{

std::uint64_t NowMs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string Lowered(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) return true;
    return Lowered(haystack).find(Lowered(needle)) != std::string::npos;
}

std::string NewSessionId()
{
    std::ostringstream stream;
    stream << "capture-" << NowMs();
    return stream.str();
}

std::string NewRecordId()
{
    static std::atomic<std::uint64_t> counter{1};
    std::ostringstream stream;
    stream << "exp-" << NowMs() << '-'
           << counter.fetch_add(1, std::memory_order_relaxed);
    return stream.str();
}

// Whether a control's value may be written down at all.
//
// Reuses the existing consequence classifier rather than introducing a second list of
// sensitive names. Two lists would eventually disagree, and the one that disagreed by
// being shorter would be the one that wrote a credential into a dataset.
bool ValueIsRecordable(const std::string& controlName)
{
    return actions::ClassifyControlConsequence(controlName) !=
        actions::ConsequenceClass::AccountOrSecurity;
}

nlohmann::json ToJson(const ExperienceRecord& record)
{
    nlohmann::json candidates = nlohmann::json::array();
    for (const ExperienceRecord::Candidate& candidate : record.candidates)
    {
        nlohmann::json entry = {
            {"name", candidate.name},
            {"role", candidate.role},
            {"inferred_label", candidate.inferredLabel},
            {"container", candidate.container},
            {"nameless", candidate.nameless},
            {"may_invoke", candidate.mayInvoke},
            {"may_edit", candidate.mayEdit},
            {"chosen", candidate.chosen},
            {"redacted", candidate.redacted}};
        if (!candidate.value.empty()) entry["value"] = candidate.value;
        candidates.push_back(std::move(entry));
    }

    return {
        {"schema", record.schemaVersion},
        {"feature_version", record.featureVersion},
        {"subgoal_schema", record.subgoalSchema},
        {"record_id", record.recordId},
        {"session_id", record.sessionId},
        {"goal_id", record.goalId},
        {"subgoal_id", record.subgoalId},
        {"iteration", record.iteration},
        {"provider", record.provider},
        {"artifact_id", record.artifactId},
        {"provenance", ToString(record.provenance)},
        {"origin", ToString(record.origin)},
        {"intent", ToString(record.intent)},
        {"application", record.application},
        {"candidates", std::move(candidates)},
        {"omitted_candidates", record.omittedCandidates},
        {"observation_available", record.observationAvailable},
        {"observation_withheld", record.observationWithheld},
        {"decision", ToString(record.decision)},
        {"reason", ToString(record.reason)},
        {"action", actions::ToString(record.action)},
        {"target", record.target},
        {"requested_name", record.requestedName},
        {"requested_role", record.requestedRole},
        {"requested_container", record.requestedContainer},
        {"task_variant", record.taskVariant},
        {"layout_variant", record.layoutVariant},
        {"carried_payload", record.carriedPayload},
        {"executed", record.executed},
        {"outcome", goals::ToString(record.outcome)},
        {"checked_by", goals::ToString(record.checkedBy)},
        {"failure_category", record.failureCategory},
        {"corrected_record_id", record.correctedRecordId},
        {"decision_us", record.decisionMicroseconds},
        {"execution_us", record.executionMicroseconds},
        {"recorded_at_ms", record.recordedAtMs},
        // Written into the row rather than recomputed by every reader. A downstream
        // tool that decides admissibility for itself is a downstream tool that can
        // decide it differently.
        {"positive_label", record.QualifiesAsPositiveLabel()}};
}

ExperienceRecord FromJson(const nlohmann::json& entry)
{
    ExperienceRecord record;
    record.schemaVersion = entry.value("schema", 0u);
    record.featureVersion = entry.value("feature_version", 0u);
    record.subgoalSchema = entry.value("subgoal_schema", 0u);
    record.recordId = entry.value("record_id", std::string{});
    record.sessionId = entry.value("session_id", std::string{});
    record.goalId = entry.value("goal_id", std::string{});
    record.subgoalId = entry.value("subgoal_id", std::string{});
    record.iteration = entry.value("iteration", 0u);
    record.provider = entry.value("provider", std::string{});
    record.artifactId = entry.value("artifact_id", std::string{});
    record.provenance = ExperienceProvenanceFromString(
        entry.value("provenance", std::string{}));
    record.origin = entry.value("origin", std::string{}) == "user_directed"
        ? RequestOrigin::UserDirected
        : (entry.value("origin", std::string{}) == "autonomous"
            ? RequestOrigin::Autonomous : RequestOrigin::Unknown);
    record.intent = SubgoalIntentFromString(entry.value("intent", std::string{}));
    record.application = entry.value("application", std::string{});
    record.omittedCandidates = entry.value("omitted_candidates", std::size_t{0});
    record.observationAvailable = entry.value("observation_available", false);
    record.observationWithheld = entry.value("observation_withheld", false);
    record.action = actions::ActionTypeFromString(entry.value("action", std::string{}));
    record.target = entry.value("target", std::string{});
    record.requestedName = entry.value("requested_name", std::string{});
    record.requestedRole = entry.value("requested_role", std::string{});
    record.requestedContainer = entry.value("requested_container", std::string{});
    record.taskVariant = entry.value("task_variant", std::string{});
    record.layoutVariant = entry.value("layout_variant", std::string{});
    record.carriedPayload = entry.value("carried_payload", false);
    record.executed = entry.value("executed", false);
    record.outcome = goals::VerificationOutcomeFromString(
        entry.value("outcome", std::string{}));
    record.checkedBy = goals::PostconditionKindFromString(
        entry.value("checked_by", std::string{}));
    record.failureCategory = entry.value("failure_category", std::string{});
    record.correctedRecordId = entry.value("corrected_record_id", std::string{});
    record.decisionMicroseconds = entry.value("decision_us", std::uint64_t{0});
    record.executionMicroseconds = entry.value("execution_us", std::uint64_t{0});
    record.recordedAtMs = entry.value("recorded_at_ms", std::uint64_t{0});

    if (entry.contains("candidates") && entry["candidates"].is_array())
    {
        for (const auto& item : entry["candidates"])
        {
            ExperienceRecord::Candidate candidate;
            candidate.name = item.value("name", std::string{});
            candidate.role = item.value("role", std::string{});
            candidate.inferredLabel = item.value("inferred_label", std::string{});
            candidate.container = item.value("container", std::string{});
            candidate.nameless = item.value("nameless", candidate.name.empty());
            candidate.mayInvoke = item.value("may_invoke", false);
            candidate.mayEdit = item.value("may_edit", false);
            candidate.chosen = item.value("chosen", false);
            candidate.redacted = item.value("redacted", false);
            candidate.value = item.value("value", std::string{});
            record.candidates.push_back(std::move(candidate));
        }
    }
    return record;
}

} // namespace

std::string ToString(const ExperienceProvenance value)
{
    switch (value)
    {
        case ExperienceProvenance::HumanDemonstration: return "human_demonstration";
        case ExperienceProvenance::TeacherProposal: return "teacher_proposal";
        case ExperienceProvenance::StudentAction: return "student_action";
        case ExperienceProvenance::Correction: return "correction";
        case ExperienceProvenance::Unknown: break;
    }
    return "unknown";
}

ExperienceProvenance ExperienceProvenanceFromString(const std::string& value)
{
    if (value == "human_demonstration") return ExperienceProvenance::HumanDemonstration;
    if (value == "teacher_proposal") return ExperienceProvenance::TeacherProposal;
    if (value == "student_action") return ExperienceProvenance::StudentAction;
    if (value == "correction") return ExperienceProvenance::Correction;
    // Unrecognised provenance is Unknown, which is inadmissible as a label. A row whose
    // provenance cannot be read is a row nobody can say what it is evidence of.
    return ExperienceProvenance::Unknown;
}

std::string ToString(const CaptureDepth value)
{
    return value == CaptureDepth::ControlValues ? "control_values" : "structure";
}

std::string ToString(const CaptureRefusal value)
{
    switch (value)
    {
        case CaptureRefusal::NotCapturing: return "not_capturing";
        case CaptureRefusal::OutsideConsentedWindow: return "outside_consented_window";
        case CaptureRefusal::QuotaReached: return "quota_reached";
        case CaptureRefusal::SessionExpired: return "session_expired";
        case CaptureRefusal::QueueFull: return "queue_full";
        case CaptureRefusal::StorageFailed: return "storage_failed";
        case CaptureRefusal::SensitiveContent: return "sensitive_content";
        case CaptureRefusal::None: break;
    }
    return "none";
}

bool ExperienceRecord::QualifiesAsPositiveLabel() const
{
    // Four separate things have to be true, and each of them is a way datasets go
    // wrong when it is not checked.
    //
    // The row has to be evidence of something -- Unknown provenance is a row nobody can
    // say what it demonstrates. It has to have actually run, because a proposal is not
    // a demonstration. Its effect has to have been *verified*, not merely un-refuted:
    // UnverifiedEffect means "it may have worked", and admitting that as a positive is
    // how a policy learns that an action nobody could confirm is a good action. And the
    // verification has to have been a typed one, because the legacy substring rule
    // cannot distinguish "no" from "I could not tell" and so cannot support a label.
    if (provenance == ExperienceProvenance::Unknown) return false;
    if (!executed) return false;
    if (outcome != goals::VerificationOutcome::Verified) return false;
    if (checkedBy == goals::PostconditionKind::TextObserved) return false;
    return true;
}

ComputerExperienceRecorder::ComputerExperienceRecorder(std::filesystem::path datasetRoot)
    : root(std::move(datasetRoot))
{
}

bool ComputerExperienceRecorder::SetRoot(std::filesystem::path datasetRoot)
{
    std::lock_guard lock(mutex);
    if (capturing) return false;
    root = std::move(datasetRoot);
    return true;
}

std::filesystem::path ComputerExperienceRecorder::Root() const
{
    std::lock_guard lock(mutex);
    return root;
}

bool ComputerExperienceRecorder::Begin(CaptureConsent newConsent)
{
    if (!newConsent.Usable())
    {
        // A capture session with no named application is a request to record the whole
        // desktop, and a capture session that cannot say what its rows are evidence of
        // produces rows no pipeline may use. Neither is expressible here.
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) return false;

    std::lock_guard lock(mutex);
    capturing = true;
    consent = std::move(newConsent);
    sessionId = NewSessionId();
    startedAtMs = NowMs();
    recorded = 0;
    refused = 0;
    bytesWritten = 0;
    lastRefusal = CaptureRefusal::None;
    // Nothing is flushed here. There is no buffer of earlier activity to turn into
    // records, because nothing was kept -- enabling recording does not reach backwards.
    return true;
}

void ComputerExperienceRecorder::End()
{
    std::lock_guard lock(mutex);
    capturing = false;
    consent = CaptureConsent{};
}

bool ComputerExperienceRecorder::Capturing() const
{
    std::lock_guard lock(mutex);
    return capturing;
}

void ComputerExperienceRecorder::SetProtocol(
    std::string taskVariant, std::string layoutVariant)
{
    std::lock_guard lock(mutex);
    protocolTask = std::move(taskVariant);
    protocolLayout = std::move(layoutVariant);
}

CaptureStatus ComputerExperienceRecorder::Status() const
{
    std::lock_guard lock(mutex);
    CaptureStatus status;
    status.capturing = capturing;
    status.sessionId = sessionId;
    status.application = consent.application;
    status.depth = consent.depth;
    status.provenance = consent.provenance;
    status.recorded = recorded;
    status.refused = refused;
    status.startedAtMs = startedAtMs;
    status.expiresAtMs = startedAtMs + consent.maximumDurationMs;
    status.lastRefusal = lastRefusal;
    status.bytesWritten = bytesWritten;
    return status;
}

bool ComputerExperienceRecorder::WithinConsent(const ExperienceRecord& record) const
{
    if (!Contains(record.application, consent.application) ||
        Lowered(record.application) != Lowered(consent.application))
    {
        return false;
    }
    return true;
}

std::filesystem::path ComputerExperienceRecorder::SessionPath(
    const std::string& session) const
{
    return root / (session + ".jsonl");
}

bool ComputerExperienceRecorder::Record(ExperienceRecord record)
{
    // Stamped here rather than by the caller, so that a row cannot claim a protocol
    // the recorder was not told about.
    {
        std::lock_guard lock(mutex);
        record.taskVariant = protocolTask;
        record.layoutVariant = protocolLayout;
    }
    std::lock_guard lock(mutex);
    if (!capturing)
    {
        lastRefusal = CaptureRefusal::NotCapturing;
        return false;
    }
    if (NowMs() - startedAtMs > consent.maximumDurationMs)
    {
        // A capture session that never ends is a capture session nobody remembers is
        // running. It closes itself rather than asking to be closed.
        capturing = false;
        lastRefusal = CaptureRefusal::SessionExpired;
        ++refused;
        return false;
    }
    if (recorded >= consent.maximumRecords)
    {
        lastRefusal = CaptureRefusal::QuotaReached;
        ++refused;
        return false;
    }
    if (bytesWritten >= quotaBytes)
    {
        lastRefusal = CaptureRefusal::QuotaReached;
        ++refused;
        return false;
    }
    if (!WithinConsent(record))
    {
        // The window the session was opened for, and nothing else. A demonstration in
        // one application is not consent to record whatever the person switches to.
        lastRefusal = CaptureRefusal::OutsideConsentedWindow;
        ++refused;
        return false;
    }

    record.sessionId = sessionId;
    record.recordId = NewRecordId();
    record.recordedAtMs = NowMs();
    record.schemaVersion = CurrentExperienceSchema;
    record.featureVersion = CurrentFeatureVersion;
    record.provenance = consent.provenance;

    // Depth is enforced here and not only where the record was built. A row that
    // arrived carrying values into a structure-only session has them removed rather
    // than being refused: the structural part is still legitimate evidence, and
    // dropping it would lose a good row over a field that should not have been filled.
    if (consent.depth == CaptureDepth::Structure)
    {
        for (ExperienceRecord::Candidate& candidate : record.candidates)
        {
            candidate.value.clear();
        }
    }
    else
    {
        for (ExperienceRecord::Candidate& candidate : record.candidates)
        {
            if (candidate.value.empty()) continue;
            if (!ValueIsRecordable(candidate.name) ||
                memory::ContainsSensitiveContent(candidate.value))
            {
                // Treated as imperfect, which is why the whole value goes rather than
                // some redacted form of it. A partially redacted secret is a secret.
                candidate.value.clear();
                candidate.redacted = true;
            }
        }
    }

    const std::string line = ToJson(record).dump();
    std::ofstream file(SessionPath(sessionId), std::ios::app);
    if (!file)
    {
        lastRefusal = CaptureRefusal::StorageFailed;
        ++refused;
        return false;
    }
    file << line << '\n';
    if (!file.good())
    {
        lastRefusal = CaptureRefusal::StorageFailed;
        ++refused;
        return false;
    }

    ++recorded;
    bytesWritten += line.size() + 1;
    lastRefusal = CaptureRefusal::None;
    return true;
}

std::vector<ExperienceRecord> ComputerExperienceRecorder::Read(
    const std::string& session) const
{
    std::vector<ExperienceRecord> records;
    std::ifstream file(SessionPath(session));
    if (!file) return records;

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty()) continue;
        // A truncated final line is what a crash mid-append leaves behind. It is
        // skipped rather than allowed to abort the read: one unreadable row must not
        // make the rows before it unreachable.
        const nlohmann::json entry = nlohmann::json::parse(line, nullptr, false);
        if (entry.is_discarded() || !entry.is_object()) continue;
        records.push_back(FromJson(entry));
    }
    return records;
}

std::vector<std::string> ComputerExperienceRecorder::Sessions() const
{
    std::vector<std::string> sessions;
    std::error_code error;
    if (!std::filesystem::exists(root, error)) return sessions;
    for (const auto& entry : std::filesystem::directory_iterator(root, error))
    {
        if (error) break;
        if (!entry.is_regular_file()) continue;
        const std::filesystem::path& path = entry.path();
        if (path.extension() != ".jsonl") continue;
        sessions.push_back(path.stem().string());
    }
    std::sort(sessions.begin(), sessions.end());
    return sessions;
}

ComputerExperienceRecorder::Deletion ComputerExperienceRecorder::Forget(
    const std::string& session)
{
    Deletion deletion;
    std::error_code error;
    const std::filesystem::path path = SessionPath(session);
    if (!std::filesystem::exists(path, error))
    {
        return deletion;
    }

    // Counted before the file goes, because afterwards there is nothing to count and a
    // deletion that cannot say how much it removed is a deletion nobody can audit.
    deletion.recordsRemoved = static_cast<std::uint32_t>(Read(session).size());
    deletion.deleted = std::filesystem::remove(path, error) && !error;

    if (deletion.deleted)
    {
        // Lineage, not reassurance. Removing the rows does not remove what a model
        // trained on them already absorbed, and saying otherwise would be claiming
        // machine unlearning on the strength of a file deletion. What is true is that
        // any artifact downstream of this session now needs retiring or retraining, and
        // naming the session is how that is traced.
        deletion.affectedArtifacts.push_back(session);
    }
    return deletion;
}

ExperienceRecord BuildExperienceRecord(
    const ComputerTaskContext& context,
    const ComputerSubgoal& subgoal,
    const ComputerDecisionRecord& decision,
    const CaptureDepth depth)
{
    ExperienceRecord record;
    record.schemaVersion = CurrentExperienceSchema;
    record.featureVersion = CurrentFeatureVersion;
    record.subgoalSchema = subgoal.schemaVersion;
    record.goalId = subgoal.goalId;
    record.subgoalId = subgoal.id;
    record.iteration = context.iteration;
    record.provider = decision.provider;
    record.origin = subgoal.origin;
    record.intent = subgoal.intent;
    // The question. Taken from the subgoal's own descriptor and never from the chosen
    // candidate, which is the answer.
    record.requestedName = subgoal.target.name;
    record.requestedRole = subgoal.target.role;
    record.requestedContainer = subgoal.target.container;
    record.application = context.observation.screen.foregroundApplication;
    record.omittedCandidates = context.observation.screen.omittedControls;
    record.observationAvailable = context.observation.Available();
    record.observationWithheld = context.observation.withheld;
    record.decision = decision.kind;
    record.reason = decision.code;

    // The candidate mask. A ranker needs to know not only what was chosen but what it
    // was chosen from, so every candidate is carried -- including the ones redaction
    // withheld, marked as withheld rather than dropped. A candidate silently missing
    // from the mask changes what the row means.
    for (const ObservedCandidate& candidate : context.observation.candidates)
    {
        ExperienceRecord::Candidate entry;
        entry.name = candidate.name;
        entry.role = candidate.role;
        // The two fields that make an unlabelled control distinguishable from the one
        // beside it, kept apart from the name because they are inferences and it is not.
        entry.inferredLabel = candidate.inferredLabel;
        entry.container = candidate.container;
        entry.nameless = candidate.nameless;
        entry.mayInvoke = candidate.mayInvoke;
        entry.mayEdit = candidate.maySetText || candidate.mayType;
        entry.redacted = !ValueIsRecordable(candidate.name);
        // Deliberately never the automation id, which is this machine's temporary
        // handle for this window today. A policy that learned one learned the machine.
        record.candidates.push_back(std::move(entry));
    }
    static_cast<void>(depth);
    return record;
}

} // namespace revia::computer
