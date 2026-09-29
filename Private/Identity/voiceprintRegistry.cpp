#include "Identity/voiceprintRegistry.h"

#include "Identity/relationshipEvidence.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <nlohmann/json.hpp>
#include <sstream>

namespace revia::identity
{

namespace
{
using json = nlohmann::json;

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool Finite(const std::vector<float>& values)
{
    return std::all_of(values.begin(), values.end(), [](const float value) { return std::isfinite(value); });
}
} // namespace

VoiceprintRegistry::VoiceprintRegistry(const float inputThreshold, const float inputMargin, const std::size_t inputMaximum)
    : threshold(inputThreshold), margin(inputMargin), maximumSamples(std::max<std::size_t>(1, inputMaximum))
{
}

void VoiceprintRegistry::Configure(const float inputThreshold, const float inputMargin)
{
    std::lock_guard lock(mutex);
    threshold = std::clamp(inputThreshold, 0.0F, 1.0F);
    margin = std::clamp(inputMargin, 0.0F, 1.0F);
}

std::vector<float> VoiceprintRegistry::Normalized(const std::vector<float>& embedding)
{
    double norm = 0.0;
    for (const float value : embedding) norm += static_cast<double>(value) * value;
    norm = std::sqrt(norm);
    std::vector<float> unit(embedding.size(), 0.0F);
    if (norm <= 0.0) return unit;
    for (std::size_t index = 0; index < embedding.size(); ++index)
    {
        unit[index] = static_cast<float>(embedding[index] / norm);
    }
    return unit;
}

float VoiceprintRegistry::Cosine(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.empty() || a.size() != b.size()) return -1.0F;
    double dot = 0.0;
    double normA = 0.0;
    double normB = 0.0;
    for (std::size_t index = 0; index < a.size(); ++index)
    {
        dot += static_cast<double>(a[index]) * b[index];
        normA += static_cast<double>(a[index]) * a[index];
        normB += static_cast<double>(b[index]) * b[index];
    }
    if (normA <= 0.0 || normB <= 0.0) return -1.0F;
    return static_cast<float>(dot / (std::sqrt(normA) * std::sqrt(normB)));
}

std::size_t VoiceprintRegistry::Enroll(
    const std::string& entityId,
    const std::string& displayName,
    const std::vector<float>& embedding,
    const std::string& consentedAt,
    std::string& outError)
{
    if (entityId.empty())
    {
        outError = "A voiceprint needs a person to belong to.";
        return 0;
    }
    if (embedding.size() < 8 || !Finite(embedding))
    {
        outError = "The embedding is too short or not a number.";
        return 0;
    }
    const std::vector<float> unit = Normalized(embedding);
    if (std::all_of(unit.begin(), unit.end(), [](const float value) { return value == 0.0F; }))
    {
        outError = "The embedding is all zeros.";
        return 0;
    }
    std::lock_guard lock(mutex);
    for (const Voiceprint& print : prints)
    {
        if (print.entityId != entityId && !print.samples.empty() && print.samples.front().size() != unit.size())
        {
            outError = "The embedding's size differs from the prints already kept; the model changed.";
            return 0;
        }
    }
    auto found = std::find_if(prints.begin(), prints.end(),
        [&](const Voiceprint& print) { return print.entityId == entityId; });
    if (found == prints.end())
    {
        Voiceprint print;
        print.entityId = entityId;
        print.displayName = displayName;
        print.consentedAt = consentedAt;
        prints.push_back(std::move(print));
        found = prints.end() - 1;
    }
    if (!displayName.empty()) found->displayName = displayName;
    if (found->consentedAt.empty()) found->consentedAt = consentedAt;
    found->samples.push_back(unit);
    while (found->samples.size() > maximumSamples) found->samples.erase(found->samples.begin());
    return found->samples.size();
}

bool VoiceprintRegistry::Forget(const std::string& entityId)
{
    std::lock_guard lock(mutex);
    const auto before = prints.size();
    prints.erase(std::remove_if(prints.begin(), prints.end(),
        [&](const Voiceprint& print) { return print.entityId == entityId; }), prints.end());
    return prints.size() != before;
}

void VoiceprintRegistry::Clear()
{
    std::lock_guard lock(mutex);
    prints.clear();
}

SpeakerMatch VoiceprintRegistry::Match(const std::vector<float>& embedding) const
{
    SpeakerMatch match;
    std::lock_guard lock(mutex);
    if (prints.empty())
    {
        match.reason = "no voice is enrolled";
        return match;
    }
    if (embedding.size() < 8 || !Finite(embedding))
    {
        match.reason = "the embedding is unusable";
        return match;
    }
    const std::vector<float> unit = Normalized(embedding);
    float best = -1.0F;
    float second = -1.0F;
    const Voiceprint* nearest = nullptr;
    for (const Voiceprint& print : prints)
    {
        // The print's score is its best sample: one clean utterance that matches is
        // worth more than an average dragged down by a noisy one.
        float score = -1.0F;
        for (const std::vector<float>& sample : print.samples)
        {
            score = std::max(score, Cosine(unit, sample));
        }
        if (score > best)
        {
            second = best;
            best = score;
            nearest = &print;
        }
        else if (score > second)
        {
            second = score;
        }
    }
    if (nearest == nullptr)
    {
        match.reason = "no print could be scored";
        return match;
    }
    match.score = best;
    match.margin = second < 0.0F ? best + 1.0F : best - second;
    match.nearest = nearest->displayName.empty() ? nearest->entityId : nearest->displayName;
    std::ostringstream reason;
    if (best < threshold)
    {
        reason << "nearest " << match.nearest << " at " << best << ", below " << threshold;
        match.reason = reason.str();
        return match;
    }
    if (prints.size() > 1 && match.margin < margin)
    {
        reason << match.nearest << " at " << best << " but only " << match.margin << " ahead of the next";
        match.reason = reason.str();
        return match;
    }
    match.matched = true;
    match.entityId = nearest->entityId;
    match.displayName = nearest->displayName;
    reason << match.nearest << " at " << best;
    match.reason = reason.str();
    return match;
}

std::vector<Voiceprint> VoiceprintRegistry::All() const
{
    std::lock_guard lock(mutex);
    return prints;
}

std::size_t VoiceprintRegistry::Count() const
{
    std::lock_guard lock(mutex);
    return prints.size();
}

std::string VoiceprintRegistry::Serialize() const
{
    std::lock_guard lock(mutex);
    json data;
    data["version"] = 1;
    json list = json::array();
    for (const Voiceprint& print : prints)
    {
        list.push_back({{"entityId", print.entityId}, {"displayName", print.displayName},
            {"consentedAt", print.consentedAt}, {"samples", print.samples}});
    }
    data["voiceprints"] = list;
    return data.dump();
}

bool VoiceprintRegistry::Deserialize(const std::string& text, std::string& outError)
{
    json data;
    try
    {
        data = json::parse(text);
    }
    catch (const std::exception& error)
    {
        outError = std::string("The voiceprints are not JSON: ") + error.what();
        return false;
    }
    if (!data.is_object() || !data.contains("voiceprints") || !data["voiceprints"].is_array())
    {
        outError = "The voiceprints file has no list.";
        return false;
    }
    std::vector<Voiceprint> loaded;
    try
    {
        for (const json& item : data["voiceprints"])
        {
            Voiceprint print;
            print.entityId = item.value("entityId", "");
            print.displayName = item.value("displayName", "");
            print.consentedAt = item.value("consentedAt", "");
            for (const json& sample : item.value("samples", json::array()))
            {
                std::vector<float> values = sample.get<std::vector<float>>();
                if (values.size() >= 8 && Finite(values)) print.samples.push_back(Normalized(values));
            }
            if (!print.entityId.empty() && !print.samples.empty()) loaded.push_back(std::move(print));
        }
    }
    catch (const std::exception& error)
    {
        outError = std::string("A voiceprint could not be read: ") + error.what();
        return false;
    }
    std::lock_guard lock(mutex);
    prints = std::move(loaded);
    return true;
}

std::string VoiceprintRegistry::Describe() const
{
    std::lock_guard lock(mutex);
    if (prints.empty()) return "No voice is enrolled.";
    std::ostringstream text;
    for (const Voiceprint& print : prints)
    {
        if (!text.str().empty()) text << '\n';
        text << (print.displayName.empty() ? print.entityId : print.displayName) << " (" << print.entityId << "): "
             << print.samples.size() << (print.samples.size() == 1 ? " sample" : " samples")
             << (print.consentedAt.empty() ? "" : ", asked for on " + print.consentedAt);
    }
    text << "\nMatch threshold " << threshold << ", margin " << margin << ".";
    return text.str();
}

bool AsksToEnrollVoice(const std::string& input)
{
    const std::string lowered = Lower(input);
    static const char* markers[] = {
        "remember my voice", "learn my voice", "enroll my voice", "enrol my voice",
        "recognise my voice", "recognize my voice", "know my voice", "memorize my voice", "memorise my voice"};
    return std::any_of(std::begin(markers), std::end(markers),
        [&lowered](const char* marker) { return lowered.find(marker) != std::string::npos; });
}

std::string ReadVoiceEnrollmentName(const std::string& input)
{
    const std::string stated = ReadStatedName(input);
    if (!stated.empty()) return stated;
    const std::string lowered = Lower(input);
    static const char* openers[] = {"i'm ", "i am ", "it's ", "it is ", "this is "};
    for (const char* opener : openers)
    {
        const std::size_t length = std::string(opener).size();
        for (std::size_t at = lowered.find(opener); at != std::string::npos; at = lowered.find(opener, at + 1))
        {
            if (at > 0 && std::isalpha(static_cast<unsigned char>(lowered[at - 1])) != 0) continue;
            const std::size_t start = at + length;
            if (start >= input.size() || std::isupper(static_cast<unsigned char>(input[start])) == 0) continue;
            std::string name;
            for (std::size_t index = start; index < input.size() && name.size() < 32; ++index)
            {
                const unsigned char character = static_cast<unsigned char>(input[index]);
                if (std::isalpha(character) != 0 || character == '-' || character == '\'')
                {
                    name.push_back(input[index]);
                    continue;
                }
                break;
            }
            if (name.size() >= 2) return name;
        }
    }
    return {};
}

bool AsksToForgetVoice(const std::string& input)
{
    const std::string lowered = Lower(input);
    static const char* markers[] = {"forget my voice", "delete my voice", "remove my voice", "unlearn my voice"};
    return std::any_of(std::begin(markers), std::end(markers),
        [&lowered](const char* marker) { return lowered.find(marker) != std::string::npos; });
}

} // namespace revia::identity
