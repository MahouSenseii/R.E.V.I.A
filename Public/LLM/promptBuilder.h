#pragma once

#include "Library/structLibrary.h"

#include <nlohmann/json.hpp>
#include <vector>

class promptBuilder
{
public:
    promptBuilder();
    ~promptBuilder();

    nlohmann::json BuildMessages(
        const aiProfile& profile,
        const std::vector<conversationMessage>& context
    ) const;
};