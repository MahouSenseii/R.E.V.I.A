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

// Where a transcript goes, given how the user has set things up.
//
// Extracted from the Qt event handler so it can be tested without an event loop, a
// microphone, or whisper.cpp. The decision is small but it is the last step of the
// voice path, and it is where "I spoke and nothing happened" and "I spoke and it sent
// something I wanted to edit first" both live.
//
// `busy` is deliberately a reason to hold rather than to drop: a transcript that
// arrived while Revia was mid-reply is still what the user said, and discarding it
// would lose speech the person has no way to get back. Holding it in the box lets them
// send it when she is done.
[[nodiscard]] constexpr TranscriptRouting DecideTranscriptRouting(
    const bool handsFree,
    const bool transcriptEmpty,
    const bool autoSendEnabled,
    const bool busy)
{
    if (transcriptEmpty) return TranscriptRouting::Ignore;
    if (handsFree) return TranscriptRouting::HandsFreeAlreadySubmitted;
    if (autoSendEnabled && !busy) return TranscriptRouting::FillAndSend;
    return TranscriptRouting::FillAndHold;
}

} // namespace revia::speech
