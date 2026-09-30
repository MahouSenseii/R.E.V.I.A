#pragma once

namespace revia::speech
{

// What the shell does with a finished transcript.
enum class TranscriptRouting
{
    // Nothing usable came back. The cycle ends without touching the message box, so a
    // failed transcription never clears something the user had already typed.
    Ignore,
    // Hands-free already submitted it as a turn on its own. The shell only returns the
    // button to its listening state.
    HandsFreeAlreadySubmitted,
    // Put it in the message box and send it.
    FillAndSend,
    // Put it in the message box and leave it for editing.
    FillAndHold
};

// Routes transcripts according to user settings; busy holds the text for review rather than discarding it.
[[nodiscard]] constexpr TranscriptRouting DecideTranscriptRouting(const bool handsFree,
    const bool transcriptEmpty, const bool autoSendEnabled, const bool busy)
{
    if (transcriptEmpty) return TranscriptRouting::Ignore;
    if (handsFree) return TranscriptRouting::HandsFreeAlreadySubmitted;
    if (autoSendEnabled && !busy) return TranscriptRouting::FillAndSend;
    return TranscriptRouting::FillAndHold;
}

} // namespace revia::speech
