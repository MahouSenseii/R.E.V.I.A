#pragma once

#include "Library/structLibrary.h"
#include "Presence/presenceRuntime.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace revia::presence
{

// Why a candidate scored what it did, for the log and for the operator's status line.
struct ChatCandidateScore
{
    double score = 0.0;
    std::vector<std::string> because;
};

// One queue for everything that arrives from outside -- a stream's chat, a Discord
// channel, a game -- and one answer to "who does she answer next".
//
// A first-in queue answered whoever typed fastest, which on a live chat is the same
// three people all night and never the moderator who asked a real question. This
// scores what is waiting instead: whether it addresses her, who sent it (the owner and
// moderators first, supporters always acknowledged), whether it is new rather than the
// same line again, how long it has waited, and fairness -- an author she just answered
// yields, and so does a source that has had the last word, so two platforms share her.
// A little randomness among the top candidates keeps her from being predictable, and
// a talkativeness setting decides how much unaddressed chat she picks up at all.
//
// Deterministic apart from the jitter, which a test can zero. It holds no runtime and
// never generates anything; the session asks it what to answer and answers it.
class ChatSelector
{
public:
    using Clock = std::chrono::steady_clock;

    ChatSelector() = default;
    explicit ChatSelector(const presenceSettings& settings);
    void Configure(const presenceSettings& settings);

    // Offers a message. False, with the reason, when it is not kept: a repeat of a
    // line already waiting or just answered, unaddressed chat below the talkativeness
    // threshold, or a full queue where it scores lower than everything waiting.
    bool Offer(ExternalAdapterEvent event, Clock::time_point now, std::string& outReason);

    // The best candidate now, removed from the queue and remembered as answered.
    // Empty when nothing is waiting, when everything waiting is stale, or when the
    // replies-per-minute allowance for this talkativeness is spent; the caller waits.
    [[nodiscard]] std::optional<ExternalAdapterEvent> Next(Clock::time_point now);

    // The score a message would get now, with its reasons. Public so a test can state
    // what fairness and priority mean in numbers, and so /stream status can show them.
    [[nodiscard]] ChatCandidateScore Score(
        const ExternalAdapterEvent& event, Clock::time_point arrived,
        Clock::time_point now) const;

    [[nodiscard]] std::size_t Pending() const { return waiting.size(); }
    void Clear();
    // How many replies this talkativeness allows per minute.
    [[nodiscard]] int RepliesPerMinute() const;
    [[nodiscard]] float Talkativeness() const { return talkativeness; }
    // "3 waiting; 2 answered in the last minute of 6 allowed; talkativeness 0.35".
    [[nodiscard]] std::string Describe(Clock::time_point now) const;

    // Zero for a deterministic order; the default keeps her a little unpredictable.
    void SetJitter(double amplitude) { jitter = amplitude; }

private:
    struct Candidate
    {
        ExternalAdapterEvent event;
        Clock::time_point arrived;
    };

    [[nodiscard]] static std::string Normalised(const std::string& text);
    void Forget(Clock::time_point now);

    std::size_t capacity = 64;
    std::chrono::seconds maximumAge{90};
    std::chrono::seconds authorCooldown{20};
    float talkativeness = 0.35F;
    double jitter = 0.25;
    std::uint32_t randomState = 0x9E3779B9U;

    std::deque<Candidate> waiting;
    // Who and what she answered lately, newest last.
    struct Answered
    {
        std::string authorKey;
        std::string source;
        std::string text;
        Clock::time_point at;
    };
    std::deque<Answered> answered;
};

} // namespace revia::presence
