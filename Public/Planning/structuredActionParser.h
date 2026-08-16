#pragma once

#include "Actions/actionTypes.h"

#include <string>

namespace revia::planning
{

struct ParsedAction
{
    bool recognized = false;
    bool succeeded = false;
    actions::ActionRequest request;
    std::string error;
};

class StructuredActionParser
{
public:
    [[nodiscard]] ParsedAction ParseJson(const std::string& input) const;
    [[nodiscard]] ParsedAction ParseCommand(const std::string& input) const;

private:
    [[nodiscard]] static std::vector<std::string> Tokenize(const std::string& input);
    [[nodiscard]] static std::string StripCodeFence(const std::string& input);
};

} // namespace revia::planning
