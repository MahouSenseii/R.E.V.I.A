#pragma once

#include "Core/messageRouter.h"
#include "Evaluation/answerJudge.h"

#include <string>

namespace revia::quality
{
// The default judge: the local llama.cpp tiers through messageRouter's bounded structured
// review call, which prefers Expert when one is configured and falls back to Main. It runs
// at low temperature with the verdict schema enforced by the server's grammar.
//
// A cloud judge belongs in its own JudgeBackend; nothing here needs to change for it.
class RouterJudgeBackend final : public evaluation::JudgeBackend
{
  public:
    RouterJudgeBackend(const messageRouter& judgeRouter, std::string judgeIdentity) : router(judgeRouter), identity(std::move(judgeIdentity))
    {
    }

    [[nodiscard]] evaluation::JudgeCompletion Complete(const evaluation::JudgeRequest& request, const std::stop_token stopToken) const override
    {
        const auto output = router.ReviewCode(request.system, request.user, request.schema, stopToken);
        evaluation::JudgeCompletion completion;
        completion.succeeded = output.bSuccess;
        completion.text = output.response;
        completion.error = output.bSuccess ? std::string() : output.reason;
        return completion;
    }

    [[nodiscard]] std::string Identity() const override
    {
        return identity;
    }

  private:
    const messageRouter& router;
    std::string identity;
};
}
