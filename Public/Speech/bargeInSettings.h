#pragma once

// Detect interruption against a rolling noise floor to reject speaker echo.
struct bargeInSettings
{
    bool bEnabled = true;
    // Absolute floor. Nothing below this is ever an interruption regardless of how quiet
    // the room is, so a silent microphone cannot produce a hair trigger.
    int energyThreshold = 1400;
    // How far above the learned floor a frame must sit to count. Speech arrives on top of
    // the echo, so a genuine interruption is a step change, not a slow drift.
    float echoMarginMultiplier = 2.6f;
    // Consecutive qualifying frames before Revia yields, so one cough, a door, or a burst
    // of laughter from the speakers does not cut a reply short. Frames are ~50 ms.
    int consecutiveFramesRequired = 8;
    // Time to learn the floor before any interruption is possible. Must be long enough to
    // capture what Revia's own playback sounds like through the microphone.
    int startupGraceMs = 700;
};
