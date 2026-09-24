#pragma once

#include "Library/structLibrary.h"

#include <string>
#include <vector>

namespace revia::conversation
{
enum class QuotedSpeaker { Unknown, User, OtherPerson, Revia };
enum class QuoteRecipient { Unknown, User, Revia };

struct ReportedSpeech
{
    std::size_t begin = 0;
    std::size_t end = 0;
    QuotedSpeaker speaker = QuotedSpeaker::Unknown;
    QuoteRecipient recipient = QuoteRecipient::Unknown;
};

// Conservative evidence from explicit reporting phrases and quote boundaries. This
// does not guess identities or rewrite pronouns inside someone else's words.
struct SpeechAttribution
{
    std::vector<ReportedSpeech> quotes;
    std::string userAuthoredText;
    bool refersToOtherPerson = false;
    bool correctsAttribution = false;
    bool requestsDirectReply = false;
};

[[nodiscard]] SpeechAttribution ReadSpeechAttribution(const std::string& input);
[[nodiscard]] std::string AnnotateReportedSpeech(const std::string& input);
[[nodiscard]] std::string BuildSpeechAttributionGuidance(
    const std::string& input, const std::vector<conversationMessage>& context);
}
