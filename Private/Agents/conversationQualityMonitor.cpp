#include "Agents/conversationQualityMonitor.h"

#include <algorithm>
#include <cctype>
#include <sstream>
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

std::string Opening(const std::string& response)
{
    const std::size_t end = response.find_first_of(".!?\n");
    std::string opening = Lower(response.substr(0, std::min<std::size_t>(
        end == std::string::npos ? response.size() : end, 96)));
    opening.erase(opening.begin(), std::find_if(opening.begin(), opening.end(),
        [](const unsigned char c) { return !std::isspace(c); }));
    return opening;
}
}

std::string ConversationQualitySnapshot::Summary() const
{
    std::ostringstream stream;
    stream << passingTurns << '/' << turns << " monitored turns passed; groundedness "
        << groundednessFlags << ", stock tails " << stockTailFlags << ", repetition "
        << repetitionFlags << ", user/Revia ownership " << ownershipFlags << '.';
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

ConversationQualitySnapshot ConversationQualityMonitor::Observe(
    const std::string& userInput,
    const std::string& response)
{
    std::lock_guard lock(mutex);
    ++snapshot.turns;
    snapshot.lastFlags.clear();
    const std::string input = Lower(userInput);
    const std::string reply = Lower(response);

    if (ContainsAny(reply, {
            "i'm at my favorite", "i am at my favorite", "i'm sitting at",
            "i am sitting at", "i just ate", "my apartment", "my bedroom",
            "i'm drinking", "i am drinking"}))
    {
        ++snapshot.groundednessFlags;
        snapshot.lastFlags.push_back("possible invented physical life");
    }
    if (ContainsAny(reply, {
            "what are we working on?", "what's on your mind?",
            "what do you want to figure out?", "how can i help you today?"}))
    {
        ++snapshot.stockTailFlags;
        snapshot.lastFlags.push_back("stock support tail");
    }
    if ((input.find("how are you") != std::string::npos ||
            input.find("asking how you are") != std::string::npos) &&
        ContainsAny(reply, {"you're feeling", "you are feeling", "you've been feeling"}))
    {
        ++snapshot.ownershipFlags;
        snapshot.lastFlags.push_back("Revia state projected onto user");
    }

    const std::string opening = Opening(response);
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
