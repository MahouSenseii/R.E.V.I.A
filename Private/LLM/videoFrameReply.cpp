#include "LLM/videoFrameReply.h"

#include "Agents/replyFormat.h"
#include "Core/utf8.h"
#include "LLM/imagePromptGuidance.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

namespace revia::llm
{
namespace
{
using Json = nlohmann::ordered_json;
constexpr std::string_view FramePrefix = "Video frame at ";

bool ValidRegions(const std::vector<std::string>& regions)
{
    std::set<std::string> seen;
    return regions.size() <= 11 && std::all_of(regions.begin(), regions.end(),
                                       [&](const auto& region) { return IsVideoFrameRegion(region) && seen.insert(region).second; });
}

std::vector<std::string> HostTimestamps(const std::vector<conversationImage>& frames)
{
    if (frames.empty() || frames.size() > 4)
        return {};
    std::vector<std::string> timestamps;
    std::int64_t previous = -1;
    for (const auto& frame : frames)
    {
        if (!frame.bytes || frame.bytes->empty() || frame.mimeType != "image/png" || frame.width == 0 || frame.height == 0 ||
            frame.width > 768 || frame.height > 768 || !frame.name.starts_with(FramePrefix))
            return {};
        const auto time = std::string_view(frame.name).substr(FramePrefix.size());
        if (time.size() != 9 || time[2] != ':' || time[5] != '.')
            return {};
        for (std::size_t index = 0; index < time.size(); ++index)
            if (index != 2 && index != 5 && (time[index] < '0' || time[index] > '9'))
                return {};
        const auto seconds = (time[3] - '0') * 10 + time[4] - '0';
        const auto milliseconds =
            ((time[0] - '0') * 10 + time[1] - '0') * 60000 + seconds * 1000 + (time[6] - '0') * 100 + (time[7] - '0') * 10 + time[8] - '0';
        if (seconds >= 60 || milliseconds >= 60000 || milliseconds <= previous)
            return {};
        previous = milliseconds;
        timestamps.emplace_back(time);
    }
    return timestamps;
}

bool BoundedText(const Json& value, const std::size_t maxCharacters)
{
    if (!value.is_string())
        return false;
    const auto& text = value.get_ref<const std::string&>();
    if (!utf8::IsValid(text) || std::any_of(text.begin(), text.end(), [](const unsigned char byte)
                                    { return (byte < 32 && byte != '\r' && byte != '\n' && byte != '\t') || byte == 127; }))
        return false;
    std::size_t characters = 0;
    for (std::size_t at = 0; at < text.size(); at += utf8::SequenceAt(text, at))
        if (++characters > maxCharacters)
            return false;
    return true;
}

std::string InlineText(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "unclear from this sample";
    text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    for (auto& character : text)
        if (character == '\r' || character == '\n' || character == '\t')
            character = ' ';
    return text;
}
}

std::string BuildVideoFrameReplySchema(const std::vector<conversationImage>& frames,
    const std::vector<std::string>& requestedRegions, std::string& outError)
{
    outError.clear();
    if (HostTimestamps(frames).size() != frames.size() || frames.empty() || !ValidRegions(requestedRegions))
    {
        outError = "The sampled-video reply requires one to four current, ordered native PNG frames and supported region keys.";
        return {};
    }
    Json schema = {{"type", "object"}, {"properties", Json::object()}, {"required", Json::array()}, {"additionalProperties", false}};
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        Json item = {{"type", "object"},
            {"properties", {{"literal_text", {{"type", "string"}, {"maxLength", 2048}}},
                               {"requested_visible_facts", {{"type", "string"}, {"maxLength", 512}}}}},
            {"required", Json::array({"literal_text", "requested_visible_facts"})}, {"additionalProperties", false}};
        if (!requestedRegions.empty())
        {
            Json regions = {
                {"type", "object"}, {"properties", Json::object()}, {"required", requestedRegions}, {"additionalProperties", false}};
            for (const auto& region : requestedRegions)
                regions["properties"][region] = {{"type", "string"}, {"maxLength", 512}};
            item["properties"]["requested_regions"] = std::move(regions);
            item["required"].push_back("requested_regions");
        }
        const auto key = "image_" + std::to_string(index + 1);
        schema["properties"][key] = std::move(item);
        schema["required"].push_back(key);
    }
    return schema.dump();
}

VideoFrameReplyResult ParseVideoFrameReply(const std::string_view reply, const std::vector<conversationImage>& frames,
    const VideoFrameReplyOptions& options)
{
    VideoFrameReplyResult result;
    const auto fail = [&]()
    {
        result.text.clear();
        result.error = "The local model did not return a complete, valid bounded frame review. No partial review was delivered.";
        return result;
    };
    const auto timestamps = HostTimestamps(frames);
    if (timestamps.empty() || !ValidRegions(options.requestedRegions) ||
        (!options.includeLiteralText && !options.includeVisibleFacts && options.requestedRegions.empty()) ||
        reply.size() > MaximumVideoFrameReplyBytes || !agents::IsCompleteJsonReply(std::string(reply), MaximumVideoFrameReplyBytes))
        return fail();
    try
    {
        const auto parsed = Json::parse(reply);
        if (!parsed.is_object() || parsed.size() != frames.size())
            return fail();
        for (std::size_t index = 0; index < frames.size(); ++index)
        {
            const auto key = "image_" + std::to_string(index + 1);
            if (!parsed.contains(key) || !parsed.at(key).is_object())
                return fail();
            const auto& item = parsed.at(key);
            if (item.size() != (options.requestedRegions.empty() ? 2U : 3U) || !item.contains("literal_text") ||
                !item.contains("requested_visible_facts") || !BoundedText(item.at("literal_text"), 2048) ||
                !BoundedText(item.at("requested_visible_facts"), 512))
                return fail();
            if (!options.requestedRegions.empty())
            {
                if (!item.contains("requested_regions") || !item.at("requested_regions").is_object() ||
                    item.at("requested_regions").size() != options.requestedRegions.size())
                    return fail();
                for (const auto& region : options.requestedRegions)
                    if (!item.at("requested_regions").contains(region) || !BoundedText(item.at("requested_regions").at(region), 512))
                        return fail();
            }
        }
        for (std::size_t index = 0; index < frames.size(); ++index)
        {
            const auto& item = parsed.at("image_" + std::to_string(index + 1));
            std::string row = "- " + timestamps[index] + ": ";
            std::vector<std::string> details;
            if (options.includeLiteralText)
            {
                const auto& literal = item.at("literal_text").get_ref<const std::string&>();
                details.push_back(literal.find_first_not_of(" \t\r\n") == std::string::npos ? "Text: unclear from this sample"
                                                                                            : "Text: " + Json(literal).dump());
            }
            for (const auto& region : options.requestedRegions)
                details.push_back(region + ": " + InlineText(item.at("requested_regions").at(region).get<std::string>()));
            if (options.includeVisibleFacts)
                details.push_back(InlineText(item.at("requested_visible_facts").get<std::string>()));
            for (std::size_t detail = 0; detail < details.size(); ++detail)
                row += (detail == 0 ? "" : "; ") + details[detail];
            result.text += (result.text.empty() ? "" : "\n") + row;
        }
        result.text +=
            "\n\nSampling limitation: Only these " + std::to_string(frames.size()) +
            " sampled still frames were inspected. Continuous motion, exact event timing, and events between samples are not established.";
        result.ok = true;
        return result;
    }
    catch (const std::exception&)
    {
        return fail();
    }
}
}
