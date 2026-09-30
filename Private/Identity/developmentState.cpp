#include "Identity/developmentState.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <vector>

namespace revia::identity
{

namespace
{
    constexpr std::array<const char*, TraitCount> Names = {
        "curiosity", "playfulness", "impulsiveness",
        "stubbornness", "competitiveness",
        "patience", "empathy", "independence",
        "confidence", "caution",
        "sociability", "talkativeness",
        "emotionalExpressiveness", "emotionalRegulation",
        "maturity", "riskTolerance"
    };
    static_assert(Names.size() == TraitCount,
        "Every Trait needs exactly one persisted name.");

    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

}

const std::array<const char*, TraitCount>& TraitNames() { return Names; }

std::string TraitAdjective(const Trait trait)
{
    switch (trait)
    {
        case Trait::Curiosity: return "curious";
        case Trait::Playfulness: return "playful";
        case Trait::Impulsiveness: return "impulsive";
        case Trait::Stubbornness: return "stubborn";
        case Trait::Competitiveness: return "competitive";
        case Trait::Patience: return "patient";
        case Trait::Empathy: return "empathetic";
        case Trait::Independence: return "independent";
        case Trait::Confidence: return "self-assured";
        case Trait::Caution: return "cautious";
        case Trait::Sociability: return "sociable";
        case Trait::Talkativeness: return "talkative";
        case Trait::EmotionalExpressiveness: return "openly expressive";
        case Trait::EmotionalRegulation: return "even-tempered";
        case Trait::Maturity: return "grown-up";
        case Trait::RiskTolerance: return "willing to take risks";
        case Trait::Count: break;
    }
    return "different";
}

std::string ToString(const Trait trait)
{
    const auto index = static_cast<std::size_t>(trait);
    return index < TraitCount ? Names[index] : "unknown";
}

Trait TraitFromString(const std::string& name)
{
    const std::string wanted = Lower(name);
    for (std::size_t index = 0; index < TraitCount; ++index)
    {
        if (Lower(Names[index]) == wanted)
        {
            return static_cast<Trait>(index);
        }
    }
    return Trait::Count;
}

TraitVector& TraitVector::Clamp(const float low, const float high)
{
    for (float& value : values)
    {
        value = std::clamp(value, low, high);
    }
    return *this;
}

TraitVector ChildlikeBaseline()
{
    TraitVector traits;
    // Initial temperament from the character profile.
    traits[Trait::Curiosity] = 0.86F;
    traits[Trait::Playfulness] = 0.78F;
    traits[Trait::Impulsiveness] = 0.72F;
    traits[Trait::Stubbornness] = 0.65F;
    traits[Trait::Competitiveness] = 0.60F;
    traits[Trait::Patience] = 0.30F;
    traits[Trait::Empathy] = 0.55F;
    traits[Trait::Independence] = 0.40F;
    traits[Trait::Confidence] = 0.52F;
    traits[Trait::Caution] = 0.28F;
    traits[Trait::Sociability] = 0.68F;
    traits[Trait::Talkativeness] = 0.62F;
    traits[Trait::EmotionalExpressiveness] = 0.82F;
    traits[Trait::EmotionalRegulation] = 0.30F;
    traits[Trait::Maturity] = 0.32F;
    traits[Trait::RiskTolerance] = 0.58F;
    return traits;
}

TraitVector BaselineFromProfile(const std::map<std::string, float>& values, std::vector<std::string>* outUnknownNames)
{
    TraitVector traits = ChildlikeBaseline();
    for (const auto& [name, value] : values)
    {
        const Trait trait = TraitFromString(name);
        if (trait == Trait::Count)
        {
            if (outUnknownNames != nullptr)
            {
                outUnknownNames->push_back(name);
            }
            continue;
        }
        traits[trait] = std::clamp(value, 0.0F, 1.0F);
    }
    return traits;
}

TraitVector DevelopmentState::Current() const
{
    TraitVector current;
    for (std::size_t index = 0; index < TraitCount; ++index)
    {
        current.values[index] = base.values[index] + delta.values[index];
    }
    return current.Clamp();
}

float DevelopmentState::Current(const Trait trait) const
{
    return std::clamp(base[trait] + delta[trait], 0.0F, 1.0F);
}

std::string DevelopmentState::DescribeDrift(const float minimumDrift) const
{
    struct Moved
    {
        Trait trait;
        float amount;
    };
    std::vector<Moved> moved;
    for (std::size_t index = 0; index < TraitCount; ++index)
    {
        const float amount = delta.values[index];
        if (std::abs(amount) >= minimumDrift)
        {
            moved.push_back({static_cast<Trait>(index), amount});
        }
    }
    if (moved.empty())
    {
        return {};
    }
    std::sort(moved.begin(), moved.end(), [](const Moved& left, const Moved& right)
    {
        return std::abs(left.amount) > std::abs(right.amount);
    });

    std::ostringstream description;
    // Keep the prompt summary to the three largest changes.
    const std::size_t shown = std::min<std::size_t>(moved.size(), 3);
    for (std::size_t index = 0; index < shown; ++index)
    {
        if (index > 0)
        {
            description << (index + 1 == shown ? ", and " : ", ");
        }
        description << (moved[index].amount > 0.0F ? "more " : "less ")
            << TraitAdjective(moved[index].trait) << " than you started out";
    }
    return description.str();
}

} // namespace revia::identity
