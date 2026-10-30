#include "Runtime/reviaSession.h"

#include "videoPublicationLane.h"

#include "Actions/actionTypes.h"
#include "Attachments/videoSelection.h"
#include "Visual/imageArtifact.h"
#include "Visual/videoRequestPolicy.h"

#include <utility>

namespace revia::runtime
{
SessionResult ReviaSession::StartVideoGeneration(
    const std::string& input, const std::vector<conversationMediaReference>& references, const agents::InputContext& captured)
{
    SessionResult result;
    result.succeeded = false;
    if (!InputContextCurrent(captured) || captured.audience.kind != identity::AudienceKind::Private)
    {
        result.text = result.reason = "Video generation requires the current private conversation.";
        return result;
    }
    const auto imageState = imageGenerator.Snapshot().state;
    if (imageState == "starting" || imageState == "queued" || imageState == "loading" || imageState == "generating" ||
        imageState == "running" || imageState == "cancelling")
    {
        result.text = result.reason = "A picture job is still running. Wait for it or stop it before starting a video.";
        return result;
    }
    std::string detail;
    if (!videoGenerator.IsAvailable(detail))
    {
        result.text = result.reason = detail;
        return result;
    }
    visual::VideoRequest request;
    request.prompt = visual::VideoGenerationPrompt(input);
    auto& library = Attachments();
    const auto epoch = conversationEpoch.load();
    const auto revision = library.MediaRevision();
    if (!references.empty())
    {
        if (references.size() != 1 || attachments::IsVideoReference(references.front()))
        {
            result.text = result.reason = "Choose one still image to animate. Editing an existing video is not supported.";
            return result;
        }
        const auto source = library.ReadImageSource(references.front());
        if (!source)
        {
            result.text = result.reason = "The animation reference is unavailable or changed. No video job was started.";
            return result;
        }
        request.mode = visual::VideoRequestMode::ImageToVideo;
        request.source = visual::ImageSource{source->reference.id, source->reference.digest, source->pixels.mimeType, source->pixels.bytes,
            source->pixels.width, source->pixels.height};
        if (!visual::VerifyImageSource(*request.source, detail))
        {
            result.text = result.reason = detail;
            return result;
        }
    }
    if (!InputContextCurrent(captured) || conversationEpoch.load() != epoch || library.MediaRevision() != revision ||
        (!references.empty() && !library.ImageReferencesCurrent(references)))
    {
        result.text = result.reason = "The reference was withdrawn before the video job could start.";
        return result;
    }
    const memory::MemoryScope scope{
        captured.participantId, captured.audience, captured.participantSource, captured.consentRevision, captured.stamp.companionId};
    std::string launched;
    result.succeeded = LaunchTask(
        "Generate a short video",
        [this, request, references, captured, epoch, revision, scope, &library](const std::stop_token stopToken)
        {
            goals::Goal goal;
            goal.title = "Generate a short video";
            goal.status = goals::GoalStatus::Failed;
            RuntimeStamp origin;
            {
                std::lock_guard lock(taskMutex);
                if (!activeTask || !activeTask->videoGeneration || taskStopSource.get_token() != stopToken)
                {
                    goal.stopDetail = "The original media task is no longer registered.";
                    return goal;
                }
                origin = activeTask->stamp;
                activeTask->mediaEpoch = epoch;
            }
            const auto admitted = [this, captured, origin, epoch, revision, references, &library, stopToken]
            {
                return !stopToken.stop_requested() && InputContextCurrent(captured) && Admits(origin) &&
                       conversationEpoch.load() == epoch && library.MediaRevision() == revision &&
                       (references.empty() || library.ImageReferencesCurrent(references));
            };
            actions::ActionRequest action;
            action.id = actions::NewActionId();
            action.type = actions::ActionType::GenerateVideo;
            action.value = request.prompt;
            action.videoRequest = request;
            action.beforeEffect = [admitted](const std::string&)
            { return admitted() ? std::string{} : "The original video request is no longer admitted."; };
            const auto executed = actionRuntime.ExecuteFor(origin, action, false, stopToken);
            if (!executed.result.video || !executed.result.video->succeeded)
            {
                goal.status = stopToken.stop_requested() ? goals::GoalStatus::Cancelled : goals::GoalStatus::Failed;
                goal.stopDetail = executed.Message();
                return goal;
            }
            const auto stillAdmitted = [&] { return admitted() && (references.empty() || library.ImageReferencesCurrent(references)); };
            const auto imported =
                library.ImportGeneratedVideo(*executed.result.video, videoGenerator.OutputDirectory(), stillAdmitted, stopToken);
            if (!executed.Succeeded() || !imported.succeeded)
            {
                if (imported.succeeded)
                    DiscardGeneratedMedia({imported.reference});
                goal.status = stopToken.stop_requested() || !admitted() ? goals::GoalStatus::Cancelled : goals::GoalStatus::Failed;
                goal.stopDetail = imported.error.empty() ? executed.Message() : imported.error;
                return goal;
            }
            auto operationLock = AcquireVideoPublicationLane(operationMutex, stillAdmitted);
            if (!operationLock.owns_lock() || !stillAdmitted() || !library.ImageReferencesCurrent({imported.reference}))
            {
                DiscardGeneratedMedia({imported.reference});
                goal.status = goals::GoalStatus::Cancelled;
                goal.stopDetail = "The generated clip was withdrawn before publication.";
                return goal;
            }
            const std::string report = "The short video is ready and attached. I haven't inspected it yet.";
            conversationMessage assistant{"assistant", report, scope.participantId, scope};
            assistant.mediaReferences = {imported.reference};
            context.AddMessage(std::move(assistant));
            ArchiveTurn("assistant", report, scope, stillAdmitted, {imported.reference});
            {
                std::lock_guard lock(taskMutex);
                if (activeTask && activeTask->stamp.taskId == origin.taskId)
                {
                    activeTask->generatedMedia = imported.reference;
                    activeTask->mediaEpoch = epoch;
                }
            }
            goal.status = goals::GoalStatus::Succeeded;
            goal.stopReason = goals::StopReason::Completed;
            goal.stopDetail = "Verified the generated MP4's content identity, duration, dimensions, frame count and readable frames.";
            return goal;
        },
        launched, true);
    result.text = launched;
    if (!result.succeeded)
        result.reason = launched;
    result.fromAssistant = true;
    return result;
}
}
