#pragma once

#include <stop_token>
#include <string>

namespace revia::evaluation
{
struct CalculationResult
{
    bool succeeded = false;
    std::string expression;
    std::string value;
    std::string unit;
    std::string limitations;
    std::string refusal;
};

CalculationResult VerifyCalculation(const std::string& typedInputJson, std::stop_token stopToken = {});
}
