#pragma once

#include <string>
#include <vector>

// Opt-in focus/title metadata excludes matching windows entirely.
// Exclusions do not mask separately permitted screenshots; awareness may capture admitted events.
struct perceptionSettings
{
    bool bEnabled = false;
    // Coalescing window. Title changes fire per keystroke in some editors, and a
    // per-keystroke record of a document title is a transcript by another name.
    int minimumEventIntervalMs = 750;
    int maxObservationsPerMinute = 60;
    std::vector<std::string> excludedApplications = {
        "keepass.exe", "keepassxc.exe", "1password.exe", "bitwarden.exe",
        "lastpass.exe", "dashlane.exe", "protonpass.exe", "enpass.exe"
    };
    std::vector<std::string> excludedTitleFragments = {
        "incognito", "inprivate", "private browsing", "private window",
        "password", "passphrase", "seed phrase", "recovery phrase",
        "authenticator", "one-time code", "bank", "banking", "credit card"
    };
};
