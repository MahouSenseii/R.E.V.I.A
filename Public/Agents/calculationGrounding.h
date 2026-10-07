#pragma once

#include "Core/conversationMessage.h"
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::agents
{
using CalculationProposer = std::function<std::string(const std::string&, std::stop_token)>;

struct CalculationGrounding
{
    bool ran = false;
    std::string promptBlock;
    std::string reason;
};

[[nodiscard]] std::string CalculationProposalInstructions();
[[nodiscard]] std::string CalculationProposalSchema(const std::string& envelope = {});
[[nodiscard]] CalculationGrounding BuildCalculationGrounding(const std::string& input, const std::vector<conversationMessage>& context,
    const CalculationProposer& proposer, std::stop_token stopToken = {}, const std::function<bool()>& admission = {});
}
