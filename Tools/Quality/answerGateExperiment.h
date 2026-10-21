#pragma once

#include <stdexcept>

namespace revia::quality
{
struct AnswerGateExperiment
{
    bool authoredSystemPrompt = true;
    bool statePacket = true;
    bool styleGuidance = true;

    [[nodiscard]] bool IsControl() const { return authoredSystemPrompt && statePacket && styleGuidance; }

    void ValidateBaselineRequest(bool writeBaseline, bool judgeEnabled, bool filteredCases) const
    {
        if (writeBaseline && (!IsControl() || !judgeEnabled || filteredCases))
            throw std::runtime_error("A baseline requires all prompt blocks, the complete corpus, and judge scoring.");
    }
};
}
