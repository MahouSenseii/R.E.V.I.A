#include "Emotion/emotionTypes.h"

#include <algorithm>
#include <cctype>

namespace revia::emotion
{

namespace
{
    // Indices here must line up with the enum. The static_assert below is what keeps
    // that true rather than a comment asking future edits to be careful.
    constexpr std::array<const char*, EmotionCount> Names = {
        "joy", "curiosity", "excitement", "amusement",
        "affection", "pride", "confidence",
        "sadness", "loneliness", "disappointment",
        "anger", "irritation", "frustration",
        "boredom", "envy", "embarrassment",
        "fear", "concern", "confusion",
        "relief",
        "contentment", "gratitude", "fondness", "warmth", "admiration", "hope",
        "anticipation", "delight", "satisfaction", "playfulness", "mischief",
        "smugness", "determination", "absorption", "nostalgia", "awe", "annoyance",
        "impatience", "indignation", "resentment", "hurt", "regret", "guilt", "shame",
        "insecurity", "doubt", "apprehension", "overwhelm", "weariness",
        "restlessness", "wistfulness", "defensiveness", "surprise", "suspicion"
    };
    static_assert(Names.size() == EmotionCount,
        "Every Emotion needs exactly one persisted name.");

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

const std::array<const char*, EmotionCount>& EmotionNames()
{
    return Names;
}

std::string ToString(const Emotion emotion)
{
    const auto index = static_cast<std::size_t>(emotion);
    return index < EmotionCount ? Names[index] : "unknown";
}

Emotion EmotionFromString(const std::string& name)
{
    const std::string wanted = Lower(name);
    for (std::size_t index = 0; index < EmotionCount; ++index)
    {
        if (Names[index] == wanted)
        {
            return static_cast<Emotion>(index);
        }
    }
    return Emotion::Count;
}

bool IsPleasant(const Emotion emotion)
{
    switch (emotion)
    {
        case Emotion::Joy:
        case Emotion::Curiosity:
        case Emotion::Excitement:
        case Emotion::Amusement:
        case Emotion::Affection:
        case Emotion::Pride:
        case Emotion::Confidence:
        case Emotion::Relief:
        case Emotion::Contentment:
        case Emotion::Gratitude:
        case Emotion::Fondness:
        case Emotion::Warmth:
        case Emotion::Admiration:
        case Emotion::Hope:
        case Emotion::Anticipation:
        case Emotion::Delight:
        case Emotion::Satisfaction:
        case Emotion::Playfulness:
        case Emotion::Mischief:
        case Emotion::Smugness:
        case Emotion::Determination:
        case Emotion::Absorption:
        case Emotion::Nostalgia:
        case Emotion::Awe:
            return true;
        default:
            // Curiosity and confusion sit close together and only one of them is
            // pleasant; concern is unpleasant even when it is useful. Neutral-ish
            // states fall here deliberately rather than being counted as good.
            return false;
    }
}

EmotionVector& EmotionVector::Clamp()
{
    for (float& value : values)
    {
        value = std::clamp(value, 0.0F, 1.0F);
    }
    return *this;
}

EmotionVector& EmotionVector::Decay(const float rate)
{
    const float kept = 1.0F - std::clamp(rate, 0.0F, 1.0F);
    for (float& value : values)
    {
        value *= kept;
        // Anything this small is indistinguishable from calm and only serves to keep a
        // feeling nominally alive forever, which makes "has she got over it yet?"
        // unanswerable.
        if (value < 0.004F)
        {
            value = 0.0F;
        }
    }
    return *this;
}

EmotionVector& EmotionVector::Add(const EmotionVector& delta)
{
    for (std::size_t index = 0; index < EmotionCount; ++index)
    {
        values[index] += delta.values[index];
    }
    return Clamp();
}

EmotionReading EmotionVector::Dominant() const
{
    EmotionReading strongest;
    strongest.emotion = Emotion::Joy;
    strongest.value = 0.0F;
    for (std::size_t index = 0; index < EmotionCount; ++index)
    {
        if (values[index] > strongest.value)
        {
            strongest.value = values[index];
            strongest.emotion = static_cast<Emotion>(index);
        }
    }
    return strongest;
}

std::vector<EmotionReading> EmotionVector::Significant(const float threshold) const
{
    std::vector<EmotionReading> readings;
    for (std::size_t index = 0; index < EmotionCount; ++index)
    {
        if (values[index] >= threshold)
        {
            readings.push_back({static_cast<Emotion>(index), values[index]});
        }
    }
    std::sort(readings.begin(), readings.end(),
        [](const EmotionReading& left, const EmotionReading& right)
        {
            // Ties broken by enum order so the same state always renders identically;
            // a prompt that reshuffles between turns looks like state that changed.
            if (left.value != right.value)
            {
                return left.value > right.value;
            }
            return static_cast<std::size_t>(left.emotion) <
                static_cast<std::size_t>(right.emotion);
        });
    return readings;
}

float EmotionVector::TotalIntensity() const
{
    float total = 0.0F;
    for (const float value : values)
    {
        total += value;
    }
    return total;
}

float EmotionVector::Valence() const
{
    float pleasant = 0.0F;
    float unpleasant = 0.0F;
    for (std::size_t index = 0; index < EmotionCount; ++index)
    {
        if (IsPleasant(static_cast<Emotion>(index)))
        {
            pleasant += values[index];
        }
        else
        {
            unpleasant += values[index];
        }
    }
    const float total = pleasant + unpleasant;
    // Nothing felt is neutral, not negative. Dividing by a zero total would make calm
    // read as misery.
    return total <= 0.0F ? 0.0F : (pleasant - unpleasant) / total;
}

bool EmotionVector::IsCalm(const float threshold) const
{
    return Dominant().value < threshold;
}

} // namespace revia::emotion
