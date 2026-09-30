#pragma once

#include "Identity/preferenceState.h"

#include <string>
#include <vector>

namespace revia::identity
{

// Runtime-justified opinion evidence: subject, direction and reason, never a model-assigned taste.
struct PreferenceObservation
{
    std::string subject;
    bool positive = true;
    PreferenceSource source = PreferenceSource::Observed;
    // Why this counted, so an opinion that formed can be traced back to the exchanges
    // that formed it rather than merely appearing one day.
    std::string reason;
};

// Classifies recognizable work into preference subjects; returns empty for other conversation.
// This vocabulary is independent of model-routing difficulty signals.
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
[[nodiscard]] std::vector<PreferenceObservation> ReadWorkPreferenceEvidence(const WorkOutcome& outcome);

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

[[nodiscard]] std::vector<PreferenceObservation> ReadCuriosityPreferenceEvidence(const CuriosityOutcome& outcome);

} // namespace revia::identity
