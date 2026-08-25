#pragma once

#include <string>

namespace revia::runtime
{

enum class AffectState
{
    Neutral,
    Curious,
    Pleased,
    Excited,
    Playful,
    Bored,
    Sulky,
    Sad,
    Melancholy,
    Angry,
    Lonely,
    Frustrated,
    Concerned,
    Focused,
    Confused
};

struct AffectSnapshot
{
    AffectState state = AffectState::Neutral;
    float intensity = 0.25F;
    std::string reason = "Calm baseline.";
};

std::string ToString(AffectState state);

} // namespace revia::runtime
