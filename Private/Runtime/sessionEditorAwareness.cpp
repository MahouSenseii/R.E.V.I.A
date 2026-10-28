#include "Runtime/reviaSession.h"

#include "Audit/contentDigest.h"
#include "Core/utf8.h"

#include <algorithm>
#include <fstream>
#include <utility>

namespace revia::runtime
{
presence::EditorObservationResult ReviaSession::ObserveEditorFile(const presence::EditorObservationRequest& request)
{
    presence::EditorObservationResult result;
    const auto refuse = [&](const char* reason) { result.reason = reason; return result; };
    bool enabled = false;
    {
        std::lock_guard lock(preferenceSnapshotMutex);
        enabled = settings.localApi.bEnabled && settings.localApi.bTrustedPrivate && settings.perception.bEnabled;
    }
    if (!started.load() || !enabled || windowEventMonitor.IsPaused() || Audience().kind != identity::AudienceKind::Private)
        return refuse("Saved-file awareness requires a running trusted private API and unpaused app awareness.");
    const auto incomingFile = std::filesystem::u8path(request.path).lexically_normal();
    const auto incomingProject = std::filesystem::u8path(request.projectRoot).lexically_normal();
    const auto receivedInputGeneration = userInteractionGeneration.load();
    {
        std::lock_guard lock(editorObservationMutex);
        if (editorObservation && (editorObservation->source.revision != request.revision ||
            editorObservation->source.path.lexically_normal() != incomingFile ||
            editorObservation->source.projectRoot.lexically_normal() != incomingProject))
            if (auto attempt = editorOpeningStop.load()) attempt->request_stop();
    }

    // A direct turn never waits for observation. The sender may retry a busy receipt;
    // no model work or unsolicited reply runs on this HTTP handler.
    std::unique_lock operationLock(operationMutex, std::try_to_lock);
    if (!operationLock.owns_lock() || busy.load())
    {
        result.retryable = true;
        return refuse("Revia is handling direct interaction; retry the latest saved revision after it settles.");
    }
    const auto input = CaptureInputContext(agents::InputSource::Typed);
    if (input.audience.kind != identity::AudienceKind::Private || !InputContextCurrent(input))
        return refuse("The private observation context changed.");

    actions::ActionRequest read;
    read.id = actions::NewActionId();
    read.type = actions::ActionType::ReadTextFile;
    read.source = incomingFile;
    read.requestedBy = "editor_observation";
    const auto decision = actionRuntime.Evaluate(read);
    if (decision.verdict != actions::PolicyVerdict::Allowed)
        return refuse("The saved file is not inside an automatically readable approved root.");
    actions::ActionRequest project;
    project.id = actions::NewActionId();
    project.type = actions::ActionType::ListDirectory;
    project.source = incomingProject;
    project.requestedBy = "editor_observation";
    const auto projectDecision = actionRuntime.Evaluate(project);
    if (projectDecision.verdict != actions::PolicyVerdict::Allowed)
        return refuse("The declared project is not an approved readable directory.");

    std::error_code error;
    const auto bytes = std::filesystem::file_size(decision.canonicalSource, error);
    if (error || bytes == 0 || bytes > 16384)
        return refuse("Stage A only observes complete C/C++ source files up to 16 KiB.");
    read.taskContract = BuildActionTaskContract(read, input.stamp, {});
    if (!read.taskContract)
        return refuse("The observation read could not retain its task admission.");
    const auto outcome = actionRuntime.ExecuteFor(input.stamp, read);
    if (!outcome.Succeeded())
        return refuse("The authorized source read did not succeed; no observation was queued.");

    auto record = std::make_shared<EditorObservationRecord>();
    record->source = {projectDecision.canonicalSource, decision.canonicalSource, request.revision, outcome.result.content,
        std::chrono::system_clock::time_point{std::chrono::milliseconds{request.occurredAtUnixMs}}};
    record->input = input;
    record->inputGeneration = receivedInputGeneration;
    if (userInteractionGeneration.load() != receivedInputGeneration)
        return refuse("Direct input superseded the saved-file observation.");
    if (!perception::ValidateSavedFileObservation(record->source, std::chrono::system_clock::now(), result.reason))
        return result;

    const std::string identity = actions::PathToUtf8(record->source.path) + ':' + record->source.revision;
    bool seen = false;
    {
        std::lock_guard lock(editorObservationMutex);
        if (editorObservation && editorObservation->source.path == record->source.path &&
            editorObservation->source.revision == record->source.revision && EditorObservationCurrent(*editorObservation))
        {
            result.accepted = true;
            result.eventId = "saved-" + std::to_string(editorObservation->epoch);
            result.reason = "The unchanged revision is already known; no new model call was requested.";
            return result;
        }
        seen = std::find(editorSeenRevisions.begin(), editorSeenRevisions.end(), identity) != editorSeenRevisions.end();
        if (!seen)
        {
            editorSeenRevisions.push_back(identity);
            if (editorSeenRevisions.size() > 32) editorSeenRevisions.pop_front();
        }
        record->epoch = ++editorObservationEpoch;
        editorObservation = record;
        // Publication and retirement share this lock: an expired snapshot cannot
        // clear a replacement cue, nor can a retired snapshot queue its source afterward.
        if (!EditorObservationCurrent(*record))
        {
            ++editorObservationEpoch;
            editorObservation.reset();
            editorSeenRevisions.clear();
            if (auto attempt = editorOpeningStop.load()) attempt->request_stop();
            conversationStarter.ClearSavedFiles();
            return refuse("The source changed while the saved-file event was being admitted.");
        }
        if (!seen)
        {
            const std::weak_ptr<const EditorObservationRecord> weak = record;
            conversationStarter.ObserveSavedFile(perception::SavedFileContext(record->source), record->source.occurredAt,
                [this, weak]
                {
                    const auto current = weak.lock();
                    return current && userInteractionGeneration.load() == current->inputGeneration && EditorObservationCurrent(*current);
                });
        }
    }
    if (!seen)
    {
        SignalInitiative("an authorized saved C/C++ revision became stable");
    }
    result.accepted = true;
    result.eventId = "saved-" + std::to_string(record->epoch);
    result.reason = seen ? "A previously considered revision is current again; no duplicate comment was requested."
                         : "Saved source admitted; Revia may comment or choose silence after the normal attention gates.";
    PublishComponent("Editor awareness", "Saved file", "One approved saved revision is current. No unsaved editor content is available.",
        -1.0, 1, 0, {}, input.audience.revision, input.stamp);
    return result;
}

bool ReviaSession::EditorObservationCurrent(const EditorObservationRecord& record) const
{
    if (editorObservationEpoch.load() != record.epoch || !InputContextCurrent(record.input) ||
        record.input.stamp.policyVersion != companionAuthority->Revision() || windowEventMonitor.IsPaused())
        return false;
    {
        std::lock_guard lock(preferenceSnapshotMutex);
        if (!settings.perception.bEnabled || !settings.localApi.bEnabled || !settings.localApi.bTrustedPrivate)
            return false;
    }
    std::string reason;
    if (!perception::ValidateSavedFileObservation(record.source, std::chrono::system_clock::now(), reason))
        return false;
    actions::ActionRequest request;
    request.type = actions::ActionType::ReadTextFile;
    request.source = record.source.path;
    const auto allowed = actionRuntime.Evaluate(request);
    if (allowed.verdict != actions::PolicyVerdict::Allowed || allowed.canonicalSource != record.source.path)
        return false;
    // Revalidation reads only the already admitted bounded file. It does not execute
    // model-proposed paths or create a new action capability.
    std::ifstream file(record.source.path, std::ios::binary);
    std::string current(16385, '\0');
    file.read(current.data(), static_cast<std::streamsize>(current.size()));
    current.resize(static_cast<std::size_t>(file.gcount()));
    return file.eof() && !file.bad() && audit::ContentDigest(current) == record.source.revision;
}

std::string ReviaSession::CurrentEditorContext() const
{
    if (windowEventMonitor.IsPaused())
        return "Saved-file awareness is paused. No current saved or unsaved editor content is available through this integration.";
    std::shared_ptr<const EditorObservationRecord> record;
    {
        std::lock_guard lock(editorObservationMutex);
        record = editorObservation;
    }
    if (!record) return {};
    if (!EditorObservationCurrent(*record))
        return "The previous saved-file observation is no longer current or authorized. No current code is available from that "
               "integration; do not claim to see unsaved edits or rely on the old source as a verified current revision.";
    return perception::SavedFileContext(record->source);
}

void ReviaSession::ClearEditorObservation()
{
    std::lock_guard lock(editorObservationMutex);
    ++editorObservationEpoch;
    editorObservation.reset();
    editorSeenRevisions.clear();
    if (auto attempt = editorOpeningStop.load()) attempt->request_stop();
    conversationStarter.ClearSavedFiles();
}

void ReviaSession::ExpireEditorObservation(const std::chrono::system_clock::time_point now)
{
    bool expired = false;
    {
        std::lock_guard lock(editorObservationMutex);
        if (editorObservation && now - editorObservation->source.occurredAt > std::chrono::seconds{120})
        {
            ++editorObservationEpoch;
            editorObservation.reset();
            if (auto attempt = editorOpeningStop.load()) attempt->request_stop();
            conversationStarter.ClearSavedFiles();
            expired = true;
        }
    }
    if (expired)
    {
        PublishComponent("Editor awareness", "Expired", "The temporary saved-source snapshot expired; a fresh save is required.");
    }
}

bool ReviaSession::PreparePrivateContext(const memory::MemoryScope& scope, const std::function<bool()>& admission)
{
    const auto admitted = [&]
    {
        try { return admission && admission(); }
        catch (...) { return false; }
    };
    if (!admitted() || scope.participantId.empty() || scope.audience.kind != identity::AudienceKind::Private)
        return false;
    const auto current = CapturePrivateMemoryScope();
    if (current.participantId != scope.participantId || current.companionId != scope.companionId ||
        current.audience.kind != scope.audience.kind || current.audience.audienceId != scope.audience.audienceId ||
        current.audience.revision != scope.audience.revision || current.audience.recipientEntityIds != scope.audience.recipientEntityIds)
        return false;
    if (privateContextParticipant != scope.participantId || privateContextAudience != scope.audience.audienceId)
    {
        conversationRuntime.ResetParticipantContinuity();
        privateContextParticipant = scope.participantId;
        privateContextAudience = scope.audience.audienceId;
        // Transient local:user continuity is valid; durable restoration retains its
        // stricter attributed-private check inside RestoreConversationContext.
        RestoreConversationContext(scope, admission);
    }
    return admitted();
}

void ReviaSession::RetireOptionalOpeningLocked(const std::stop_token token, const RuntimeStamp& origin,
    const identity::AudienceContext& audience)
{
    if (!started.load() || !sessionIdentity.IsCurrent(origin) || CurrentOperationToken() != token)
        return;
    const auto currentAudience = Audience();
    if (currentAudience.kind != audience.kind || currentAudience.audienceId != audience.audienceId ||
        currentAudience.revision != audience.revision || currentAudience.recipientEntityIds != audience.recipientEntityIds)
        return;
    const auto currentState = state.load();
    if (currentState == RuntimeState::Thinking || currentState == RuntimeState::Responding)
        SetState(RuntimeState::Idle, "The optional observation has finished.", audience.revision, origin);
}

std::function<bool()> ReviaSession::CaptureInitiativeAdmission(const initiative::Proposal& proposal,
    const RuntimeStamp& origin, const identity::AudienceContext& audience, const std::uint64_t inputGeneration,
    const std::uint64_t awarenessEpoch, const bool requireSpareResources,
    std::function<initiative::AttentionContext()> sampleAttention) const
{
    return [this, origin, audience, inputGeneration, awarenessEpoch, requireSpareResources,
        sampleAttention = std::move(sampleAttention),
        requiresAwareness = proposal.kind == initiative::Proposal::Kind::ConversationStarter, sourceAdmission = proposal.admission]
    {
        initiativeSettings initiativeConfig;
        perceptionSettings perceptionConfig;
        {
            std::lock_guard lock(preferenceSnapshotMutex);
            initiativeConfig = settings.initiative;
            perceptionConfig = settings.perception;
        }
        auto attention = sampleAttention ? sampleAttention() : initiative::SampleDesktop(perceptionConfig);
        attention.inputNeverPauses = inputRhythm.NeverPauses();
        const initiative::AttentionPolicy hardGates(initiativeConfig);
        const bool enabled = (!requiresAwareness || perceptionConfig.bEnabled) &&
            hardGates.Evaluate(1.0F, attention) == initiative::AttentionVerdict::Speak;
        const auto load = CurrentLoad();
        // Compute activity admits new work; only capacity throttling retires work
        // already running. An opening's own inference may make the GPU busy.
        const bool resourcesAdmitted = requireSpareResources ? load.allowOptionalBackgroundWork
                                                           : load.state != resources::LoadState::Throttled;
        return started.load() && Admits(origin) && origin.policyVersion == companionAuthority->Revision() &&
            audience.kind == identity::AudienceKind::Private && Audience().revision == audience.revision &&
            userInteractionGeneration.load() == inputGeneration && editorObservationEpoch.load() == awarenessEpoch &&
            enabled && (!requiresAwareness || !windowEventMonitor.IsPaused()) && !initiativeController.IsQuiet() &&
            !speechRecognitionService.IsRecording() &&
            resourcesAdmitted && (!sourceAdmission || sourceAdmission());
    };
}
}
