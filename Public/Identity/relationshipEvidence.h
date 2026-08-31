#pragma once

#include "Identity/relationshipState.h"

#include <string>

namespace revia::identity
{

// What actually happened in one exchange, as observable signals.
//
// Deliberately not a model's opinion. The rule the whole relationship system rests on is
// that a language model may not assign relationship numbers -- if it could, Revia would
// become fond of anyone who told her they were friends. So evidence is read from things
// the runtime can see for itself: what was said, whether the turn succeeded, whether the
// user is repeating a correction.
//
// This is coarse, and that is correct. Coarse evidence applied a hundred times builds a
// relationship slowly and defensibly; precise evidence inferred once builds one that
// cannot be explained.
struct ConversationSignals
{
    std::string userInput;
    std::string reply;
    // Whether the turn produced a usable answer at all.
    bool succeeded = true;
    // The user is repeating themselves because Revia missed something.
    bool repeatedCorrection = false;
    // The user is thanking, praising, or otherwise closing warmly.
    bool expressedAppreciation = false;
    // Aimed at Revia rather than at the problem.
    bool hostileTowardRevia = false;
    // Cooperative work: a task attempted together that landed.
    bool collaborative = false;
    // How much this exchange should count at all.
    float importance = 0.4F;
};

// Reads observable signals out of one turn. Keyword-driven and deterministic, sharing
// the same vocabulary the affect classifier uses, so what moves a relationship and what
// moves a feeling cannot silently disagree about what was said.
[[nodiscard]] ConversationSignals ReadConversationSignals(
    const std::string& userInput,
    const std::string& reply,
    bool succeeded);

// Converts signals into a bounded relationship event.
//
// Confidence is deliberately below one: this is inference from keywords, not a measured
// fact, and the registry scales every delta by it. An exchange the reader is unsure
// about should barely move anything.
// Reads a name the speaker stated about themselves, or an empty string.
//
// Deterministic and narrow on purpose. A model asked "what is their name" will answer
// even when nobody said one, and a relationship labelled with a hallucinated name is
// worse than one labelled "local:user" -- it looks authoritative while being invented.
// Only an explicit self-introduction counts.
[[nodiscard]] std::string ReadStatedName(const std::string& userInput);

[[nodiscard]] RelationshipEvent BuildRelationshipEvent(
    const std::string& entityId,
    const ConversationSignals& signals);

} // namespace revia::identity
