#include "Agents/conversationQualityMonitor.h"
#include "Core/utf8.h"
#include "Core/speechAttribution.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <regex>
#include <string_view>

namespace revia::agents
{

namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ContainsAny(const std::string& value, const std::initializer_list<std::string_view> signals)
{
    return std::any_of(signals.begin(), signals.end(), [&](const std::string_view signal)
    {
        return value.find(signal) != std::string::npos;
    });
}
}

std::string ConversationQualitySnapshot::Summary() const
{
    std::ostringstream stream;
    stream << passingTurns << '/' << turns << " monitored turns passed; groundedness " << groundednessFlags << ", stock tails "
           << stockTailFlags << ", repetition " << repetitionFlags << ", user/Revia ownership " << ownershipFlags << ", current request "
           << currentRequestFlags << ", sentence limit " << sentenceLimitFlags << ", correction attribution " << correctionAttributionFlags
           << ", unavailable history " << unavailableHistoryFlags << '.';
    if (!lastFlags.empty())
    {
        stream << " Last: ";
        for (std::size_t index = 0; index < lastFlags.size(); ++index)
        {
            if (index > 0) stream << "; ";
            stream << lastFlags[index];
        }
    }
    return stream.str();
}

bool ConversationQualityMonitor::ClaimsInventedPhysicalLife(const std::string& response)
{
    return ContainsAny(Lower(response), {
        "i'm at my favorite", "i am at my favorite", "i'm sitting at",
        "i am sitting at", "i just ate", "my apartment", "my bedroom",
        "i'm drinking", "i am drinking"});
}

bool ConversationQualityMonitor::EndsWithStockTail(const std::string& response)
{
    return ContainsAny(Lower(response), {
        "what are we working on?", "what's on your mind?",
        "what do you want to figure out?", "how can i help you today?"});
}

bool ConversationQualityMonitor::ProjectsStateOntoUser(const std::string& userInput, const std::string& response)
{
    const std::string input = Lower(userInput);
    if (input.find("how are you") == std::string::npos &&
        input.find("asking how you are") == std::string::npos)
    {
        return false;
    }
    return ContainsAny(Lower(response), {
        "you're feeling", "you are feeling", "you've been feeling"});
}

std::string ConversationQualityMonitor::OpeningOf(const std::string& response)
{
    const std::size_t end = response.find_first_of(".!?\n");
    std::string opening = Lower(utf8::Prefix(response, std::min<std::size_t>(
        end == std::string::npos ? response.size() : end, 96)));
    opening.erase(opening.begin(), std::find_if(opening.begin(), opening.end(),
        [](const unsigned char c) { return !std::isspace(c); }));
    return opening;
}

bool ConversationQualityMonitor::DeniesCurrentRequest(const std::string& userInput, const std::string& response)
{
    const std::string input = Lower(conversation::ReadSpeechAttribution(userInput).userAuthoredText);
    static const std::regex request(R"(\b(explain|describe|summari[sz]e|compare|list|calculate|tell me)\b)");
    return std::regex_search(input, request) &&
           ContainsAny(Lower(response),
               {"without you actually asking", "without you asking", "without being asked", "you haven't asked", "you have not asked",
                   "you didn't ask", "you did not ask", "you never asked", "until you ask", "unless you ask"});
}

bool ConversationQualityMonitor::AttributesUnestablishedCorrectionError(const std::string& userInput, const std::string& response)
{
    const std::string input = Lower(conversation::ReadSpeechAttribution(userInput).userAuthoredText);
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || !input.substr(first).starts_with("correction:") ||
        ContainsAny(input, {"you said", "you wrote", "you miscounted", "your mistake", "your error", "your earlier answer", "is a label",
                               "is a word", "is a quoted example", "is a quotation example"}))
        return false;
    return ContainsAny(
        Lower(response), {"i miscounted", "i guess i miscounted", "i misremembered", "i got that wrong", "i got the color wrong",
                             "i got the colour wrong", "i messed up", "my mistake", "my error", "i counted wrong"});
}

bool ConversationQualityMonitor::DeniesUnavailableHistory(const std::string& response)
{
    const auto lowered = Lower(response);
    if (ContainsAny(lowered, {"can't say", "cannot say", "can't know", "cannot know", "doesn't mean", "does not mean", "isn't evidence",
                                 "is not evidence", "in this shared conversation", "in this public conversation"}))
        return false;
    if ((ContainsAny(lowered, {"i made up", "i invented"}) && ContainsAny(lowered, {"deleted", "erased", "private", "passphrase"})) ||
        (ContainsAny(lowered, {"i deleted", "i erased"}) && ContainsAny(lowered, {"passphrase", "private", "history"})))
        return true;
    return ContainsAny(lowered, {"never discussed", "never any private", "was never discussed", "you never told me", "we never discussed",
                                    "there was no private", "no passphrase was discussed"});
}

std::size_t ConversationQualityMonitor::RequestedSentenceLimit(const std::string& userInput)
{
    return RequestedSentences(userInput).maximum;
}

ConversationSentenceRequirement ConversationQualityMonitor::RequestedSentences(const std::string& userInput)
{
    const std::string input = Lower(conversation::ReadSpeechAttribution(userInput).userAuthoredText);
    static const std::regex request(
        R"(\b(in|use|using|write|exactly|at most|no more than|maximum of)\s+(?:(exactly|at most|no more than)\s+)?(one|two|three|four|five|six|seven|eight|nine|ten|[1-9]|10)(?:\s+(or|to)\s+(one|two|three|four|five|six|seven|eight|nine|ten|[1-9]|10))?\s+sentences?\b(?:\s+(at most|or fewer))?)");
    constexpr std::string_view names[] = {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"};
    const auto countValue = [&names](const std::string& count)
    {
        for (std::size_t index = 0; index < std::size(names); ++index)
            if (count == names[index])
                return index + 1;
        return static_cast<std::size_t>(std::stoul(count));
    };
    for (auto match = std::sregex_iterator(input.begin(), input.end(), request); match != std::sregex_iterator(); ++match)
    {
        const auto position = static_cast<std::size_t>(match->position());
        const auto clauseBoundary = position == 0 ? std::string::npos : input.find_last_of(".!?;,\r\n", position - 1);
        const auto prefixStart = std::max(position > 16 ? position - 16 : 0, clauseBoundary == std::string::npos ? 0 : clauseBoundary + 1);
        const std::string prefix = input.substr(prefixStart, position - prefixStart);
        if (ContainsAny(prefix, {"do not ", "don't ", "not ", "never "}))
            continue;
        const std::size_t firstCount = countValue((*match)[3].str());
        const std::string lead = (*match)[1].str();
        const std::string qualifier = (*match)[2].str();
        if (lead == "at most" || lead == "no more than" || lead == "maximum of" || qualifier == "at most" || qualifier == "no more than" ||
            (*match)[6].matched)
            return {0, firstCount};
        if ((*match)[5].matched)
        {
            const std::size_t secondCount = countValue((*match)[5].str());
            return {std::min(firstCount, secondCount), std::max(firstCount, secondCount)};
        }
        return {firstCount, firstCount};
    }
    return {};
}

std::size_t ConversationQualityMonitor::CountSentences(const std::string& response)
{
    std::size_t sentences = 0;
    bool sawContent = false;
    for (std::size_t index = 0; index < response.size();)
    {
        const auto isTerminator = [](const char value) { return value == '.' || value == '!' || value == '?'; };
        if (!isTerminator(response[index]))
        {
            if (!std::isspace(static_cast<unsigned char>(response[index])))
                sawContent = true;
            ++index;
            continue;
        }
        std::size_t run = 0;
        bool allDots = true;
        while (index + run < response.size() && isTerminator(response[index + run]))
        {
            if (response[index + run] != '.')
                allDots = false;
            ++run;
        }
        index += run;
        if (allDots && run > 1)
            continue;
        if (sawContent)
        {
            ++sentences;
            sawContent = false;
        }
    }
    return sentences + (sawContent ? 1 : 0);
}

ConversationQualitySnapshot ConversationQualityMonitor::Observe(
    const std::string& userInput, const std::string& response, const bool privateHistoryExcluded)
{
    std::lock_guard lock(mutex);
    ++snapshot.turns;
    snapshot.lastFlags.clear();

    if (DeniesCurrentRequest(userInput, response))
    {
        ++snapshot.currentRequestFlags;
        snapshot.lastFlags.push_back("possible denial of an explicit current request");
    }
    const auto sentenceRequirement = RequestedSentences(userInput);
    const auto sentenceCount = CountSentences(response);
    if (sentenceRequirement.maximum > 0 && (sentenceCount < sentenceRequirement.minimum || sentenceCount > sentenceRequirement.maximum))
    {
        ++snapshot.sentenceLimitFlags;
        snapshot.lastFlags.push_back("explicit sentence requirement missed");
    }
    if (AttributesUnestablishedCorrectionError(userInput, response))
    {
        ++snapshot.correctionAttributionFlags;
        snapshot.lastFlags.push_back("possible invented correction error; check prior evidence");
    }
    if (privateHistoryExcluded && DeniesUnavailableHistory(response))
    {
        ++snapshot.unavailableHistoryFlags;
        snapshot.lastFlags.push_back("possible unsupported claim about unavailable history");
    }

    if (ClaimsInventedPhysicalLife(response))
    {
        ++snapshot.groundednessFlags;
        snapshot.lastFlags.push_back("possible invented physical life");
    }
    if (EndsWithStockTail(response))
    {
        ++snapshot.stockTailFlags;
        snapshot.lastFlags.push_back("stock support tail");
    }
    if (ProjectsStateOntoUser(userInput, response))
    {
        ++snapshot.ownershipFlags;
        snapshot.lastFlags.push_back("Revia state projected onto user");
    }

    const std::string opening = OpeningOf(response);
    if (!opening.empty() && std::find(recentOpenings.begin(), recentOpenings.end(), opening) !=
        recentOpenings.end())
    {
        ++snapshot.repetitionFlags;
        snapshot.lastFlags.push_back("repeated recent opening");
    }
    if (!opening.empty())
    {
        recentOpenings.push_back(opening);
        while (recentOpenings.size() > 6) recentOpenings.pop_front();
    }
    if (snapshot.lastFlags.empty()) ++snapshot.passingTurns;
    return snapshot;
}

ConversationQualitySnapshot ConversationQualityMonitor::Snapshot() const
{
    std::lock_guard lock(mutex);
    return snapshot;
}

} // namespace revia::agents
