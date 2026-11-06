#pragma once

#include "Identity/relationshipState.h"

#include <string>
#include <vector>

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

// True when the speaker names themselves ("my name is", "call me", "I'm called", "name's"), not
// when someone announces an arrival ("this is Sam"). Either way earlier unnamed lines stay
// withheld; this only decides how she is told about them.
[[nodiscard]] bool NamesThemselves(const std::string& userInput);

// True when the speaker's own words say a different person is now speaking: the previous one
// left ("Noor left", "she had to go") or the speaker is someone else ("I'm a different person",
// "not the same person", "my turn"). Quoted words do not count. A handover is a privacy
// boundary even when the new person goes on to say "my name is" or "call me".
[[nodiscard]] bool AnnouncesAnotherPerson(const std::string& userInput);

// "I meant Mari, not Jo" while the speaker is called Jo: the corrected name, else empty.
// Only the name currently in use can be rejected, and only in the speaker's own words.
[[nodiscard]] std::string ReadSpeakerNameCorrection(const std::string& userInput, const std::string& currentName);

// "hey, it's Mari" from someone already known as Mari: that known name, else empty. An
// unknown name is never adopted this way, so "it's late" or "I'm tired" names nobody.
[[nodiscard]] std::string ReadReturningSpeaker(const std::string& userInput, const std::vector<std::string>& knownNames);

// Builds a bounded event with subunit keyword-inference confidence; the registry scales deltas by it.
[[nodiscard]] RelationshipEvent BuildRelationshipEvent(const std::string& entityId, const ConversationSignals& signals);

} // namespace revia::identity
