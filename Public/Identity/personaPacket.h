#pragma once

#include "Library/structLibrary.h"

#include <string>

namespace revia::identity
{

// The stable half of who she is, rendered once per session in the order that caches:
// the identity sheet, her character, the style guide, then the exchanges that show her
// manner -- including technical answers that lead with substance, which is the fix for
// a reply that was a metaphor and nothing further. The per-turn state packet follows
// it in the prompt and is not part of this.
//
// A profile without a packet renders as its system prompt alone, exactly as before.
// Deterministic: the same profile always renders identically, which is what lets the
// prefix cache keep it between turns.
[[nodiscard]] std::string RenderPersonaPacket(const aiProfile& profile);

// The two-to-four lines that bring her back to the present turn as herself after the
// record of a long conversation. Empty for a profile without a packet.
[[nodiscard]] std::string RenderPersonaAnchor(const aiProfile& profile);

} // namespace revia::identity
