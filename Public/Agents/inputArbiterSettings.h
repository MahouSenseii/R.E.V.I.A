#pragma once

#include <string>
#include <vector>

// Merging and filtering what arrives, instead of answering every fragment separately.
struct inputArbiterSettings
{
    // Inputs landing inside this window are treated as one thought. Speaking in three
    // bursts should not produce three replies.
    int mergeWindowMs = 350;
    // Below this, a fragment is treated as noise unless it is clearly addressed to Revia.
    int minimumMeaningfulCharacters = 3;
    int maxQueuedInputs = 8;
    // Recognisers emit these constantly from room noise. They are dropped rather than
    // answered.
    std::vector<std::string> ignoredFragments = {
        "uh", "um", "erm", "hmm", "mm", "mhm", "ah", "oh", "eh", "huh",
        "you", "thanks for watching", "thank you", "[blank_audio]", "..."
    };
};
