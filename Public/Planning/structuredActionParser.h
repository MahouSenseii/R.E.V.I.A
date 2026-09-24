#pragma once

#include "Actions/actionTypes.h"

#include <nlohmann/json.hpp>

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

    // One already-decoded action object. A multi-step goal plan carries its actions
    // nested rather than as separate documents, and both paths have to agree on what a
    // valid action looks like, so they share this.
    [[nodiscard]] static ParsedAction ParseObject(const nlohmann::json& data);

private:
    [[nodiscard]] static std::vector<std::string> Tokenize(const std::string& input);
    [[nodiscard]] static std::string StripCodeFence(const std::string& input);
};

} // namespace revia::planning
