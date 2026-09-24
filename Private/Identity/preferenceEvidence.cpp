#include "Identity/preferenceEvidence.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace revia::identity
{

namespace
{
// A curiosity topic is proposed by a model, and this is the one place that matters.
// The model does not get to say she likes it -- the runtime decides that from what
// happened -- but it does supply the words the subject is stored under, so the words
// have to be bounded before they become a durable key.
constexpr std::size_t MaximumTopicCharacters = 80;

std::string LowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

template <std::size_t Size>
bool ContainsAny(
    const std::string& value,
    const std::array<std::string_view, Size>& signals)
{
    return std::any_of(signals.begin(), signals.end(),
        [&value](const std::string_view signal)
        {
            return value.find(signal) != std::string::npos;
        });
}

// Ordered most specific first. Debugging beats writing code, because "the code crashes"
// is debugging even though it says code, and a turn is only ever one kind of work.
const std::array<std::string_view, 8> DebuggingSignals = {
    "deadlock", "race condition", "use-after-free", "crash", "stack trace",
    "exception", "debug", "root cause"
};
const std::array<std::string_view, 5> ArchitectureSignals = {
    "architecture", "design the", "ownership across", "threat model", "trade-off"
};
const std::array<std::string_view, 5> CodeSignals = {
    "implement", "refactor", "write a function", "write the", "add a test"
};
const std::array<std::string_view, 5> ExplainSignals = {
    "explain", "why does", "how do", "summarize", "what is the difference"
};
const std::array<std::string_view, 3> PlanningSignals = {
    "plan ", "plan the", "roadmap"
};
const std::array<std::string_view, 4> ResearchSignals = {
    "look up", "search for", "find out", "research"
};
}

std::string ReadWorkKind(const std::string& userInput)
{
    const std::string lowered = LowerCopy(userInput);
    if (ContainsAny(lowered, DebuggingSignals)) return "debugging";
    if (ContainsAny(lowered, ArchitectureSignals)) return "architecture work";
    if (ContainsAny(lowered, CodeSignals)) return "writing code";
    if (ContainsAny(lowered, ExplainSignals)) return "explaining things";
    if (ContainsAny(lowered, PlanningSignals)) return "planning";
    if (ContainsAny(lowered, ResearchSignals)) return "looking things up";
    return {};
}

std::vector<PreferenceObservation> ReadWorkPreferenceEvidence(const WorkOutcome& outcome)
{
    std::vector<PreferenceObservation> observations;
    if (outcome.workKind.empty())
    {
        return observations;
    }

    // Notably bad: the work did not come together, or she had to be put right on it
    // again. Both are things the runtime saw rather than concluded.
    if (!outcome.succeeded || outcome.wasCorrected)
    {
        observations.push_back({
            outcome.workKind,
            false,
            PreferenceSource::Observed,
            outcome.succeeded
                ? "she had to be corrected again on " + outcome.workKind
                : outcome.workKind + " did not come together"});
        return observations;
    }

    // Notably good: it worked AND the person said so. Success on its own is not
    // evidence of enjoyment -- it is the expected outcome, and counting it would turn
    // her tastes into a tally of how often each kind of work happened to succeed.
    if (outcome.expressedAppreciation)
    {
        observations.push_back({
            outcome.workKind,
            true,
            PreferenceSource::Observed,
            outcome.workKind + " went well and was appreciated"});
    }
    return observations;
}

std::vector<PreferenceObservation> ReadCuriosityPreferenceEvidence(
    const CuriosityOutcome& outcome)
{
    std::vector<PreferenceObservation> observations;
    if (!outcome.produced)
    {
        return observations;
    }
    const std::string subject = PreferenceSet::NormaliseSubject(outcome.topic);
    if (subject.empty() || subject.size() > MaximumTopicCharacters)
    {
        return observations;
    }
    observations.push_back({
        subject,
        true,
        PreferenceSource::Observed,
        "she went looking into this on her own and it paid off"});
    return observations;
}

} // namespace revia::identity
