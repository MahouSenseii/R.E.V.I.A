#include "Autonomy/driveState.h"

#include <algorithm>
#include <array>
#include <sstream>

namespace revia::autonomy
{

namespace
{
    constexpr std::array<const char*, DriveCount> Names = {
        "curiosity", "boredom", "social", "unfinishedGoal",
        "exploration", "learning", "creativity"
    };
    static_assert(Names.size() == DriveCount,
        "Every Drive needs exactly one persisted name.");
}

std::string ToString(const Drive drive)
{
    const auto index = static_cast<std::size_t>(drive);
    return index < DriveCount ? Names[index] : "unknown";
}

Drive DriveFromString(const std::string& name)
{
    for (std::size_t index = 0; index < DriveCount; ++index)
    {
        if (Names[index] == name)
        {
            return static_cast<Drive>(index);
        }
    }
    return Drive::Count;
}

DriveState& DriveState::Clamp()
{
    for (float& value : values)
    {
        value = std::clamp(value, 0.0F, 1.0F);
    }
    return *this;
}

Drive DriveState::Strongest() const
{
    Drive strongest = Drive::Curiosity;
    float best = 0.0F;
    for (std::size_t index = 0; index < DriveCount; ++index)
    {
        if (values[index] > best)
        {
            best = values[index];
            strongest = static_cast<Drive>(index);
        }
    }
    return strongest;
}

bool DriveState::IsQuiet(const float threshold) const
{
    return std::none_of(values.begin(), values.end(),
        [threshold](const float value) { return value >= threshold; });
}

std::string DriveState::Describe(const float threshold) const
{
    std::vector<std::pair<float, Drive>> active;
    for (std::size_t index = 0; index < DriveCount; ++index)
    {
        if (values[index] >= threshold)
        {
            active.emplace_back(values[index], static_cast<Drive>(index));
        }
    }
    if (active.empty())
    {
        return {};
    }
    std::sort(active.begin(), active.end(),
        [](const auto& left, const auto& right) { return left.first > right.first; });

    std::ostringstream description;
    const std::size_t shown = std::min<std::size_t>(active.size(), 3);
    for (std::size_t index = 0; index < shown; ++index)
    {
        if (index > 0)
        {
            description << (index + 1 == shown ? " and " : ", ");
        }
        description << ToString(active[index].second);
    }
    return description.str();
}

DriveController::DriveController(DriveDynamics inputDynamics)
    : dynamics(inputDynamics)
{
}

DriveState DriveController::Observe(
    DriveState drives,
    const emotion::Stimulus& stimulus,
    const emotion::EmotionVector& emotion) const
{
    const float weight = std::clamp(stimulus.importance, 0.0F, 1.0F) *
        std::clamp(stimulus.certainty, 0.0F, 1.0F);
    if (weight <= 0.0F)
    {
        return drives;
    }
    const float ceiling = dynamics.maximumStep;
    const auto add = [&](const Drive drive, const float amount)
    {
        drives[drive] += std::clamp(amount * weight, -ceiling, ceiling);
    };

    // Novelty is what makes curiosity a drive rather than a mood: something new appeared
    // and has not been followed up.
    if (stimulus.novelty > 0.0F)
    {
        add(Drive::Curiosity, stimulus.novelty * 0.6F);
        add(Drive::Exploration, stimulus.novelty * 0.35F);
    }
    // Anything happening at all relieves boredom, which is what boredom means.
    add(Drive::Boredom, -0.4F);

    switch (stimulus.source)
    {
        case emotion::StimulusSource::Goal:
            // A goal that failed leaves something unfinished pulling at her; one that
            // succeeded settles it.
            add(Drive::UnfinishedGoal, stimulus.failure > 0.0F ? 0.5F : -0.6F);
            if (stimulus.failure > 0.0F)
            {
                // A failure worth understanding is a reason to learn, not only to sulk.
                add(Drive::Learning, 0.3F);
            }
            break;
        case emotion::StimulusSource::Research:
            add(Drive::Learning, 0.4F);
            add(Drive::Curiosity, -0.2F);
            break;
        case emotion::StimulusSource::Conversation:
            add(Drive::Social, stimulus.valence >= 0.0F ? 0.2F : -0.3F);
            break;
        case emotion::StimulusSource::Perception:
        case emotion::StimulusSource::Environment:
            add(Drive::Exploration, 0.25F);
            break;
        default:
            break;
    }

    // Emotion feeds wanting. Curiosity felt is curiosity pursued; boredom felt is the
    // drive itself; loneliness is what turns into wanting company.
    drives[Drive::Curiosity] += 0.15F * emotion[emotion::Emotion::Curiosity];
    drives[Drive::Boredom] += 0.2F * emotion[emotion::Emotion::Boredom];
    drives[Drive::Social] += 0.18F * emotion[emotion::Emotion::Loneliness];
    drives[Drive::Creativity] += 0.12F * emotion[emotion::Emotion::Excitement];
    return drives.Clamp();
}

DriveState DriveController::Settle(DriveState drives, const bool userPresent) const
{
    for (std::size_t index = 0; index < DriveCount; ++index)
    {
        // Boredom is excluded from the general decay because it has the opposite
        // dynamics: everything else fades when nothing happens, and boredom is what
        // nothing happening feels like. Decaying it here as well made decay outpace
        // growth, pinning it near zero forever -- the drive existed but could never
        // actually build.
        if (static_cast<Drive>(index) == Drive::Boredom)
        {
            continue;
        }
        drives.values[index] = std::max(
            0.0F, drives.values[index] - dynamics.decayRate);
    }
    // The one drive time itself creates. Everything else needs an event, which is what
    // stops a quiet afternoon from manufacturing a reason to act.
    drives[Drive::Boredom] = std::min(
        1.0F, drives[Drive::Boredom] + dynamics.boredomGrowth);
    if (userPresent)
    {
        // Company is not the same as conversation, but it does take the edge off both
        // boredom and the pull to seek someone out.
        drives[Drive::Boredom] =
            std::max(0.0F, drives[Drive::Boredom] - dynamics.boredomGrowth * 0.5F);
        drives[Drive::Social] =
            std::max(0.0F, drives[Drive::Social] - dynamics.decayRate);
    }
    return drives.Clamp();
}

DriveState DriveController::Satisfy(DriveState drives, const Drive drive) const
{
    // Acting on a drive spends it. Without this, finding the answer would not reduce the
    // wanting and she would research the same thing forever.
    drives[drive] *= (1.0F - std::clamp(dynamics.satisfactionRate, 0.0F, 1.0F));
    return drives.Clamp();
}

} // namespace revia::autonomy
