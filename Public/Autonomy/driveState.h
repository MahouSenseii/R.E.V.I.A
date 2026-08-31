#pragma once

#include "Emotion/emotionTypes.h"
#include "Emotion/moodState.h"
#include "Emotion/stimulus.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace revia::autonomy
{

// What Revia is pulled toward. Not what she is feeling, and not what she is doing.
//
// Drives are the difference between a companion that acts when a timer fires and one
// that acts because something is actually pulling at her. A timer may permit an
// activity; only a drive can motivate one.
enum class Drive : std::size_t
{
    Curiosity,
    Boredom,
    Social,
    UnfinishedGoal,
    Exploration,
    Learning,
    Creativity,

    Count
};

inline constexpr std::size_t DriveCount = static_cast<std::size_t>(Drive::Count);

[[nodiscard]] std::string ToString(Drive drive);
[[nodiscard]] Drive DriveFromString(const std::string& name);

struct DriveState
{
    std::array<float, DriveCount> values{};

    float& operator[](Drive drive) { return values[static_cast<std::size_t>(drive)]; }
    float operator[](Drive drive) const { return values[static_cast<std::size_t>(drive)]; }

    DriveState& Clamp();
    [[nodiscard]] Drive Strongest() const;
    [[nodiscard]] bool IsQuiet(float threshold = 0.35F) const;
    [[nodiscard]] std::string Describe(float threshold = 0.4F) const;
};

// How drives move.
//
// Everything decays. A drive that only ever rose would mean Revia becomes more restless
// forever, and the first quiet afternoon would end with her doing something drastic.
struct DriveDynamics
{
    // Per idle step, toward zero.
    float decayRate = 0.03F;
    // Boredom is the one drive that grows from nothing happening -- that is what boredom
    // is. Everything else needs an event.
    float boredomGrowth = 0.012F;
    // Acting on a drive spends it. Without this she would research the same thing
    // forever, because finding the answer would not reduce the wanting.
    float satisfactionRate = 0.55F;
    // Ceiling on how much one event can add, so a single surprise cannot produce a
    // compulsion.
    float maximumStep = 0.25F;
};

// Moves drives from events and from time passing.
//
// Pure and clock-free: the caller decides when a step happens, so the dynamics stay a
// function of their inputs and can be tested without waiting.
class DriveController
{
public:
    explicit DriveController(DriveDynamics dynamics = {});

    // Something happened. Drives respond to the same typed stimuli emotion does, so
    // wanting and feeling cannot disagree about what occurred.
    [[nodiscard]] DriveState Observe(
        DriveState drives,
        const emotion::Stimulus& stimulus,
        const emotion::EmotionVector& emotion) const;

    // Time passing with nothing happening. Boredom rises; everything else fades.
    // userPresent matters: being alone is not the same as being ignored.
    [[nodiscard]] DriveState Settle(DriveState drives, bool userPresent) const;

    // Acting on a drive spends it.
    [[nodiscard]] DriveState Satisfy(DriveState drives, Drive drive) const;

    [[nodiscard]] const DriveDynamics& Dynamics() const { return dynamics; }

private:
    DriveDynamics dynamics;
};

} // namespace revia::autonomy
