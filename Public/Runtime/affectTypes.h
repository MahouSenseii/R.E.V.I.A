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

// Who is speaking, and how the day has gone so far.
//
// The same sentence is not the same event. A jab from someone Revia has talked to for
// months is teasing; the identical words from a stranger are an attack. A mild
// annoyance lands hard on a day that has already gone badly and slides off one that
// has not. Without this, hostility from a friend of 500 turns classified exactly like
// hostility from someone she met a minute ago, which is the least human thing the
// emotion system did.
//
// Carried as plain numbers rather than a reference to the controller that owns them, so
// classification stays a pure function of its inputs and can be tested without building
// a social history first. The defaults are deliberately inert: a caller that supplies no
// context gets exactly the unmodulated reading.
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
