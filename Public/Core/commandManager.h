#pragma once

#include "Library/structLibrary.h"
#include <string>

class messageRouter;
class configManager;

class commandManager
{
public:
    commandManager();
    ~commandManager();

    commandOutput HandleCommand(const std::string &input, appSettings &settings, aiProfile &profile, configManager &config, messageRouter &router) const;
private:

    bool IsCommand(const std::string& input) const;
    static std::string StatusToString(systemStatus status);
    commandOutput BuildHelpOutput() const;
    commandOutput BuildStatusOutput(const appSettings& settings,const aiProfile& profile, const healthOutput& llmHealth) const;
    commandOutput HandleProfileCommand(const std::string& input,appSettings& settings,aiProfile& profile,
    configManager& config,messageRouter& router) const;

};
