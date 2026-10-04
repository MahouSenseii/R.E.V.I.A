#include "Runtime/reviaSession.h"
#include "Runtime/runtimeEvents.h"
#include "Diagnostics/issueLog.h"
#include "Speech/addresseeGate.h"
#include "Speech/speechService.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace revia::runtime
{

namespace
{
constexpr auto SynthesisIssueComponent = "Voice";
constexpr auto SynthesisIssueCode = "synthesis-failed";
constexpr auto SynthesisFailureNotice = "Voice synthesis failed for this reply. The text remains available. The cause is unknown.";

std::pair<std::string, std::string> PresentSystemCue(const speech::SystemCuePhase phase)
{
    using speech::SystemCuePhase;
    switch (phase)
    {
    case SystemCuePhase::Preparing:
        return {"Preparing", "Preparing approved status clips while voice work is idle."};
    case SystemCuePhase::Prepared:
        return {"Prepared", "Validated status clips are cached for the selected voice."};
    case SystemCuePhase::PreparationFailed:
        return {"PreparationFailed", "Status clips could not be fully prepared. Text remains available."};
    case SystemCuePhase::Queued:
        return {"Queued", "A validated cached status clip is queued."};
    case SystemCuePhase::Playing:
        return {"Playing", "A cached status clip is playing."};
    case SystemCuePhase::Played:
        return {"Played", "Cached playback finished. It does not verify fresh synthesis."};
    case SystemCuePhase::Cancelled:
        return {"Cancelled", "Cached status playback was cancelled."};
    case SystemCuePhase::PlaybackFailed:
        return {"PlaybackFailed", "Cached audio playback failed. Text remains available."};
    case SystemCuePhase::Unavailable:
        return {"Unavailable", "Approved status clips are not fully available for the selected voice. Text remains available."};
    }
    return {"Unavailable", "Status clips are unavailable. Text remains available."};
}

std::pair<std::string, std::string> PresentManualVoice(const std::string& phase)
{
    if (phase == "ManualLoading")
        return {"Loading", "Preparing the selected voice."};
    if (phase == "ManualDesigning")
        return {"Designing", "Creating or preparing voice assets."};
    if (phase == "ManualGenerating")
        return {"Generating", "Generating a voice preview."};
    if (phase == "ManualReady")
        return {"Ready", "The voice-studio operation completed. Ordinary synthesis health is unchanged."};
    if (phase == "ManualFallback")
        return {"Fallback", "Voice preparation did not complete. The cause is unknown."};
    if (phase == "ManualError")
        return {"Error", "The voice-studio operation did not complete. The cause is unknown."};
    if (phase == "ManualWarning")
        return {"Warning", "Voice assets could not be fully prepared. The cause is unknown."};
    return {};
}
}

void ReviaSession::ReportSpeechHistoryFailure()
{
    if (speechHistoryWarningReported.exchange(true))
        return;
    const std::string message = "Voice health is available for this session, but its issue history could not be saved or loaded.";
    appLogger.Warning(message);
}

void ReviaSession::LoadSpeechFaultHistory()
{
    std::string error;
    {
        std::lock_guard lock(speechObservationMutex);
        if (speechIssuesLoaded)
            return;
        speechIssuesLoaded = true;
        speechIssues.Load(error);
    }
    if (!error.empty())
        ReportSpeechHistoryFailure();
}

void ReviaSession::RestoreSelectedSpeechFault()
{
    const auto snapshot = speechService.SynthesisHealthSnapshot();
    if (!snapshot.configured || snapshot.state == speech::SynthesisHealth::Degraded)
        return;
    const auto issue =
        speechIssues.Find(SynthesisIssueComponent, SynthesisIssueCode, snapshot.selection.profileId, snapshot.selection.presetId);
    if (issue && issue->status == diagnostics::IssueStatus::Open)
    {
        // Rehydrate an evicted scope before publishing its current capability.
        speechService.RestoreSynthesisFault(snapshot.selection.profileId, snapshot.selection.presetId);
    }
}

void ReviaSession::PublishVoiceHealth() const
{
    const auto snapshot = speechService.SynthesisHealthSnapshot();
    std::string phase = "Unverified";
    std::string message = "Local voice synthesis has not been verified for the selected voice in this session.";
    if (snapshot.state == speech::SynthesisHealth::Degraded)
    {
        phase = "Degraded";
        message =
            snapshot.restoredFailure
                ? "A voice synthesis failure was recorded for this voice. Fresh synthesis has not verified recovery. The cause is unknown."
                : SynthesisFailureNotice;
    }
    else if (snapshot.state == speech::SynthesisHealth::Available)
    {
        phase = "Available";
        message = "Fresh local voice synthesis succeeded for this voice. Speaker playback has not been verified.";
    }
    if (!snapshot.enabled)
    {
        phase = "Disabled";
        message = "Voice output is switched off. " + message;
    }
    PublishComponent("Voice health", phase, message);
}

void ReviaSession::HandleSynthesisObservation(const speech::SynthesisObservation& observation)
{
    if (observation.observationId == 0)
        return;
    bool storageFailed = false;
    bool announceFailure = false;
    {
        std::lock_guard lock(speechObservationMutex);
        if (observation.observationId <= lastSpeechObservationId)
            return;
        lastSpeechObservationId = observation.observationId;
        if (observation.attemptId != 0 && observation.stateChanged)
        {
            std::string error;
            if (!observation.succeeded && observation.state == speech::SynthesisHealth::Degraded)
            {
                diagnostics::Issue issue;
                issue.component = SynthesisIssueComponent;
                issue.code = SynthesisIssueCode;
                issue.profileId = observation.selection.profileId;
                issue.voiceId = observation.selection.presetId;
                issue.correlationId =
                    "epoch=" + std::to_string(observation.selection.epoch) + ";generation=" + std::to_string(observation.generation) +
                    ";turn=" + std::to_string(observation.utteranceId) + ";sequence=" + std::to_string(observation.sequence) +
                    ";attempt=" + std::to_string(observation.attemptId) + ";observation=" + std::to_string(observation.observationId);
                issue.severity = diagnostics::IssueSeverity::Degraded;
                issue.summary = "Local voice synthesis failed; replies remain available as text.";
                issue.detail = "A synthesis request failed. The cause is unknown.";
                issue.remedy = "Check the voice service and verify a fresh synthesis request before treating the voice as recovered.";
                storageFailed = !speechIssues.Record(issue, error);
                announceFailure = true;
            }
            else if (observation.succeeded && observation.fresh && observation.state == speech::SynthesisHealth::Available &&
                     observation.attemptId > observation.failureWatermark)
            {
                const auto current = speechService.SynthesisHealthSnapshot(observation.selection.profileId, observation.selection.presetId);
                // Restoration can invalidate a success while its callback is queued.
                if (current.state == speech::SynthesisHealth::Available && observation.attemptId > current.failureWatermark)
                {
                    speechIssues.Resolve(SynthesisIssueComponent, SynthesisIssueCode, observation.selection.profileId,
                        observation.selection.presetId, &error);
                    storageFailed = !error.empty();
                }
            }
        }
    }
    // Subscriber callbacks can select another voice; keep them outside admission locks.
    if (storageFailed)
        ReportSpeechHistoryFailure();
    if (observation.attemptId == 0)
        RestoreSelectedSpeechFault();
    if (announceFailure)
    {
        appLogger.Warning(SynthesisFailureNotice);
    }
    PublishVoiceHealth();
}

void ReviaSession::HandleSpeechEvent(const speech::SpeechEvent& speechEvent)
{
    if (speechEvent.synthesis)
    {
        HandleSynthesisObservation(*speechEvent.synthesis);
        return;
    }
    if (speechEvent.phase.starts_with("Manual"))
    {
        const auto [phase, message] = PresentManualVoice(speechEvent.phase);
        if (!phase.empty())
            PublishComponent("Voice studio", phase, message);
        return;
    }
    if (speechEvent.phase.starts_with("Cue"))
    {
        // A queued notification may belong to a previous selection.
        const auto [phase, message] = PresentSystemCue(speechService.SystemCueStatusSnapshot().phase);
        PublishComponent("System cues", phase, message);
        if (speechEvent.phase == "CuePlaying" || speechEvent.phase == "CuePlayed" || speechEvent.phase == "CueCancelled" ||
            speechEvent.phase == "CuePlaybackFailed" || speechEvent.phase == "CueUnavailable")
        {
            const bool playing = speechService.IsAudioPlaying();
            speechRecognitionService.SetOutputActive(playing);
            if (playing)
                CancelScreenAwarenessAttempt();
            PublishComponent(
                "Voice", playing ? "Speaking" : "Stopped", playing ? "Voice audio is playing." : "Voice audio playback has finished.");
        }
        return;
    }
    if (speechEvent.phase == "Queued" || speechEvent.phase == "Generating" || speechEvent.phase == "Speaking")
    {
        // Speech preempts shared-GPU visual refresh; its last summary stays available.
        CancelScreenAwarenessAttempt();
    }
    if (speechEvent.phase == "Speaking")
    {
        speechRecognitionService.SetOutputActive(true);
    }
    else if (speechEvent.phase == "Ready" || speechEvent.phase == "Stopped" || speechEvent.phase == "Interrupted" ||
             speechEvent.phase == "Error" || speechEvent.phase == "Disabled" || speechEvent.phase == "Fallback")
    {
        speechRecognitionService.SetOutputActive(false);
        // Keep the conversational follow-up window open after speech.
        addresseeGate.NoteExchange(speech::AddresseeGate::Clock::now());
        // Exchange the active intent once so a completed report cannot release it twice.
        if (const std::uint64_t finished = speakingIntentId.exchange(0); finished != 0)
        {
            speechCoordinator.NotePlaybackFinished(finished);
        }
    }
    if (speechEvent.phase == "Generated" && speechEvent.elapsedMilliseconds >= 0.0)
    {
        appLogger.Timing(
            "voice utterance #" + std::to_string(speechEvent.utteranceId), {{"qwen_synthesis", speechEvent.elapsedMilliseconds, true}});
    }
    if (speechEvent.phase == "Batch")
    {
        // Keep batch throughput evidence beside its request.
        appLogger.Log("[Voice] batch #" + std::to_string(speechEvent.utteranceId) + " | " + speechEvent.detail);
    }
    if (speechEvent.phase == "BatchFallback")
    {
        // Surface the slower per-phrase fallback even when synthesis succeeds.
        appLogger.Warning(
            "[Voice] batch fell back to per-phrase synthesis (#" + std::to_string(speechEvent.utteranceId) + "): " + speechEvent.detail);
    }
    if (speechEvent.phase == "BackendVerified")
    {
        appLogger.Log("[Voice] " + speechEvent.detail);
    }
    if (speechEvent.phase == "BackendMismatch")
    {
        // A backend mismatch invalidates measurements of the configured fast path.
        appLogger.Warning("[Voice] " + speechEvent.detail);
    }
    if (speechEvent.phase == "Profile" && !speechEvent.timings.empty())
    {
        appLogger.Timing("voice stages #" + std::to_string(speechEvent.utteranceId), speechEvent.timings);
        // Preserve device and queue conditions beside stage timings.
        appLogger.Log("[Voice] request #" + std::to_string(speechEvent.utteranceId) + " | " + speechEvent.detail);
    }
    if ((speechEvent.phase == "FirstAudioReady" || speechEvent.phase == "FirstAudioPlayed") && speechEvent.elapsedMilliseconds >= 0.0)
    {
        appLogger.Timing("voice utterance #" + std::to_string(speechEvent.utteranceId),
            {{speechEvent.phase == "FirstAudioReady" ? "first_audio_ready" : "first_audio_played", speechEvent.elapsedMilliseconds, true}});
    }
    RuntimeEvent event;
    event.kind = RuntimeEventKind::ComponentStatus;
    event.state = state.load();
    event.message = speechEvent.detail;
    event.component = "Voice";
    event.phase = speechEvent.phase;
    event.resource = speechEvent.device;
    event.elapsedMilliseconds = speechEvent.elapsedMilliseconds;
    event.queueDepth = speechEvent.queueDepth;
    // Use the existing turn correlation for text and audio.
    event.turnId = speechEvent.utteranceId;
    event.utteranceId = speechEvent.utteranceId;
    const std::size_t workerMarker = speechEvent.device.find("voice-worker-");
    if (workerMarker != std::string::npos)
    {
        RuntimeEvent workerEvent = event;
        const std::size_t workerEnd = speechEvent.device.find(" / ", workerMarker);
        workerEvent.component = "Voice " + speechEvent.device.substr(
                                               workerMarker, workerEnd == std::string::npos ? std::string::npos : workerEnd - workerMarker);
        eventBus.Publish(std::move(workerEvent));
    }
    eventBus.Publish(std::move(event));
}

} // namespace revia::runtime
