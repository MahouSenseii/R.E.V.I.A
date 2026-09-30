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

// Value-only social/history context keeps classification pure.
// Inert defaults preserve the unmodulated reading when no context is supplied.
struct SocialContext
{
    // 0 stranger .. 1 long shared history.
    float familiarity = 0.25F;
    // 0 calm .. 1 patience already spent.
    float irritation = 0.0F;
    // How much appetite she has for interaction right now. Playing costs some.
    float socialEnergy = 0.6F;
    // How sure of herself she currently is. Decides whether a failure stings or defeats.
    float confidence = 0.62F;
};

} // namespace revia::runtime
