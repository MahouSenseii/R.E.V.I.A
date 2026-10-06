#pragma once

#include <string>

namespace revia::visual
{

enum class DrawingIntent
{
    None,
    Diagram,
    Raster
};

// Deterministically recognizes conversational drawing requests without a model classification call.
class DrawingRequestPolicy
{
public:
  [[nodiscard]] static DrawingIntent Classify(const std::string& input);
  [[nodiscard]] static bool ShouldDraw(const std::string& input);

  // The drawing request with its framing removed, so "can you draw me a diagram of the
  // turn path" asks for "the turn path" rather than for the phrase "can you draw me".
  [[nodiscard]] static std::string ExtractSubject(const std::string& input);
};

} // namespace revia::visual
