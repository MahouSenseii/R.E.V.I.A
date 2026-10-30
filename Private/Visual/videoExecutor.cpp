#include "Visual/videoExecutor.h"

#include <utility>

namespace revia::visual
{
VideoExecutor::VideoExecutor(VideoGenerator& inputProvider, std::filesystem::path ownedOutputRoot)
    : provider(inputProvider), outputRoot(std::move(ownedOutputRoot))
{
}

bool VideoExecutor::Handles(const actions::ActionType type) const
{
    return type == actions::ActionType::GenerateVideo;
}

actions::ActionResult VideoExecutor::Execute(const actions::ActionRequest& request, const actions::PolicyDecision& decision)
{
    actions::ActionResult result;
    result.backend = "local_ltx_video";
    result.dryRun = request.dryRun;
    std::error_code error;
    const auto providerRoot = std::filesystem::weakly_canonical(provider.OutputDirectory(), error);
    if (!Handles(request.type) || !request.videoRequest || !request.beforeEffect || error || providerRoot != outputRoot ||
        decision.canonicalDestination != outputRoot)
    {
        result.message = "Video execution requires the admitted companion-owned provider destination and typed request.";
        return result;
    }
    const auto admitted = [&] { return request.beforeEffect(actions::PathToUtf8(outputRoot)).empty(); };
    if (!admitted())
    {
        result.message = "Video admission expired before generation.";
        return result;
    }
    if (request.dryRun)
    {
        result.succeeded = true;
        result.message = "Would generate a video in the companion's owned video directory.";
        return result;
    }
    result.attempted = true;
    result.video = provider.Generate(*request.videoRequest, request.executionStop, admitted, outputRoot);
    result.succeeded = result.video->succeeded && admitted();
    result.message = result.video->message;
    result.content = result.video->detail;
    if (result.succeeded)
        result.entries.push_back(actions::PathToUtf8(result.video->path));
    else if (result.video->succeeded)
        result.message = "The video was generated, but its admission expired before publication.";
    return result;
}
}
