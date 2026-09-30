#pragma once

// Evidence-based interruption thresholds and proposal backoff; silence is the default.
struct initiativeSettings
{
    bool bEnabled = false;
    // Curiosity nominates from evidence; timers create no topics or permission to interrupt.
    // AttentionPolicy retains the final gate.
    bool bCuriosityEnabled = true;
    bool bSpontaneousSpeechEnabled = true;
    bool bSpeakWhenUserAway = true;
    // Keep bounded grounded summaries and source URLs after permitted autonomous research.
    // Never store raw page bodies or private reasoning.
    bool bAutonomousLearningEnabled = false;
    int curiosityCheckSeconds = 30;
    int autonomousQuietSeconds = 45;
    int curiosityTopicCooldownMinutes = 1440;
    // Below this, Revia stays quiet no matter how relevant the observation looks.
    float minimumConfidence = 0.72f;
    int maxUtterancesPerHour = 4;
    int cooldownSeconds = 900;
    // Longer after a dismissal than after an accepted one. Being told "no" should cost
    // more than being ignored.
    int dismissalCooldownSeconds = 3600;
    // Do not interrupt someone mid-keystroke. Measured from the last input event of any
    // kind, which needs no keyboard hook and records nothing about what was typed.
    int quietInputSeconds = 4;
    // Proposals accepted versus dismissed. Below this, Revia halves its own rate. An
    // assistant that cannot tell it is being annoying is a defect.
    float minimumPrecision = 0.34f;
    int precisionSampleFloor = 5;
    bool bSuppressWhenFullScreen = true;
    // Time bounds evidence; a qualifying foreground transition wakes the initiative worker.
    int focusSessionMinutes = 12;
    int returnAfterMinutes = 20;
    int contextSwitchWindowSeconds = 300;
    int contextSwitchCount = 6;
    int cueMaxAgeMinutes = 10;
};
