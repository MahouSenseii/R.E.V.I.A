#include "LLM/videoFrameReply.h"
#include "LLM/imagePromptGuidance.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <stdexcept>

namespace
{
using Json = nlohmann::json;
using namespace revia::llm;

void Check(const bool passed, const std::string& reason)
{
    if (!passed)
        throw std::runtime_error(reason);
}

std::vector<conversationImage> Frames()
{
    const auto pixels = std::make_shared<const std::string>("already-admitted-fixture-pixels");
    return {{"Video frame at 00:11.500", "image/png", pixels, 640, 360}, {"Video frame at 00:11.900", "image/png", pixels, 640, 360},
        {"Video frame at 00:12.200", "image/png", pixels, 640, 360}, {"Video frame at 00:12.500", "image/png", pixels, 640, 360}};
}

Json Reply(const std::vector<std::string>& regions = {})
{
    Json reply = Json::object();
    for (int index = 1; index <= 4; ++index)
    {
        Json item = {{"literal_text", "TEST-LABEL"}, {"requested_visible_facts", "OUTER_GEOMETRY_FIXTURE"}};
        if (!regions.empty())
        {
            item["requested_regions"] = Json::object();
            for (const auto& region : regions)
                item["requested_regions"][region] = "OBSERVATION_" + region + "_" + std::to_string(index);
        }
        reply["image_" + std::to_string(index)] = std::move(item);
    }
    return reply;
}

void TestQualifiedSchemaAndHostRendering()
{
    std::string error;
    const auto schemaText = BuildVideoFrameReplySchema(Frames(), {"center"}, error);
    Check(!schemaText.empty() && error.empty(), "Valid admitted frame metadata did not produce a bounded schema.");
    const auto schema = Json::parse(schemaText);
    Check(schema.at("required") == Json::array({"image_1", "image_2", "image_3", "image_4"}) &&
              schema.at("additionalProperties") == false && schema.at("properties").size() == 4,
        "The schema lost its exact supplied-frame ordinal contract.");
    for (const auto& [key, child] : schema.at("properties").items())
    {
        Check(child.at("required") == Json::array({"literal_text", "requested_visible_facts", "requested_regions"}) &&
                  child.at("additionalProperties") == false && child.at("properties").size() == 3 &&
                  child.at("properties").at("literal_text").at("maxLength") == 2048 &&
                  child.at("properties").at("requested_visible_facts").at("maxLength") == 512 &&
                  child.at("properties").at("requested_regions").at("required") == Json::array({"center"}) &&
                  child.at("properties").at("requested_regions").at("additionalProperties") == false &&
                  child.at("properties").at("requested_regions").at("properties").at("center").at("maxLength") == 512,
            "The production schema differs from the qualified bounded fields/region shape: " + key);
    }
    const auto rendered = ParseVideoFrameReply(Reply({"center"}).dump(), Frames(), {{"center"}, true, false});
    Check(rendered.ok && rendered.error.empty(), "A complete schema-conforming frame reply was refused.");
    for (const auto* timestamp : {"00:11.500", "00:11.900", "00:12.200", "00:12.500"})
        Check(rendered.text.find(timestamp) != std::string::npos, "Rendering omitted a supplied host frame timestamp.");
    Check(rendered.text.find("TEST-LABEL") != std::string::npos && rendered.text.find("OBSERVATION_center_4") != std::string::npos &&
              rendered.text.find("OUTER_GEOMETRY_FIXTURE") == std::string::npos &&
              rendered.text.find("Sampling limitation:") != std::string::npos &&
              rendered.text.find("between samples") != std::string::npos && rendered.text.find("audio") == std::string::npos &&
              rendered.text.find("image_1") == std::string::npos,
        "Rendering exposed internal JSON, unrequested outer facts, unsupported audio state or lost sampled-coverage limits.");
    const auto noOcr = ParseVideoFrameReply(Reply().dump(), Frames(), {{}, false, true});
    Check(noOcr.ok && noOcr.text.find("TEST-LABEL") == std::string::npos && noOcr.text.find("OUTER_GEOMETRY_FIXTURE") != std::string::npos,
        "The renderer ignored requested OCR/visible-facts selection.");
    const auto mixed = ParseVideoFrameReply(Reply({"left", "upper-right"}).dump(), Frames(), {{"left", "upper-right"}, true, true});
    Check(mixed.ok && mixed.text.find("OBSERVATION_left_1") != std::string::npos &&
              mixed.text.find("OBSERVATION_upper-right_4") != std::string::npos &&
              mixed.text.find("OUTER_GEOMETRY_FIXTURE") != std::string::npos,
        "A mixed explicit region and general-facts request lost one admitted output field.");
}

void TestStrictCompleteReplyValidation()
{
    const auto original = Reply({"center"});
    const auto refused = [&](const std::string& candidate)
    {
        const auto result = ParseVideoFrameReply(candidate, Frames(), {{"center"}, true, false});
        Check(!result.ok && result.text.empty() && !result.error.empty(), "Invalid or partial frame JSON escaped as a successful reply.");
    };
    for (const auto* text : {"", "{}", "[]", "```json\n{}\n```", "{", "{\"image_1\":{},\"image_1\":{}}"})
        refused(text);
    auto value = original;
    value.erase("image_4");
    refused(value.dump());
    value = original;
    value["image_5"] = value["image_1"];
    refused(value.dump());
    for (const auto* key : {"literal_text", "requested_visible_facts", "requested_regions"})
    {
        value = original;
        value["image_2"].erase(key);
        refused(value.dump());
        value["image_2"][key] = 7;
        refused(value.dump());
    }
    value = original;
    value["image_1"]["unknown"] = "extra";
    refused(value.dump());
    value = original;
    value["image_1"]["requested_regions"]["left"] = "unrequested";
    refused(value.dump());
    auto duplicate = original.dump();
    duplicate.insert(1, "\"image_1\":" + original["image_1"].dump() + ",");
    refused(duplicate);
    duplicate = original.dump();
    const auto at = duplicate.find("\"literal_text\":");
    duplicate.insert(at, "\"literal_text\":\"duplicate\",");
    refused(duplicate);
    duplicate = original.dump();
    duplicate.insert(duplicate.find("\"center\":"), "\"center\":\"duplicate\",");
    refused(duplicate);
    refused(original.dump() + " trailing text");
    refused(std::string(MaximumVideoFrameReplyBytes + 1, 'x'));
    auto invalidUtf8 = original.dump();
    invalidUtf8[invalidUtf8.find("TEST-LABEL")] = static_cast<char>(255);
    refused(invalidUtf8);
    value = original;
    value["image_3"]["literal_text"] = std::string("hidden\0value", 12);
    refused(value.dump());
    for (const auto* field : {"literal_text", "requested_visible_facts"})
    {
        value = original;
        value["image_3"][field] = std::string(std::string(field) == "literal_text" ? 2049 : 513, 'x');
        refused(value.dump());
    }
    value = original;
    value["image_3"]["requested_regions"]["center"] = std::string(513, 'x');
    refused(value.dump());
    value = original;
    std::string unicode;
    for (int index = 0; index < 2048; ++index)
        unicode += "\xC3\xA9";
    value["image_1"]["literal_text"] = unicode;
    Check(ParseVideoFrameReply(value.dump(), Frames(), {{"center"}, true, false}).ok,
        "A maxLength-compliant UTF-8 OCR string was incorrectly limited by its byte count.");
    value["image_1"]["literal_text"] = unicode + "\xC3\xA9";
    refused(value.dump());
    value = original;
    value["image_1"]["literal_text"] = "";
    value["image_1"]["requested_regions"]["center"] = " \t";
    const auto uncertain = ParseVideoFrameReply(value.dump(), Frames(), {{"center"}, true, false});
    Check(uncertain.ok && uncertain.text.find("unclear") != std::string::npos,
        "Empty admitted observation strings silently became a confident blank-region claim.");
    value = original;
    value["image_2"]["requested_regions"]["center"] = "UNVERIFIED_VISUAL_CLAIM_FIXTURE";
    const auto unverified = ParseVideoFrameReply(value.dump(), Frames(), {{"center"}, true, false});
    Check(unverified.ok && unverified.text.find("UNVERIFIED_VISUAL_CLAIM_FIXTURE") != std::string::npos,
        "Structural parsing silently altered an unverified model claim instead of leaving semantic quality to independent acceptance.");
}

void TestHostFrameAndRegionBounds()
{
    for (int control = 0; control < 8; ++control)
    {
        auto frames = Frames();
        std::vector<std::string> regions{"center"};
        if (control == 0)
            frames.clear();
        if (control == 1)
            frames.push_back(frames.back());
        if (control == 2)
            frames[0].name = "Model-selected time 00:01.000";
        if (control == 3)
            frames[1].name = frames[0].name;
        if (control == 4)
            frames[0].bytes.reset();
        if (control == 5)
            frames[0].name = "Video frame at 01:00.000";
        if (control == 6)
            regions = {"arbitrary-user-schema-key"};
        if (control == 7)
            regions = {"center", "center"};
        std::string error;
        Check(BuildVideoFrameReplySchema(frames, regions, error).empty() && !error.empty(),
            "Unbounded/stale host frame metadata or arbitrary region keys entered the schema.");
        Check(!ParseVideoFrameReply(Reply({"center"}).dump(), frames, {regions, true, false}).ok,
            "Unbounded/stale host frame metadata or arbitrary region keys entered rendered output.");
    }
}

void TestQuestionFieldsRemainScopedToRequestedVisualDetails()
{
    Check(RequestedVideoFrameRegions("Read the upper-left and lower-right labels, then inspect the center.") ==
                  std::vector<std::string>({"upper-left", "lower-right", "center"}) &&
              RequestedVideoFrameRegions("Read the words \"center\" and \"left\".").empty(),
        "Canonical region fields were inferred from quoted content or lost their requested order.");
    Check(RequestsVideoFrameLiteralText("Read the literal label at each sampled time.") &&
              RequestsVideoFrameLiteralText("Transcribe the on-screen text.") &&
              !RequestsVideoFrameLiteralText("What happens around 00:12?") && !RequestsVideoFrameLiteralText("Identify the center shape."),
        "The native OCR renderer ignored whether the question requested visible text.");
    Check(!RequestsVideoFrameOverview("Inspect the center at every sampled timestamp.") &&
              RequestsVideoFrameOverview("Inspect the center and list all other shapes.") &&
              !RequestsVideoFrameOverview("Inspect the center; do not include other geometry.") &&
              !RequestsVideoFrameOverview("Read the upper-left and lower-right labels."),
        "A region-only question was expanded to unrequested or explicitly excluded whole-frame facts.");
    Check(AllowsVideoFrameReplyFormat("Read the label in your own voice.") &&
              AllowsVideoFrameReplyFormat("Read the word \"audio\" from the frame.") &&
              !AllowsVideoFrameReplyFormat("Describe the center in CSV.") &&
              !AllowsVideoFrameReplyFormat("Describe the sound in the clip.") &&
              !AllowsVideoFrameReplyFormat("Read the label and how do you feel?") &&
              !AllowsVideoFrameReplyFormat("Read what you called the center earlier."),
        "Frame formatting overrode a delivery preference, audio request, prior-state question or explicit user format.");
}
}

void RunVideoFrameReplyTests()
{
    TestQualifiedSchemaAndHostRendering();
    TestStrictCompleteReplyValidation();
    TestHostFrameAndRegionBounds();
    TestQuestionFieldsRemainScopedToRequestedVisualDetails();
    std::cout << "Bounded video frame schema, strict complete parsing and host-metadata rendering checks passed.\n";
}

#ifdef REVIA_VIDEO_FRAME_REPLY_TEST_MAIN
int main()
{
    try
    {
        RunVideoFrameReplyTests();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
