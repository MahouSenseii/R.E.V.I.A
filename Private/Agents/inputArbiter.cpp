#include "Agents/inputArbiter.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace revia::agents
{

std::string ToString(const InputVerdict value)
{
    switch (value)
    {
        case InputVerdict::Queued: return "queued";
        case InputVerdict::IgnoredEmpty: return "ignored (empty)";
        case InputVerdict::IgnoredNoise: return "ignored (noise)";
        case InputVerdict::IgnoredDuplicate: return "ignored (repeat)";
        case InputVerdict::DroppedOverflow: return "dropped (queue full)";
    }
    return "queued";
}

InputArbiter::InputArbiter(inputArbiterSettings settings)
    : configuration(std::move(settings))
{
}

std::string InputArbiter::Normalize(const std::string& text)
{
    std::string normalized;
    normalized.reserve(text.size());
    for (const unsigned char character : text)
    {
        if (std::isalnum(character) != 0)
        {
            normalized.push_back(static_cast<char>(std::tolower(character)));
        }
        else if (std::isspace(character) != 0 && !normalized.empty() &&
            normalized.back() != ' ')
        {
            normalized.push_back(' ');
        }
    }
    while (!normalized.empty() && normalized.back() == ' ')
    {
        normalized.pop_back();
    }
    return normalized;
}

bool InputArbiter::IsNoise(const inputArbiterSettings& settings, const std::string& text)
{
    const std::string normalized = Normalize(text);
    if (normalized.empty())
    {
        return true;
    }
    for (const std::string& fragment : settings.ignoredFragments)
    {
        if (normalized == Normalize(fragment))
        {
            return true;
        }
    }
    // Short and not a question is almost always the recogniser hearing the room. A short
    // question is a real one, so "why?" survives where "uh" does not.
    if (normalized.size() < static_cast<std::size_t>(settings.minimumMeaningfulCharacters) &&
        text.find('?') == std::string::npos)
    {
        return true;
    }
    return false;
}

InputVerdict InputArbiter::Offer(
    const std::string& text,
    const InputSource source,
    const std::chrono::system_clock::time_point now)
{
    std::lock_guard lock(mutex);
    const std::string normalized = Normalize(text);
    if (normalized.empty())
    {
        return InputVerdict::IgnoredEmpty;
    }

    // Typed input bypasses the noise filter entirely. Filtering something a person
    // deliberately typed is a far worse failure than answering one stray "hmm".
    if (source != InputSource::Typed && IsNoise(configuration, text))
    {
        return InputVerdict::IgnoredNoise;
    }

    // Recognisers commonly emit the same phrase twice from one utterance.
    if (source != InputSource::Typed && normalized == lastAccepted &&
        now - lastAcceptedAt < std::chrono::seconds(8))
    {
        return InputVerdict::IgnoredDuplicate;
    }
    const auto duplicateInQueue = std::find_if(
        queued.begin(),
        queued.end(),
        [&normalized](const PendingInput& pending)
        {
            return Normalize(pending.text) == normalized;
        });
    if (duplicateInQueue != queued.end())
    {
        return InputVerdict::IgnoredDuplicate;
    }

    if (queued.size() >= static_cast<std::size_t>(configuration.maxQueuedInputs))
    {
        return InputVerdict::DroppedOverflow;
    }

    queued.push_back({text, source, now});
    lastAccepted = normalized;
    lastAcceptedAt = now;
    return InputVerdict::Queued;
}

bool InputArbiter::IsReady(const std::chrono::system_clock::time_point now) const
{
    std::lock_guard lock(mutex);
    if (queued.empty())
    {
        return false;
    }
    // Ready once nothing new has arrived for the merge window. Someone speaking in bursts
    // gets one reply to the whole thought rather than one per pause.
    return now - queued.back().receivedAt >=
        std::chrono::milliseconds(configuration.mergeWindowMs);
}

std::string InputArbiter::Take()
{
    std::lock_guard lock(mutex);
    std::ostringstream stream;
    for (std::size_t index = 0; index < queued.size(); ++index)
    {
        std::string text = queued[index].text;
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        {
            text.pop_back();
        }
        if (text.empty())
        {
            continue;
        }
        if (index > 0)
        {
            // Joined so the model reads it as one turn. A separator that ends the previous
            // fragment keeps two statements from fusing into one malformed sentence.
            const char last = text.empty() ? ' ' : stream.str().back();
            stream << (last == '.' || last == '!' || last == '?' ? " " : ". ");
        }
        stream << text;
    }
    queued.clear();
    return stream.str();
}

std::size_t InputArbiter::Size() const
{
    std::lock_guard lock(mutex);
    return queued.size();
}

void InputArbiter::Clear()
{
    std::lock_guard lock(mutex);
    queued.clear();
}

} // namespace revia::agents
