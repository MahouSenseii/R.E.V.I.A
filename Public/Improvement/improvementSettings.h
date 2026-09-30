#pragma once

#include <string>

// Revia reviewing her own source and proving what she suggests. She never edits the real
// source: a proposal is a patch a person applies.
struct improvementSettings
{
    bool bEnabled = true;
    // Review parts of herself on her own while idle, not only when a problem is recorded
    // or someone asks.
    bool bExplore = true;
    // Build and test each proposal in the workbench copy before reporting it.
    bool bVerify = true;
    // The estimated benefit (0..1) a proposal needs before she keeps it. Found on her own,
    // the bar is high; aimed at a measured problem, it is lower, because the problem is real.
    double minimumBenefit = 0.6;
    double evidenceMinimumBenefit = 0.4;
    // Her estimated chance (0..1) that the change breaks something.
    double maximumRisk = 0.5;
    int explorationIntervalMinutes = 120;
    // Quiet time before a build starts on its own. A build uses much of the machine, so it
    // waits longer than conversation-level background work, and stops when you return.
    int idleMinutesBeforeBuilding = 10;
    // New reviews pause while this many proven proposals wait for your decision.
    int maximumAwaitingDecision = 5;
    int windowLines = 220;
    // 0 = half the CPU cores.
    int buildJobs = 0;
    int buildTimeoutMinutes = 120;
    // Empty = %LOCALAPPDATA%/Revia/ImprovementWorkbench, outside any synced folder.
    std::string workbenchPath;
    std::string proposalsPath = "RuntimeData/Improvement/Proposals";
};
