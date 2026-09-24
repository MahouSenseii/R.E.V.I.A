#include "Runtime/outputChannelPolicy.h"

#include <algorithm>
#include <cctype>

namespace revia::runtime
{

namespace
{
    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

    bool Listed(const std::vector<std::string>& list, const std::string& lowered)
    {
        return std::any_of(list.begin(), list.end(),
            [&lowered](const std::string& entry)
            {
                return Lower(entry) == lowered;
            });
    }
}

std::string ToString(const ChannelSpeechReason reason)
{
    switch (reason)
    {
        case ChannelSpeechReason::LocalConversation: return "local_conversation";
        case ChannelSpeechReason::VoiceEnabledApplication: return "voice_enabled_policy";
        case ChannelSpeechReason::TextOnlyApplication: return "text_only_policy";
        case ChannelSpeechReason::UnclassifiedApplication: return "unclassified_default";
    }
    return "unclassified_default";
}

ChannelPolicy ResolveOutputChannel(
    const outputChannel channel,
    const std::string& application,
    const conversationChannelSettings& settings)
{
    ChannelPolicy policy;
    if (channel == outputChannel::LocalVoice)
    {
        policy.speak = true;
        policy.reason = ChannelSpeechReason::LocalConversation;
        policy.status = "Talking locally. Replies are spoken when voice is enabled.";
        policy.logLine = "[Channel] target=LocalVoice";
        return policy;
    }

    const std::string named = application.empty() ? "another application" : application;
    const std::string lowered = Lower(application);

    // Checked first. An explicit opt-in is the more specific statement, and it is the
    // only thing that can lift the ordinary silence of composing into another window.
    if (!application.empty() && Listed(settings.voiceEnabledApplications, lowered))
    {
        policy.speak = true;
        policy.reason = ChannelSpeechReason::VoiceEnabledApplication;
        policy.status = "Composing into " + named +
            ". Voice is explicitly enabled for this application.";
    }
    else if (!application.empty() && Listed(settings.textOnlyApplications, lowered))
    {
        policy.speak = false;
        policy.reason = ChannelSpeechReason::TextOnlyApplication;
        policy.status = "Composing into " + named + ". Text only by application policy.";
    }
    else
    {
        policy.speak = false;
        policy.reason = ChannelSpeechReason::UnclassifiedApplication;
        policy.status = "Composing into " + named +
            ". Text only, because this application has not been opted into voice.";
    }

    // The application name is a diagnostic fact about where output went. What was
    // composed is never in here.
    policy.logLine = "[Channel] target=ExternalApplication app=" +
        (application.empty() ? std::string("unknown") : application) +
        " speak=" + (policy.speak ? "yes" : "no") +
        " reason=" + ToString(policy.reason);
    return policy;
}

} // namespace revia::runtime
