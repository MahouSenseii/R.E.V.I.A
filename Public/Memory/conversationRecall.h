#pragma once

#include "Memory/conversationArchive.h"
#include "Memory/temporalQuery.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace revia::memory
{

// What kind of question about the past this turn is asking, if any.
enum class RecallKind
{
    // The overwhelmingly common answer. This turn is not about what was said.
    None,
    // A stretch of conversation named by time: "what did we talk about last Tuesday".
    Window,
    // What was said about a subject, whenever that was: "what did I say about the
    // emotion system".
    Topic,
    // When a subject was first raised: "when did I first mention Project Hunter".
    Earliest
};

// One resolved request to consult the durable transcript.
//
// Deliberately typed rather than a free-text query. The archive is a separate store from
// curated memory and stays that way (SYSTEM_DESIGN 15); this is the narrow, explicit
// interface by which a turn may reach it, and nothing about it lets model output choose
// what gets read.
struct RecallRequest
{
    RecallKind kind = RecallKind::None;
    // Valid only for Window. The stretch of time the question named.
    TimeWindow window;
    // The topic words left after the question's scaffolding is removed. May be empty for
    // a Window request, which is then answered by the stretch alone.
    std::vector<std::string> terms;
    // Why this fired, in words, for the activity feed and the log. A retrieval the user
    // cannot see the reason for is one they cannot judge.
    std::string reason;

    [[nodiscard]] bool Wanted() const
    {
        return kind != RecallKind::None;
    }
};

// Decides whether the current turn needs the durable transcript, and what to ask it for.
//
// Deterministic and local. Making this a model call would put a hidden classification
// round-trip in front of every social turn, and would let generated text decide when to
// read the record of everything that was ever said -- which is the sort of authority the
// trust boundary exists to withhold.
//
// It is written to stay quiet. A time reference alone is not enough: "I will do it
// tomorrow" names a time and asks for nothing. Something in the turn has to be about the
// conversation itself.
class ConversationRecallPolicy
{
public:
    [[nodiscard]] static RecallRequest Evaluate(
        const std::string& input,
        std::int64_t nowEpoch);
};

// Renders retrieved turns into the one bounded block a turn is allowed to carry.
//
// Bounded twice: each turn is truncated, and the block as a whole stops at
// maxCharacters. An archive excerpt that crowded out the conversation it was meant to
// support would cost more than it returned.
[[nodiscard]] std::string RenderRecallBlock(
    const RecallRequest& request,
    const std::vector<ArchivedTurn>& turns,
    const std::string& assistantName,
    std::int64_t nowEpoch,
    std::size_t maxCharacters = 2400);

} // namespace revia::memory
