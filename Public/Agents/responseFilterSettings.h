#pragma once

// Hard grounding/structure checks always run; AI review is an optional latency preference.
// Personality does not control the output boundary.
struct responseFilterSettings
{
    bool bAiReviewEnabled = false;
    int aiMaxReviewTokens = 192;
    int maxReplyCharacters = 12000;
};
