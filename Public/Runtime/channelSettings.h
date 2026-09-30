#pragma once

#include <string>
#include <vector>

struct conversationChannelSettings
{
    // Executables Revia may speak for even when composing into them. Empty by default:
    // typing into Discord or a browser is text, and narrating it is noise.
    std::vector<std::string> voiceEnabledApplications;
    // Named so the reason a reply was silent can be reported rather than guessed at.
    std::vector<std::string> textOnlyApplications = {
        "discord.exe", "slack.exe", "teams.exe", "telegram.exe", "whatsapp.exe",
        "msedge.exe", "chrome.exe", "firefox.exe", "thunderbird.exe", "outlook.exe"
    };
};
