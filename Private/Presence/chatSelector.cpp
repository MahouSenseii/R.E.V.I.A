#include "Presence/chatSelector.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace revia::presence
{

namespace
{
std::string AuthorKey(const ExternalAdapterEvent& event)
{
    return event.source + ":" + (event.authorId.empty() ? event.author : event.authorId);
}

// The people whose messages are answered first, and the supporters who are always
// acknowledged: a paid event ignored on air is worse than a dull reply.
double RoleWeight(const std::string& role)
{
    if (role == "owner" || role == "operator" || role == "broadcaster") return 3.0;
    if (role == "moderator" || role == "mod") return 1.5;
    if (role == "supporter" || role == "subscriber" || role == "vip" || role == "member")
    {
        return 1.0;
    }
    return 0.0;
}

bool IsSupporter(const std::string& role)
{
    return role == "supporter" || role == "subscriber" || role == "vip" || role == "member";
}
} // namespace

ChatSelector::ChatSelector(const presenceSettings& settings)
{
    Configure(settings);
}

void ChatSelector::Configure(const presenceSettings& settings)
{
    capacity = static_cast<std::size_t>(std::max(4, settings.chatQueueCapacity));
    maximumAge = std::chrono::seconds(std::max(5, settings.chatMaximumAgeSeconds));
    authorCooldown = std::chrono::seconds(std::max(0, settings.chatAuthorCooldownSeconds));
    talkativeness = std::clamp(settings.streamTalkativeness, 0.0F, 1.0F);
}

std::string ChatSelector::Normalised(const std::string& text)
{
    std::string out;
    for (const char value : text)
    {
        if (std::isalnum(static_cast<unsigned char>(value)))
        {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
        }
        else if (static_cast<unsigned char>(value) >= 0x80)
        {
            out += value;
        }
    }
    return out;
}

void ChatSelector::Forget(const Clock::time_point now)
{
    while (!answered.empty() && now - answered.front().at > std::chrono::minutes(2))
    {
        answered.pop_front();
    }
    std::erase_if(waiting, [&](const Candidate& candidate)
    {
        return now - candidate.arrived > maximumAge;
    });
}

int ChatSelector::RepliesPerMinute() const
{
    // Quiet at 0: two a minute, answering only what is aimed at her. Chatty at 1: a
    // reply every five seconds, which is as fast as her voice can keep up.
    return 2 + static_cast<int>(std::lround(10.0F * talkativeness));
}

ChatCandidateScore ChatSelector::Score(
    const ExternalAdapterEvent& event,
    const Clock::time_point arrived,
    const Clock::time_point now) const
{
    ChatCandidateScore result;
    result.score = 1.0;
    if (event.addressedToRevia)
    {
        result.score += 2.0;
        result.because.push_back("addressed to her");
    }
    const double role = RoleWeight(event.role);
    if (role > 0.0)
    {
        result.score += role;
        result.because.push_back(event.role);
    }
    if (event.source != "stream")
    {
        // A Discord channel or a game speaks to her directly; a stream's chat speaks
        // to the room.
        result.score += 1.0;
        result.because.push_back("direct channel");
    }
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - arrived).count();
    if (age > 0)
    {
        // Waiting counts for a little, so nobody is starved forever by louder people.
        result.score += std::min(1.0, 0.02 * static_cast<double>(age));
    }
    const std::string author = AuthorKey(event);
    std::size_t recentFromAuthor = 0;
    for (auto it = answered.rbegin(); it != answered.rend(); ++it)
    {
        if (it->authorKey == author && now - it->at < authorCooldown) ++recentFromAuthor;
    }
    if (recentFromAuthor > 0 && !IsSupporter(event.role))
    {
        result.score -= 1.5 * static_cast<double>(recentFromAuthor);
        result.because.push_back("just answered them");
    }
    if (!answered.empty() && answered.back().source == event.source && event.source == "stream")
    {
        // The other platform's turn, when it has anyone waiting.
        const bool otherWaiting = std::any_of(waiting.begin(), waiting.end(),
            [&](const Candidate& candidate) { return candidate.event.source != event.source; });
        if (otherWaiting)
        {
            result.score -= 0.3;
            result.because.push_back("another source waits");
        }
    }
    if (!event.addressedToRevia && RoleWeight(event.role) == 0.0 && event.source == "stream")
    {
        // Overheard chat: worth answering only when she is in a talkative mood.
        result.score -= 1.0 * (1.0 - static_cast<double>(talkativeness));
        result.because.push_back("overheard");
    }
    return result;
}

bool ChatSelector::Offer(ExternalAdapterEvent event, const Clock::time_point now,
    std::string& outReason)
{
    Forget(now);
    const std::string text = Normalised(event.text);
    if (text.empty())
    {
        outReason = "The message had no words.";
        return false;
    }
    if (event.source == "stream" && !event.addressedToRevia && RoleWeight(event.role) == 0.0 &&
        talkativeness < 0.5F)
    {
        outReason = "Overheard chat is not answered below talkativeness 0.5.";
        return false;
    }
    const bool repeatWaiting = std::any_of(waiting.begin(), waiting.end(),
        [&](const Candidate& candidate) { return Normalised(candidate.event.text) == text; });
    const bool repeatAnswered = std::any_of(answered.begin(), answered.end(),
        [&](const Answered& done) { return done.text == text; });
    if (repeatWaiting || repeatAnswered)
    {
        outReason = "The same line is already waiting or was just answered.";
        return false;
    }
    if (waiting.size() >= capacity)
    {
        // Full: the newcomer replaces the weakest candidate only when it outscores it.
        auto weakest = std::min_element(waiting.begin(), waiting.end(),
            [&](const Candidate& left, const Candidate& right)
            {
                return Score(left.event, left.arrived, now).score <
                    Score(right.event, right.arrived, now).score;
            });
        if (weakest == waiting.end() ||
            Score(event, now, now).score <= Score(weakest->event, weakest->arrived, now).score)
        {
            outReason = "The queue is full of messages that score higher.";
            return false;
        }
        waiting.erase(weakest);
    }
    waiting.push_back({std::move(event), now});
    return true;
}

std::optional<ExternalAdapterEvent> ChatSelector::Next(const Clock::time_point now)
{
    Forget(now);
    if (waiting.empty()) return std::nullopt;
    const int allowance = RepliesPerMinute();
    const auto lastMinute = std::count_if(answered.begin(), answered.end(),
        [&](const Answered& done) { return now - done.at < std::chrono::minutes(1); });
    // A supporter is acknowledged even when the allowance is spent.
    const bool supporterWaiting = std::any_of(waiting.begin(), waiting.end(),
        [](const Candidate& candidate) { return IsSupporter(candidate.event.role); });
    if (lastMinute >= allowance && !supporterWaiting) return std::nullopt;

    // Score everything, then choose among the near-best with a little randomness.
    std::vector<std::pair<double, std::size_t>> scored;
    scored.reserve(waiting.size());
    for (std::size_t index = 0; index < waiting.size(); ++index)
    {
        const Candidate& candidate = waiting[index];
        double score = Score(candidate.event, candidate.arrived, now).score;
        if (lastMinute >= allowance && !IsSupporter(candidate.event.role)) score -= 100.0;
        if (jitter > 0.0)
        {
            // xorshift: enough randomness for a queue, no <random> state to seed.
            randomState ^= randomState << 13;
            randomState ^= randomState >> 17;
            randomState ^= randomState << 5;
            const double unit = static_cast<double>(randomState % 10000U) / 10000.0;
            score += (unit * 2.0 - 1.0) * jitter;
        }
        scored.emplace_back(score, index);
    }
    const auto best = std::max_element(scored.begin(), scored.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });
    if (best->first < -50.0) return std::nullopt;
    Candidate chosen = std::move(waiting[best->second]);
    waiting.erase(waiting.begin() + static_cast<std::ptrdiff_t>(best->second));
    answered.push_back({AuthorKey(chosen.event), chosen.event.source,
        Normalised(chosen.event.text), now});
    while (answered.size() > 64) answered.pop_front();
    return chosen.event;
}

void ChatSelector::Clear()
{
    waiting.clear();
}

std::string ChatSelector::Describe(const Clock::time_point now) const
{
    const auto lastMinute = std::count_if(answered.begin(), answered.end(),
        [&](const Answered& done) { return now - done.at < std::chrono::minutes(1); });
    std::ostringstream stream;
    stream << waiting.size() << " waiting; " << lastMinute << " answered in the last minute of "
           << RepliesPerMinute() << " allowed; talkativeness " << talkativeness;
    return stream.str();
}

} // namespace revia::presence
