#pragma once

#include <string>

// Decision provider and opt-in recording settings grant no authority.
// Every proposed step retains the existing validation, policy, confirmation and audit boundary.
struct computerControlSettings
{
    // "legacy", "shadow", "assisted" or "learned"; unknown values fall back to legacy.
    std::string providerMode = "legacy";
    // How many times a cheaper provider may hand a run back to the model before the run
    // stops paying for the attempt.
    int escalationBudget = 3;
    // The experience recorder. Off, and separately off from the mode: comparing
    // providers and keeping a dataset are different things wanting different consent.
    bool bRecordingEnabled = false;
    // Where an opt-in dataset is written, relative to the runtime data directory.
    std::string datasetDirectory = "ComputerExperience";
    // Structural metadata by default; "control_values" separately opts in to captured content.
    std::string captureDepth = "structure";
    // A learned artifact, when one has been qualified. Empty leaves learned mode
    // inactive rather than loading whatever is lying around.
    std::string learnedArtifactPath;
};
