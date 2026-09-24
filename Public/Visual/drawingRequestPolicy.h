#pragma once

#include <string>

namespace revia::visual
{

// Recognizes a request to draw, deterministically.
//
// Asking Revia to sketch something in conversation should draw it. Requiring /draw makes
// the capability exist only for someone who already knows it exists, which is the same
// failure as an assistant that can do a thing but never offers to.
//
// The same shape as InternetLookupPolicy and for the same reason: whether an expensive
// capability runs is decided by deterministic code, not by asking the model to decide
// whether it should call itself. A recognizer can be read, tested, and corrected; a
// model's self-assessment can only be re-prompted.
class DrawingRequestPolicy
{
public:
    [[nodiscard]] static bool ShouldDraw(const std::string& input);

    // The drawing request with its framing removed, so "can you draw me a diagram of the
    // turn path" asks for "the turn path" rather than for the phrase "can you draw me".
    [[nodiscard]] static std::string ExtractSubject(const std::string& input);
};

} // namespace revia::visual
