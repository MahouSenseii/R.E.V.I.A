#pragma once

#include "Library/structLibrary.h"
#include <functional>
#include <string>

class messageRouter;

class commandManager
{
public:
    commandManager();
    ~commandManager();

    using ProfileActivator = std::function<commandOutput(const std::string&)>;
    commandOutput HandleCommand(const std::string& input, const appSettings& settings,
        const aiProfile& profile, messageRouter& router,
        const ProfileActivator& activateProfile) const;
private:

    bool IsCommand(const std::string& input) const;
    static std::string StatusToString(systemStatus status);
    commandOutput BuildHelpOutput() const;
    commandOutput BuildStatusOutput(
        const appSettings& settings,
        const aiProfile& profile,
        const healthOutput& llmHealth,
        const healthOutput& embeddingHealth) const;
};
