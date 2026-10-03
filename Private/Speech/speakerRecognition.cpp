#include "Speech/speakerRecognition.h"

#include "Actions/actionTypes.h"
#include "Identity/relationshipRegistry.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <vector>

namespace revia::speech
{
namespace
{
std::uint32_t ReadLittle(const unsigned char* bytes, const std::size_t count)
{
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < count; ++index)
        result |= static_cast<std::uint32_t>(bytes[index]) << (8 * index);
    return result;
}

bool ValidFeatures(const SpeakerFeatures& features)
{
    double norm = 0;
    for (const float value : features.values)
    {
        if (!std::isfinite(value) || std::abs(value) > 1)
            return false;
        norm += static_cast<double>(value) * value;
    }
    return features.version == SpeakerFeatureVersion && norm > 0;
}

double Similarity(const SpeakerFeatures& left, const SpeakerFeatures& right)
{
    double dot = 0, leftNorm = 0, rightNorm = 0;
    for (std::size_t index = 0; index < SpeakerFeatureCount; ++index)
    {
        dot += static_cast<double>(left.values[index]) * right.values[index];
        leftNorm += static_cast<double>(left.values[index]) * left.values[index];
        rightNorm += static_cast<double>(right.values[index]) * right.values[index];
    }
    return dot / std::sqrt(leftNorm * rightNorm);
}
} // namespace

std::optional<SpeakerFeatures> WaveSpeakerRecognitionProvider::Extract(
    const std::filesystem::path& transientWave, const std::stop_token stopToken, std::string& outError) const
{
    outError = "The transient audio could not provide a bounded speaker feature sample.";
    std::error_code error;
    const auto size = std::filesystem::file_size(transientWave, error);
    if (error || size < 44 || size > 32 * 1024 * 1024 || stopToken.stop_requested())
        return std::nullopt;
    std::ifstream stream(transientWave, std::ios::binary);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return std::nullopt;
    const auto tag = [&](const std::size_t offset, const char* text)
    { return std::equal(text, text + 4, bytes.begin() + static_cast<std::ptrdiff_t>(offset)); };
    if (!tag(0, "RIFF") || !tag(8, "WAVE") || static_cast<std::uint64_t>(ReadLittle(bytes.data() + 4, 4)) + 8 != size)
        return std::nullopt;
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
    std::size_t data = 0, dataSize = 0;
    for (std::size_t offset = 12; offset + 8 <= bytes.size();)
    {
        const std::size_t chunk = ReadLittle(bytes.data() + offset + 4, 4);
        if (chunk > bytes.size() - offset - 8)
            return std::nullopt;
        if (tag(offset, "fmt "))
        {
            if (chunk < 16 || ReadLittle(bytes.data() + offset + 8, 2) != 1 || ReadLittle(bytes.data() + offset + 22, 2) != 16)
                return std::nullopt;
            channels = static_cast<std::uint16_t>(ReadLittle(bytes.data() + offset + 10, 2));
            rate = ReadLittle(bytes.data() + offset + 12, 4);
            if (channels < 1 || channels > 2 || rate < 8000 || rate > 48000 || ReadLittle(bytes.data() + offset + 20, 2) != channels * 2)
                return std::nullopt;
        }
        if (tag(offset, "data"))
        {
            data = offset + 8;
            dataSize = chunk;
        }
        offset += 8 + chunk + (chunk & 1);
    }
    if (!rate || !data || dataSize % (channels * 2) != 0)
        return std::nullopt;
    const auto frames = std::min<std::size_t>(dataSize / (channels * 2), static_cast<std::size_t>(rate) * 10);
    if (frames < rate / 4)
        return std::nullopt;
    std::array<double, SpeakerFeatureCount> energies{};
    constexpr std::size_t window = 256;
    double signalEnergy = 0;
    for (std::size_t begin = 0; begin + window <= frames; begin += window)
    {
        if (stopToken.stop_requested())
            return std::nullopt;
        std::array<double, window> samples{};
        double mean = 0;
        for (std::size_t frame = 0; frame < window; ++frame)
        {
            for (std::size_t channel = 0; channel < channels; ++channel)
            {
                const auto value =
                    static_cast<std::int16_t>(ReadLittle(bytes.data() + data + ((begin + frame) * channels + channel) * 2, 2));
                samples[frame] += static_cast<double>(value) / (32768 * channels);
            }
            mean += samples[frame] / window;
        }
        for (auto& sample : samples)
        {
            sample -= mean;
            signalEnergy += sample * sample;
        }
        for (std::size_t band = 0; band < SpeakerFeatureCount; ++band)
        {
            const double frequency = 120 * std::pow(1.22, static_cast<double>(band));
            const double coefficient = 2 * std::cos(2 * std::numbers::pi * frequency / rate);
            double previous = 0, previousTwo = 0;
            for (std::size_t frame = 0; frame < window; ++frame)
            {
                const double tapered = samples[frame] * (0.5 - 0.5 * std::cos(2 * std::numbers::pi * frame / (window - 1)));
                const double current = tapered + coefficient * previous - previousTwo;
                previousTwo = previous;
                previous = current;
            }
            energies[band] += std::max(0.0, previous * previous + previousTwo * previousTwo - coefficient * previous * previousTwo);
        }
    }
    if (signalEnergy / frames < 0.00001)
        return std::nullopt;
    double norm = 0;
    SpeakerFeatures features;
    for (std::size_t band = 0; band < SpeakerFeatureCount; ++band)
    {
        features.values[band] = static_cast<float>(std::sqrt(energies[band]));
        norm += energies[band];
    }
    if (norm <= 0 || stopToken.stop_requested())
        return std::nullopt;
    for (auto& value : features.values)
        value = static_cast<float>(value / std::sqrt(norm));
    outError.clear();
    return features;
}

SpeakerRecognition::SpeakerRecognition(
    identity::RelationshipRegistry& registry, std::shared_ptr<const ISpeakerRecognitionProvider> extractor)
    : identities(registry), provider(std::move(extractor))
{
}

bool SpeakerRecognition::Enroll(
    const std::string& entityId, const std::uint64_t consentRevision, const SpeakerFeatures& features, std::string& outError)
{
    return identities.SetSpeakerTemplate(entityId, consentRevision, features, outError);
}

bool SpeakerRecognition::Enroll(const std::string& entityId, const std::uint64_t consentRevision, const std::filesystem::path& wave,
    std::string& outError, const std::stop_token stopToken)
{
    outError = "Speaker enrollment requires current explicit consent and a usable transient sample.";
    if (!provider || identities.RecognitionConsentRevision(entityId) != consentRevision || stopToken.stop_requested())
        return false;
    try
    {
        std::string ignored;
        const auto features = provider->Extract(wave, stopToken, ignored);
        return features && !stopToken.stop_requested() && Enroll(entityId, consentRevision, *features, outError);
    }
    catch (...)
    {
        return false;
    }
}

void SpeakerRecognition::Forget(const std::string& entityId)
{
    std::string ignored;
    identities.RevokeRecognitionConsent(entityId, ignored);
    identities.DeleteSpeakerTemplate(entityId);
}

identity::SpeakerObservation SpeakerRecognition::Resolve(
    const SpeakerFeatures& features, const std::map<std::string, std::uint64_t>& approved) const
{
    if (!ValidFeatures(features))
        return {};
    std::optional<SpeakerTemplate> best;
    double first = -1, second = -1;
    for (const auto& candidate : identities.SpeakerTemplates())
    {
        const auto allowed = approved.find(candidate.entityId);
        if (allowed == approved.end() || allowed->second != candidate.consentRevision || !ValidFeatures(candidate.features))
            continue;
        const double similarity = Similarity(features, candidate.features);
        if (similarity > first)
        {
            second = first;
            first = similarity;
            best = candidate;
        }
        else
            second = std::max(second, similarity);
    }
    // These conservative feature thresholds are an uncalibrated continuity heuristic, never authority.
    if (!best || first < 0.92 || first - second < 0.03 || identities.RecognitionConsentRevision(best->entityId) != best->consentRevision)
        return {};
    return {best->entityId, identity::SpeakerSource::ConsentedVoice, actions::NewActionId(), best->consentRevision,
        static_cast<float>(std::clamp(first, 0.0, 1.0))};
}

identity::SpeakerObservation SpeakerRecognition::Observe(
    const std::filesystem::path& wave, const std::map<std::string, std::uint64_t>& approved, const std::stop_token stopToken) const
{
    if (!provider || approved.empty() || stopToken.stop_requested())
        return {};
    try
    {
        std::string ignored;
        const auto features = provider->Extract(wave, stopToken, ignored);
        return features && !stopToken.stop_requested() ? Resolve(*features, approved) : identity::SpeakerObservation{};
    }
    catch (...)
    {
        return {};
    }
}
} // namespace revia::speech
