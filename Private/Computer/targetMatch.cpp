#include "Computer/targetMatch.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace revia::computer
{

namespace
{

std::string Lowered(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) return true;
    return Lowered(haystack).find(Lowered(needle)) != std::string::npos;
}

} // namespace

TargetMatch MatchTarget(
    const std::vector<ObservedCandidate>& candidates,
    const TargetDescriptor& wanted,
    const TargetAffordance affordance)
{
    enum class Tier { PublishedName, InferredLabel, ContainerAndRole };

    const auto affords = [&](const ObservedCandidate& candidate)
    {
        switch (affordance)
        {
            case TargetAffordance::Editable:
                return candidate.maySetText || candidate.mayType;
            case TargetAffordance::Invokable:
                return candidate.mayInvoke;
            case TargetAffordance::Any:
            default:
                return true;
        }
    };
    const auto roleAccepts = [&](const ObservedCandidate& candidate)
    {
        // Role narrows; it never widens. A candidate whose role the observer could not
        // name is skipped when a role was asked for, because "unknown" is not "matches".
        return wanted.role.empty() || Lowered(candidate.role) == Lowered(wanted.role);
    };
    const auto containerAccepts = [&](const ObservedCandidate& candidate)
    {
        // A candidate whose container is unknown is excluded when one was asked for,
        // rather than admitted by default.
        if (wanted.container.empty()) return true;
        return !candidate.container.empty() &&
            Contains(candidate.container, wanted.container);
    };

    const auto matchesTier = [&](const ObservedCandidate& candidate, const Tier tier)
    {
        switch (tier)
        {
            case Tier::PublishedName:
                // Matched whole, not as a fragment. "Send" must not press "Send later":
                // a descriptor that matched loosely could press the wrong button and
                // report that it pressed the right one.
                return !wanted.name.empty() && !candidate.name.empty() &&
                    Lowered(candidate.name) == Lowered(wanted.name);
            case Tier::InferredLabel:
                return !wanted.name.empty() && !candidate.inferredLabel.empty() &&
                    Lowered(candidate.inferredLabel) == Lowered(wanted.name);
            case Tier::ContainerAndRole:
                // No name to go on at all. This tier only resolves anything when the
                // descriptor named a container and a role, which together pick out one
                // control in one panel -- and it refuses to consider candidates that
                // published a name, because those belong to the tier above.
                return wanted.name.empty() && !wanted.container.empty() &&
                    !wanted.role.empty() && candidate.nameless;
        }
        return false;
    };

    for (const Tier tier :
        {Tier::PublishedName, Tier::InferredLabel, Tier::ContainerAndRole})
    {
        TargetMatch match;
        match.usedInference = tier != Tier::PublishedName;
        for (const ObservedCandidate& candidate : candidates)
        {
            if (!matchesTier(candidate, tier)) continue;
            if (!roleAccepts(candidate)) continue;
            if (!containerAccepts(candidate)) continue;
            if (!affords(candidate)) continue;

            ++match.matchCount;
            if (match.matchCount == 1) match.candidate = &candidate;
        }

        if (match.matchCount == 1)
        {
            match.outcome = TargetMatch::Outcome::Found;
            return match;
        }
        if (match.matchCount > 1)
        {
            // Never resolved by taking the first. A description that fits two things is
            // a question about which one was meant, and answering it by position is
            // answering it by accident.
            match.outcome = TargetMatch::Outcome::Ambiguous;
            match.candidate = nullptr;
            return match;
        }
    }

    // Nothing matched in any tier. Whether that means "not there" or "there and not
    // describable" is the caller's to decide from the observation.
    return TargetMatch{};
}

} // namespace revia::computer
