#include "Vision/screenAwarenessAssessment.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace revia::vision
{

namespace
{
std::string BoundedText(const std::string& value, const std::size_t maximum)
{
    std::string bounded;
    bounded.reserve(std::min(value.size(), maximum));
    bool pendingSpace = false;
    for (const unsigned char character : value)
    {
        if (character == '\0' || (character < 0x20U && character != '\n' &&
                character != '\r' && character != '\t') || character == 0x7FU)
        {
            continue;
        }
        if (character == '\r')
        {
            continue;
        }
        if (character == ' ' || character == '\t')
        {
            pendingSpace = !bounded.empty() && bounded.back() != '\n';
            continue;
        }
        if (character == '\n')
        {
            while (!bounded.empty() && bounded.back() == ' ') bounded.pop_back();
            if (!bounded.empty() && bounded.back() != '\n') bounded.push_back('\n');
            pendingSpace = false;
            continue;
        }
        if (pendingSpace && bounded.size() < maximum)
        {
            bounded.push_back(' ');
        }
        pendingSpace = false;
        if (bounded.size() >= maximum) break;
        bounded.push_back(static_cast<char>(character));
    }
    while (!bounded.empty() && std::isspace(
        static_cast<unsigned char>(bounded.back())) != 0)
    {
        bounded.pop_back();
    }
    return bounded;
}

// Finds the first balanced object instead of using first/last brace. A visible log or
// editor may cause the model to put braces after its answer; those must not become part
// of the control object merely because they occurred later in the response.
std::string ExtractJsonObject(const std::string& raw)
{
    const std::size_t start = raw.find('{');
    if (start == std::string::npos) return {};

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t index = start; index < raw.size(); ++index)
    {
        const char character = raw[index];
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (character == '\\' && inString)
        {
            escaped = true;
            continue;
        }
        if (character == '"')
        {
            inString = !inString;
            continue;
        }
        if (inString) continue;
        if (character == '{')
        {
            ++depth;
        }
        else if (character == '}' && --depth == 0)
        {
            return raw.substr(start, index - start + 1);
        }
    }
    return {};
}
}

ScreenAwarenessAssessment ScreenAwarenessAssessmentParser::Parse(
    const std::string& response)
{
    ScreenAwarenessAssessment assessment;
    assessment.summary = BoundedText(response, MaximumSummaryCharacters);
    const std::string candidate = ExtractJsonObject(response);
    if (candidate.empty())
    {
        assessment.reason = "The screen assessment contained no complete JSON object.";
        return assessment;
    }

    try
    {
        const nlohmann::json document = nlohmann::json::parse(candidate);
        if (!document.is_object() || !document.contains("summary") ||
            !document["summary"].is_string() ||
            !document.contains("attention_required") ||
            !document["attention_required"].is_boolean() ||
            !document.contains("confidence") ||
            !document["confidence"].is_number() ||
            !document.contains("issue") || !document["issue"].is_string())
        {
            assessment.reason = "The screen assessment omitted a required typed field.";
            return assessment;
        }

        const double confidence = document["confidence"].get<double>();
        if (confidence < 0.0 || confidence > 1.0)
        {
            assessment.reason = "Screen issue confidence must be between zero and one.";
            return assessment;
        }

        assessment.summary = BoundedText(
            document["summary"].get<std::string>(), MaximumSummaryCharacters);
        assessment.attentionRequired =
            document["attention_required"].get<bool>();
        assessment.confidence = static_cast<float>(confidence);
        assessment.issue = BoundedText(
            document["issue"].get<std::string>(), MaximumIssueCharacters);
        if (assessment.summary.empty())
        {
            assessment.reason = "The screen assessment summary was empty.";
            assessment.attentionRequired = false;
            return assessment;
        }
        if (assessment.attentionRequired && assessment.issue.empty())
        {
            assessment.reason = "A screen assessment requested attention without an issue.";
            assessment.attentionRequired = false;
            return assessment;
        }

        assessment.valid = true;
        assessment.reason = "Structured screen assessment accepted.";
        return assessment;
    }
    catch (const std::exception& error)
    {
        assessment.reason = std::string("Invalid screen assessment JSON: ") + error.what();
        assessment.attentionRequired = false;
        return assessment;
    }
}

} // namespace revia::vision
