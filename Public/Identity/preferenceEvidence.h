#pragma once

#include "Identity/preferenceState.h"

#include <string>
#include <vector>

namespace revia::identity
{

// What the runtime observed that justifies moving one opinion.
//
// Deliberately not a model's claim about what Revia enjoys. The rule the preference
// system rests on is the same one the relationship system rests on: if a language model
// could assert her tastes, she would come to like whatever she was last told she liked.
// So an observation carries a subject the runtime can name, a direction the runtime can
// justify, and the reason, and nothing else.
struct PreferenceObservation
{
    std::string subject;
    bool positive = true;
    PreferenceSource source = PreferenceSource::Observed;
    // Why this counted, so an opinion that formed can be traced back to the exchanges
    // that formed it rather than merely appearing one day.
    std::string reason;
};

// The kind of work a turn was, as a subject an opinion can be held about.
//
// SYSTEM_DESIGN §10 names "work she enjoys" and "work she dislikes" as preference
// categories, and this is how a turn earns one. Empty for anything that is not
// recognisably a kind of work, which is most conversation.
//
// This vocabulary is deliberately separate from IntelligenceRouter's routing signals.
// The two overlap in words and answer different questions: routing asks how much
// computation a turn deserves, this asks what sort of work it was. Merging them would
// make "unreal engine" a kind of work because it happens to be a hard-problem signal.
[[nodiscard]] std::string ReadWorkKind(const std::string& userInput);

// One finished turn, reduced to what the runtime confirmed about it.
//
// Every field here is observed rather than asserted: succeeded comes from whether a
// reply was produced, and the other two come from ReadConversationSignals, the same
// deterministic reader that moves relationships.
struct WorkOutcome
{
    std::string workKind;
    bool succeeded = true;
    bool wasCorrected = false;
    bool expressedAppreciation = false;
};

// Ordinary turns produce nothing on purpose.
//
// A preference that moved every time a turn merely worked would reach a strong opinion
// in half a dozen exchanges, which is a counter, not a taste. Only a turn that went
// notably well or notably badly counts, so most work leaves her opinions where they were.
[[nodiscard]] std::vector<PreferenceObservation> ReadWorkPreferenceEvidence(
    const WorkOutcome& outcome);

// One self-directed curiosity run that finished.
//
// The strongest honest signal available: she chose the topic with nobody asking, the
// runtime watched what happened, and a run that produced a usable cited finding is her
// own experience of the subject rather than anyone's claim about it.
struct CuriosityOutcome
{
    std::string topic;
    // The run actually produced something usable. A failed lookup says nothing about
    // whether she cares for the subject, so it produces no evidence in either direction.
    bool produced = false;
};

[[nodiscard]] std::vector<PreferenceObservation> ReadCuriosityPreferenceEvidence(
    const CuriosityOutcome& outcome);

} // namespace revia::identity
