#include "Attachments/videoCoverage.h"

#include <algorithm>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <string_view>

namespace revia::attachments
{
namespace
{
bool Any(const std::string& words, const std::initializer_list<std::string_view> choices)
{
    return std::any_of(choices.begin(), choices.end(), [&](const auto choice) { return words.find(choice) != std::string::npos; });
}

std::string QuestionWords(const std::string& input)
{
    if (input.empty() || input.size() > 1024 || input.find('"') != std::string::npos ||
        input.find(static_cast<char>(96)) != std::string::npos || input.find("\xE2\x80\x9C") != std::string::npos ||
        input.find("\xE2\x80\x9D") != std::string::npos || input.find("\xE2\x80\x98") != std::string::npos)
        return {};
    std::string words = " ";
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto byte = static_cast<unsigned char>(input[index]);
        if (byte == '\'')
        {
            const auto previous = index == 0 ? 0 : static_cast<unsigned char>(input[index - 1]);
            if (!((previous >= 'a' && previous <= 'z') || (previous >= 'A' && previous <= 'Z')))
                return {};
        }
        if (input.compare(index, 3, "\xE2\x80\x99") == 0)
            index += 2;
        else if (byte >= 'A' && byte <= 'Z')
        {
            words += static_cast<char>(byte + 'a' - 'A');
            continue;
        }
        else if (byte >= 'a' && byte <= 'z')
        {
            words += static_cast<char>(byte);
            continue;
        }
        else if (byte >= 0x80 || std::string_view(" \t\r\n?.,:;!-'/()").find(static_cast<char>(byte)) == std::string_view::npos)
            return {};
        if (words.back() != ' ')
            words += ' ';
    }
    if (words.back() != ' ')
        words += ' ';
    return words;
}

bool PureTemporalQuestion(const std::string& input)
{
    auto words = QuestionWords(input);
    if (words.starts_with(" please "))
        words.erase(0, 7);
    if (words.empty() || words.starts_with(" do not "))
        return false;
    const bool question = words.starts_with(" does ") || words.starts_with(" do ") || words.starts_with(" did ") ||
                          words.starts_with(" is ") || words.starts_with(" are ") || words.starts_with(" was ") ||
                          words.starts_with(" were ") || words.starts_with(" can ") || words.starts_with(" could ") ||
                          words.starts_with(" describe whether ") || words.starts_with(" tell me whether ") ||
                          words.starts_with(" what can ") || words.starts_with(" what cannot ") || words.starts_with(" how confident ") ||
                          words.starts_with(" how certain ");
    const bool feet = Any(words, {" foot ", " feet "}) &&
                      Any(words, {" slide ", " slides ", " sliding ", " slid ", " slip ", " slips ", " slipping ", " slippage "});
    const bool motion =
        Any(words, {" motion ", " movement "}) &&
        Any(words, {" continuous ", " continuity ", " smooth ", " smoothly ", " smoothness ", " abrupt ", " abruptly ", " discontinuous ",
                       " conclude ", " determine ", " establish ", " prove ", " confirm ", " support ", " demonstrate "});
    if (!question || (!feet && !motion))
        return false;

    // A closed vocabulary declines mixed visual tasks instead of swallowing their factual requests.
    const std::string_view allowed =
        " describe tell whether if from these those the this that samples sampled frames frame still stills video clip during in on at "
        "around between before after throughout movement motion continuous continuity smooth smoothly smoothness abrupt abruptly "
        "discontinuous foot feet character characters s slide slides sliding slid slip slips slipping slippage does do did is are was "
        "were any a an or and not cannot can determine establish show prove confirm support demonstrate conclude conclusions state "
        "what say confidence confident confidently certain certainty actually really it they their his her its there evidence enough "
        "sufficient "
        "to be been have has happened happen occurs occur occurring could you we i me please with without about of observed visible how "
        "much ";
    for (std::size_t start = 1; start < words.size();)
    {
        const auto end = words.find(' ', start);
        if (end == std::string::npos)
            break;
        if (allowed.find(words.substr(start - 1, end - start + 2)) == std::string_view::npos)
            return false;
        start = end + 1;
    }
    return true;
}

std::string Timestamp(const std::int64_t milliseconds)
{
    std::ostringstream text;
    text << std::setfill('0') << std::setw(2) << milliseconds / 60000 << ':' << std::setw(2) << milliseconds / 1000 % 60 << '.'
         << std::setw(3) << milliseconds % 1000;
    return text.str();
}
}

std::string VideoSamplingLimitation(const std::string& input, const std::vector<std::int64_t>& timestampsMs)
{
    if (timestampsMs.empty() || timestampsMs.size() > 4)
        return {};
    std::int64_t largestGap = 0;
    for (std::size_t index = 0; index < timestampsMs.size(); ++index)
    {
        if (timestampsMs[index] < 0 || timestampsMs[index] > 60000 || (index > 0 && timestampsMs[index] <= timestampsMs[index - 1]))
            return {};
        if (index > 0)
            largestGap = std::max(largestGap, timestampsMs[index] - timestampsMs[index - 1]);
    }
    if ((timestampsMs.size() > 1 && largestGap <= 100) || !PureTemporalQuestion(input))
        return {};
    std::string reply;
    if (timestampsMs.size() == 1)
        reply = "Only one video frame is available, at " + Timestamp(timestampsMs.front()) + ". ";
    else
    {
        reply = "The available video samples are at ";
        for (std::size_t index = 0; index < timestampsMs.size(); ++index)
            reply += (index == 0 ? "" : ", ") + Timestamp(timestampsMs[index]);
        reply += " (largest gap: " + std::to_string(largestGap) + " ms). ";
    }
    return reply + "These still samples cannot determine continuous motion or foot sliding between frames. "
                   "Ask for a narrower interval for denser sampling.";
}

}
