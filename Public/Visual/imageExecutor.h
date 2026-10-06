#pragma once

#include "Actions/IActionExecutor.h"
#include "Visual/imageGenerator.h"

namespace revia::visual
{
class ImageExecutor final : public actions::IActionExecutor
{
  public:
    ImageExecutor(ImageGenerator& provider, std::filesystem::path ownedOutputRoot);
    [[nodiscard]] bool Handles(actions::ActionType type) const override;
    [[nodiscard]] actions::ActionResult Execute(const actions::ActionRequest& request, const actions::PolicyDecision& decision) override;

  private:
    ImageGenerator& provider;
    std::filesystem::path outputRoot;
};
}
