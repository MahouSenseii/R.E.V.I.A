#include "Agents/replyStreamGate.h"

#include "Agents/replyFragmenter.h"
#include "Agents/responseFilter.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace revia::agents
{

ReplyStreamGate::ReplyStreamGate(Filter inputFilter)
    : filter(std::move(inputFilter))
{
}

std::string ReplyStreamGate::Consume(const std::string& delta)
{
    if (halted || delta.empty() || !filter)
        return {};
    raw += delta;
    const std::size_t start = raw.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};

    // Sentence ends not judged before, oldest first. A judged end is followed by the
    // whitespace that confirmed it, and no sentence's punctuation runs across whitespace,
    // so every new end begins after it. An ellipsis reports one end for each of its dots.
    std::vector<std::size_t> ends;
    for (std::size_t index = std::max(start, judgedThrough); index < raw.size(); ++index)
    {
        const std::size_t end = ReplyFragmenter::SentenceEnd(raw, index);
        if (end != std::string::npos && end > judgedThrough && (ends.empty() || end > ends.back()))
            ends.push_back(end);
    }
    if (ends.empty())
        return {};
    judgedThrough = ends.back();

    // The longest prefix the filters leave alone is released. A shorter one is tried
    // when a longer one is edited: the reply's last sentence is where tail repairs land.
    for (auto end = ends.rbegin(); end != ends.rend(); ++end)
    {
        const std::string candidate = raw.substr(start, *end - start);
        if (candidate.size() <= released.size())
            break;
        const StreamGuard guard = GuardStreamedPrefix(candidate);
        if (guard.blocked)
        {
            halted = true;
            return {};
        }
        if (guard.holdTail)
            continue;
        const std::optional<std::string> filtered = filter(candidate);
        if (!filtered)
        {
            halted = true;
            return {};
        }
        if (*filtered != candidate || candidate.compare(0, released.size(), released) != 0)
            continue;
        std::string approved = candidate.substr(released.size());
        released = candidate;
        return approved;
    }
    return {};
}

const std::string& ReplyStreamGate::Released() const
{
    return released;
}

bool ReplyStreamGate::Halted() const
{
    return halted;
}

} // namespace revia::agents
