#pragma once

#include "Actions/IActionExecutor.h"
#include "Visual/videoGenerator.h"

namespace revia::visual
{
class VideoExecutor final : public actions::IActionExecutor
{
  public:
    VideoExecutor(VideoGenerator& provider, std::filesystem::path ownedOutputRoot);
    [[nodiscard]] bool Handles(actions::ActionType type) const override;
    [[nodiscard]] actions::ActionResult Execute(const actions::ActionRequest& request, const actions::PolicyDecision& decision) override;

  private:
    VideoGenerator& provider;
    std::filesystem::path outputRoot;
};
}
