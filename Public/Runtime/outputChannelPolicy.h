#pragma once

#include "Library/structLibrary.h"

#include <string>

namespace revia::runtime
{

// Why a reply was or was not spoken on the current channel.
//
// Named rather than implied, because "silent" has three different causes here and they
// call for different things: a browser is silent by declared policy, an unrecognised
// application is silent because the safe default is silence, and a local conversation
// is only silent if voice itself is off.
enum class ChannelSpeechReason
{
    LocalConversation,
    // The executable is listed in voiceEnabledApplications: an explicit opt-in that
    // overrides the ordinary silence of composing into somebody else's window.
    VoiceEnabledApplication,
    // The executable is listed in textOnlyApplications. Same behaviour as the default
    // below, but a declared classification rather than an absence of one -- which is
    // what lets the status line say why instead of guessing.
    TextOnlyApplication,
    // Composing into an application nobody has classified. Silence is the safe default:
    // narrating text into an unknown window is the failure that cannot be undone.
    UnclassifiedApplication
};

struct ChannelPolicy
{
    bool speak = true;
    ChannelSpeechReason reason = ChannelSpeechReason::LocalConversation;
    // What the shell shows. Reflects the resolved policy, including the case where an
    // application was explicitly opted into voice.
    std::string status;
    // One diagnostic line. Never contains the text being composed.
    std::string logLine;
};

[[nodiscard]] std::string ToString(ChannelSpeechReason reason);

// Resolves whether Revia speaks, given where her output is going.
//
// Pure so the policy can be tested without a window manager, an action, or a voice.
// Precedence is deliberate: an explicit voice opt-in beats a text-only listing, because
// the opt-in is the more specific statement and a user who put an application in both
// lists most recently meant to hear it.
[[nodiscard]] ChannelPolicy ResolveOutputChannel(
    outputChannel channel,
    const std::string& application,
    const conversationChannelSettings& settings);

} // namespace revia::runtime
