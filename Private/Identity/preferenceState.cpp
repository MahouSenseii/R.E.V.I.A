#include "Identity/preferenceState.h"

#include <algorithm>
#include <cctype>

namespace revia::identity
{

namespace
{
    float ConfidenceFor(const std::size_t evidenceCount, const std::size_t confidentEvidence)
    {
        if (confidentEvidence == 0)
        {
            return 1.0F;
        }
        const float ratio = static_cast<float>(evidenceCount) /
            static_cast<float>(confidentEvidence);
        return std::clamp(ratio, 0.0F, 1.0F);
    }

    // How authoritative an origin is, most authoritative first. Written out rather than
    // read off the enum's order: ordering an enum by meaning makes reordering it a
    // silent behaviour change, and this decides which origin survives a merge.
    int Authority(const PreferenceSource source)
    {
        switch (source)
        {
            case PreferenceSource::Profile: return 0;
            case PreferenceSource::Stated: return 1;
            case PreferenceSource::Observed: return 2;
            case PreferenceSource::Inferred: return 3;
        }
        return 3;
    }

    // Strength weighted by confidence. A confident mild taste is more usefully hers than
    // a barely evidenced strong one, and ranking on strength alone would put every
    // first-impression guess at the top of the prompt.
    float Weight(const Preference& preference)
    {
        return std::abs(preference.strength) * preference.confidence;
    }
}

std::string ToString(const PreferenceDirection direction)
{
    switch (direction)
    {
        case PreferenceDirection::Dislike: return "dislike";
        case PreferenceDirection::Neutral: return "neutral";
        case PreferenceDirection::Like: return "like";
    }
    return "neutral";
}

std::string ToString(const PreferenceSource source)
{
    switch (source)
    {
        case PreferenceSource::Profile: return "profile";
        case PreferenceSource::Stated: return "stated";
        case PreferenceSource::Observed: return "observed";
        case PreferenceSource::Inferred: return "inferred";
    }
    return "inferred";
}

PreferenceSource PreferenceSourceFromString(const std::string& name)
{
    if (name == "profile") return PreferenceSource::Profile;
    if (name == "stated") return PreferenceSource::Stated;
    if (name == "observed") return PreferenceSource::Observed;
    return PreferenceSource::Inferred;
}

PreferenceDirection Preference::Direction(const float neutralBand) const
{
    if (strength > neutralBand) return PreferenceDirection::Like;
    if (strength < -neutralBand) return PreferenceDirection::Dislike;
    return PreferenceDirection::Neutral;
}

bool Preference::WorthStating(
    const float minimumConfidence, const float neutralBand) const
{
    return confidence >= minimumConfidence &&
        Direction(neutralBand) != PreferenceDirection::Neutral;
}

std::string PreferenceSet::NormaliseSubject(const std::string& subject)
{
    std::string result;
    result.reserve(subject.size());
    bool pendingSpace = false;
    for (const unsigned char character : subject)
    {
        if (std::isspace(character) != 0)
        {
            pendingSpace = !result.empty();
            continue;
        }
        if (pendingSpace)
        {
            result.push_back(' ');
            pendingSpace = false;
        }
        result.push_back(static_cast<char>(std::tolower(character)));
    }
    return result;
}

Preference PreferenceSet::Reinforce(
    const std::string& subject,
    const bool positive,
    const PreferenceSource source,
    const std::string& timestamp,
    const PreferenceLimits& limits)
{
    const std::string key = NormaliseSubject(subject);
    const auto existing = std::find_if(preferences.begin(), preferences.end(),
        [&key](const Preference& candidate) { return candidate.subject == key; });

    Preference* target = nullptr;
    if (existing == preferences.end())
    {
        Preference created;
        created.subject = key;
        created.source = source;
        preferences.push_back(std::move(created));
        target = &preferences.back();
    }
    else
    {
        target = &*existing;
    }

    // Bounded, and the same size whichever way it points. A step that grew with
    // agreement would let a run of enthusiasm outrun the evidence behind it.
    const float step = positive ? limits.maximumStep : -limits.maximumStep;
    target->strength = std::clamp(target->strength + step, -1.0F, 1.0F);
    ++target->evidenceCount;
    target->confidence = ConfidenceFor(target->evidenceCount, limits.confidentEvidence);
    target->lastReinforced = timestamp;
    // The recorded origin is the most authoritative one seen: once she has said a thing
    // or it has been observed, "guessed" is no longer how she got here.
    if (Authority(source) < Authority(target->source))
    {
        target->source = source;
    }

    const Preference result = *target;
    Prune(limits);
    return result;
}

bool PreferenceSet::SeedFromProfile(const std::string& subject, const float strength)
{
    const std::string key = NormaliseSubject(subject);
    if (key.empty())
    {
        return false;
    }
    const auto existing = std::find_if(preferences.begin(), preferences.end(),
        [&key](const Preference& candidate) { return candidate.subject == key; });
    if (existing != preferences.end())
    {
        return false;
    }

    Preference seeded;
    seeded.subject = key;
    seeded.strength = std::clamp(strength, -1.0F, 1.0F);
    // Authored rather than learned, so it is held with full confidence and no evidence
    // count. Evidence counts observations; a profile is not an observation.
    seeded.confidence = 1.0F;
    seeded.evidenceCount = 0;
    seeded.source = PreferenceSource::Profile;
    preferences.push_back(std::move(seeded));
    return true;
}

const Preference* PreferenceSet::Find(const std::string& subject) const
{
    const std::string key = NormaliseSubject(subject);
    const auto found = std::find_if(preferences.begin(), preferences.end(),
        [&key](const Preference& candidate) { return candidate.subject == key; });
    return found == preferences.end() ? nullptr : &*found;
}

std::vector<Preference> PreferenceSet::All() const { return preferences; }

std::vector<Preference> PreferenceSet::Strongest(const std::size_t limit) const
{
    std::vector<Preference> held;
    held.reserve(preferences.size());
    for (const Preference& preference : preferences)
    {
        if (preference.Direction() != PreferenceDirection::Neutral)
        {
            held.push_back(preference);
        }
    }
    std::stable_sort(held.begin(), held.end(),
        [](const Preference& left, const Preference& right)
        {
            return Weight(left) > Weight(right);
        });
    if (held.size() > limit)
    {
        held.resize(limit);
    }
    return held;
}

void PreferenceSet::Replace(std::vector<Preference> loaded)
{
    preferences = std::move(loaded);
}

void PreferenceSet::Prune(const PreferenceLimits& limits)
{
    if (limits.maximumRetained == 0 || preferences.size() <= limits.maximumRetained)
    {
        return;
    }
    // Weakest and least evidenced go first. A profile-declared preference is kept ahead
    // of an equally weak learned one: it was authored deliberately.
    std::stable_sort(preferences.begin(), preferences.end(),
        [](const Preference& left, const Preference& right)
        {
            if (left.source == PreferenceSource::Profile &&
                right.source != PreferenceSource::Profile)
            {
                return true;
            }
            if (right.source == PreferenceSource::Profile &&
                left.source != PreferenceSource::Profile)
            {
                return false;
            }
            return Weight(left) > Weight(right);
        });
    preferences.resize(limits.maximumRetained);
}

} // namespace revia::identity
