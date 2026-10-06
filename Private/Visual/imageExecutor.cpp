#include "Visual/imageExecutor.h"

#include <utility>

namespace revia::visual
{
ImageExecutor::ImageExecutor(ImageGenerator& inputProvider, std::filesystem::path ownedOutputRoot)
    : provider(inputProvider), outputRoot(std::move(ownedOutputRoot))
{
}

bool ImageExecutor::Handles(const actions::ActionType type) const
{
    return type == actions::ActionType::GenerateImage;
}

actions::ActionResult ImageExecutor::Execute(const actions::ActionRequest& request, const actions::PolicyDecision& decision)
{
    actions::ActionResult result;
    result.backend = "local_diffusers";
    result.dryRun = request.dryRun;
    std::error_code error;
    const auto providerRoot = std::filesystem::weakly_canonical(provider.OutputDirectory(), error);
    if (!Handles(request.type) || !request.beforeEffect || error || providerRoot != outputRoot ||
        decision.canonicalDestination != outputRoot)
    {
        result.message = "Image execution requires the admitted companion-owned provider destination.";
        return result;
    }
    const auto admitted = [&]() { return request.beforeEffect(actions::PathToUtf8(outputRoot)).empty(); };
    if (!admitted())
    {
        result.message = "Image admission expired before generation.";
        return result;
    }
    if (request.dryRun)
    {
        result.succeeded = true;
        result.message = "Would generate an image in the companion's owned image directory.";
        return result;
    }
    result.attempted = true;
    result.image = provider.Generate(request.value, {}, {}, admitted, outputRoot);
    result.succeeded = result.image->succeeded;
    result.message = result.image->message;
    if (result.succeeded)
    {
        result.entries.push_back(actions::PathToUtf8(result.image->path));
        result.content = result.image->detail;
        if (!admitted())
        {
            result.succeeded = false;
            result.message = "The image file was generated, but its admission expired before publication.";
        }
    }
    return result;
}
}
