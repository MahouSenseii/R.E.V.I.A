#pragma once

#include <cstddef>
#include <string>

namespace revia::vision
{

// One bounded interpretation of an ambient multi-monitor capture.
//
// The summary is ordinary visual context. Attention is deliberately separate: seeing
// an error-like word is not, by itself, permission to interrupt the user. The caller
// still applies its configured confidence threshold and the ordinary initiative policy.
struct ScreenAwarenessAssessment
{
    bool valid = false;
    std::string summary;
    bool attentionRequired = false;
    float confidence = 0.0F;
    std::string issue;
    std::string reason;
};

class ScreenAwarenessAssessmentParser
{
public:
    static constexpr std::size_t MaximumSummaryCharacters = 1800;
    static constexpr std::size_t MaximumIssueCharacters = 400;

    // Accepts one JSON object, optionally surrounded by a short preamble or a code
    // fence. On malformed output, summary retains a bounded copy of the model response
    // so ordinary awareness still works, but attention always fails closed.
    [[nodiscard]] static ScreenAwarenessAssessment Parse(const std::string& response);
};

} // namespace revia::vision
