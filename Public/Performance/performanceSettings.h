#pragma once

#include <string>

// Singing. Separate from speechSettings on purpose: a song is one continuous timeline
// with its own audio device, and nothing here may change how ordinary speech behaves.
struct performanceSettings
{
    bool bEnabled = true;
    std::string songLibraryPath = "RuntimeData/Songs";
    // One voice: she stops singing to answer rather than talking over herself. Turning
    // this off leaves the song playing and the reply on screen only.
    bool bInterruptSongToSpeak = true;
    int maxSongSeconds = 600;
    int outputBufferMs = 120;
    double instrumentalGain = 1.0;
    double vocalGain = 1.0;
};
