#pragma once

#include "Identity/relationshipState.h"

#include <string>

namespace revia::identity
{

// Observable exchange signals for bounded relationship evidence; models cannot assign relationship values.
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
    // Explicit joking language in this turn; closeness alone is not evidence of it.
    bool explicitlyPlayful = false;
};

// Reads observable signals out of one turn. Keyword-driven and deterministic, sharing
// the same vocabulary the affect classifier uses, so what moves a relationship and what
// moves a feeling cannot silently disagree about what was said.
[[nodiscard]] ConversationSignals ReadConversationSignals(const std::string& userInput, const std::string& reply, bool succeeded);

// Returns a name only from an explicit self-introduction; otherwise empty.
[[nodiscard]] std::string ReadStatedName(const std::string& userInput);

// Builds a bounded event with subunit keyword-inference confidence; the registry scales deltas by it.
[[nodiscard]] RelationshipEvent BuildRelationshipEvent(const std::string& entityId, const ConversationSignals& signals);

} // namespace revia::identity
