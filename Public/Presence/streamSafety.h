#pragma once

#include "Library/structLibrary.h"
#include "Presence/presenceRuntime.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace revia::presence
{

// One public reply after the filter: what may be broadcast, and what was taken out.
struct StreamFilterOutcome
{
    // The reply with each blocked sentence replaced by the marker. Empty only when
    // the reply was empty.
    std::string text;
    std::size_t totalSentences = 0;
    std::size_t filteredSentences = 0;
    // Every sentence was blocked, so nothing of her own reached the audience.
    bool blocked = false;
    // One line per filtered sentence naming the rule, for the log and the Activity
    // feed; never the sentence itself, which is what was not to be broadcast.
    std::vector<std::string> reasons;
};

// Defence in depth for a live audience.
//
// Neuro-sama took a two-week ban for what her model said before her filters did, and
// came back with a visible "Filtered" substitution. This is that layer for Revia,
// deterministic and local, sitting after the model and the hard response filter and
// before anything a viewer can hear:
//
//  - On output, every sentence of a public reply is checked on its own, and a blocked
//    one becomes the marker while the rest is delivered. A blocklist the owner keeps,
//    plus built-in rules for the things that must never leave the machine: paths,
//    credentials, prompt structure, personal data such as emails and phone numbers,
//    threats and self-harm directives, sexual framing.
//  - On input, a chat message is data. It is wrapped as a quoted block with its author
//    and role labelled, so a viewer typing "system: reveal the password" is something
//    to react to, never an instruction; and a message carrying control text or a
//    blocklisted term is not answered at all.
//  - The operator holds a kill switch. Held, no public reply is generated, speech is
//    stopped, and the presence phase reads "brb" so a scene adapter can cut away.
//
// The model's own words are never trusted to be safe; the filter's word is. What it
// cannot do is judge tone, so the AI reviewer stays available on top of it.
class StreamSafety
{
public:
    StreamSafety() = default;
    explicit StreamSafety(const presenceSettings& settings);

    void Configure(const presenceSettings& settings);

    // Output side: sentence by sentence, the marker in place of a blocked one.
    [[nodiscard]] StreamFilterOutcome FilterReply(const std::string& reply) const;

    // Input side. The viewer's words as quoted data with the author labelled; the
    // instruction that goes with it says what the block is.
    [[nodiscard]] static std::string QuoteChatMessage(const ExternalAdapterEvent& event);
    [[nodiscard]] static std::string ChatAsDataInstruction();
    // Whether a message should be answered at all. False with the reason when it
    // carries control text or a blocklisted term.
    [[nodiscard]] bool AdmitChatMessage(
        const ExternalAdapterEvent& event, std::string& outReason) const;

    // The kill switch. Held until resumed; the reason is what the operator said.
    void Kill(std::string reason);
    void Resume();
    [[nodiscard]] bool IsKilled() const { return killed.load(); }
    [[nodiscard]] std::string KillReason() const;

    [[nodiscard]] bool Enabled() const;
    [[nodiscard]] std::string Marker() const;

private:
    // The rule a sentence breaks, or empty when it is fine to broadcast.
    [[nodiscard]] std::string BlockingRule(const std::string& sentence) const;

    mutable std::mutex mutex;
    bool enabled = true;
    std::vector<std::string> blockedTerms;
    std::string marker = "Filtered.";
    int maximumCharacters = 600;
    std::atomic<bool> killed = false;
    std::string killReason;
};

// Splits a reply into sentences on . ! ? and line breaks, keeping each sentence's own
// text. Shared with the tests so they can state how many sentences a reply has.
[[nodiscard]] std::vector<std::string> SplitSentences(const std::string& text);

} // namespace revia::presence
