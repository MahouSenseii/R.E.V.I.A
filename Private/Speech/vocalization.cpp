#include "Speech/vocalization.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

namespace revia::speech
{

namespace
{
using revia::runtime::AffectState;

std::string Lowered(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool HasSpeakableContent(const std::string& value)
{
    // Punctuation alone is not worth a synthesis round trip, and a lone "." handed to
    // the TTS comes back as a click or a spoken word depending on the model.
    return std::any_of(value.begin(), value.end(), [](const unsigned char character)
    {
        return std::isalnum(character) != 0;
    });
}

std::string Trimmed(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// Collapses the runs of spaces left behind where a tag used to be.
std::string CollapseSpaces(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    bool inSpace = false;
    for (const char character : value)
    {
        const bool isSpace = character == ' ' || character == '\t';
        if (isSpace)
        {
            inSpace = true;
            continue;
        }
        if (inSpace && !result.empty())
        {
            result += ' ';
        }
        inSpace = false;
        result += character;
    }
    return result;
}

// A space before punctuation is the visible scar of a removed tag: "That is great ."
std::string RepairPunctuationSpacing(std::string value)
{
    static const std::string punctuation = ".,!?;:";
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        if (value[index] == ' ' && index + 1 < value.size() &&
            punctuation.find(value[index + 1]) != std::string::npos)
        {
            continue;
        }
        result += value[index];
    }
    return result;
}

const std::unordered_map<std::string, VocalizationKind>& Synonyms()
{
    // Inflections included on purpose. A 8B local model asked for "[laugh]" will
    // cheerfully emit "[laughs]", "[laughing]" and "[chuckle]" across three replies,
    // and a parser that only knows one spelling makes that look like a model failure
    // when it is a parser failure.
    static const std::unordered_map<std::string, VocalizationKind> table = {
        {"laugh", VocalizationKind::Laugh},
        {"laughs", VocalizationKind::Laugh},
        {"laughing", VocalizationKind::Laugh},
        {"laughter", VocalizationKind::Laugh},
        {"chuckle", VocalizationKind::SoftLaugh},
        {"chuckles", VocalizationKind::SoftLaugh},
        {"giggle", VocalizationKind::SoftLaugh},
        {"giggles", VocalizationKind::SoftLaugh},
        {"softlaugh", VocalizationKind::SoftLaugh},
        {"soft laugh", VocalizationKind::SoftLaugh},
        {"sigh", VocalizationKind::Sigh},
        {"sighs", VocalizationKind::Sigh},
        {"sighing", VocalizationKind::Sigh},
        {"hmm", VocalizationKind::Hmm},
        {"hm", VocalizationKind::Hmm},
        {"hums", VocalizationKind::Hmm},
        {"thinking", VocalizationKind::Hmm},
        {"gasp", VocalizationKind::Gasp},
        {"gasps", VocalizationKind::Gasp},
        {"surprised", VocalizationKind::Gasp},
        {"breath", VocalizationKind::Breath},
        {"breathes", VocalizationKind::Breath},
        {"exhale", VocalizationKind::Breath},
        {"exhales", VocalizationKind::Breath}
    };
    return table;
}
}

std::string ToString(const VocalizationKind kind)
{
    switch (kind)
    {
        case VocalizationKind::Laugh: return "laugh";
        case VocalizationKind::SoftLaugh: return "soft-laugh";
        case VocalizationKind::Sigh: return "sigh";
        case VocalizationKind::Hmm: return "hmm";
        case VocalizationKind::Gasp: return "gasp";
        case VocalizationKind::Breath: return "breath";
    }
    return "laugh";
}

std::string DisplayLabel(const VocalizationKind kind)
{
    switch (kind)
    {
        case VocalizationKind::Laugh: return "laughs";
        case VocalizationKind::SoftLaugh: return "chuckles";
        case VocalizationKind::Sigh: return "sighs";
        case VocalizationKind::Hmm: return "hmm";
        case VocalizationKind::Gasp: return "gasps";
        case VocalizationKind::Breath: return "exhales";
    }
    return "laughs";
}

std::string StyleInstruction(const VocalizationKind kind)
{
    // These are the instructions handed to the VoiceDesign model when the bank is
    // rendered. They describe a SOUND, never a sentence -- ask for "a laugh" and the
    // model reads the words; ask for the sound and it produces it.
    switch (kind)
    {
        case VocalizationKind::Laugh:
            return "A short, genuine, bright laugh. Nonverbal vocal sound only, no words.";
        case VocalizationKind::SoftLaugh:
            return "A quiet amused chuckle under the breath. Nonverbal vocal sound only, no words.";
        case VocalizationKind::Sigh:
            return "A soft resigned sigh, breath released slowly. Nonverbal vocal sound only, no words.";
        case VocalizationKind::Hmm:
            return "A short thoughtful hum, considering something. Nonverbal vocal sound only, no words.";
        case VocalizationKind::Gasp:
            return "A small surprised intake of breath. Nonverbal vocal sound only, no words.";
        case VocalizationKind::Breath:
            return "A calm audible exhale. Nonverbal vocal sound only, no words.";
    }
    return "A short, genuine, bright laugh. Nonverbal vocal sound only, no words.";
}

std::vector<VocalizationKind> AllVocalizationKinds()
{
    return {
        VocalizationKind::Laugh,
        VocalizationKind::SoftLaugh,
        VocalizationKind::Sigh,
        VocalizationKind::Hmm,
        VocalizationKind::Gasp,
        VocalizationKind::Breath
    };
}

bool VocalizationFromWord(const std::string& word, VocalizationKind& outKind)
{
    const auto found = Synonyms().find(Lowered(Trimmed(word)));
    if (found == Synonyms().end())
    {
        return false;
    }
    outKind = found->second;
    return true;
}

bool SpokenScript::HasVocalization() const
{
    return std::any_of(segments.begin(), segments.end(), [](const ScriptSegment& segment)
    {
        return segment.kind == SegmentKind::Vocalization;
    });
}

SpokenScript ParseVocalizations(const std::string& reply)
{
    SpokenScript script;
    std::string pending;
    std::string display;

    const auto flushSpeech = [&]()
    {
        const std::string cleaned = Trimmed(RepairPunctuationSpacing(CollapseSpaces(pending)));
        if (!cleaned.empty() && HasSpeakableContent(cleaned))
        {
            ScriptSegment segment;
            segment.kind = SegmentKind::Speech;
            segment.text = cleaned;
            script.segments.push_back(segment);
        }
        else if (!cleaned.empty())
        {
            // Punctuation orphaned by a tag belongs to the sentence it ended, not to a
            // segment of its own: "Nice [laugh]." must not send "." to the TTS.
            //
            // Walk BACK to the last speech segment rather than checking only the
            // previous one, because the segment immediately before is the vocalization
            // that orphaned the punctuation in the first place. Checking only the back
            // dropped the full stop entirely, and a clause delivered without its final
            // punctuation loses the falling intonation that ends a sentence.
            const auto lastSpeech = std::find_if(
                script.segments.rbegin(), script.segments.rend(),
                [](const ScriptSegment& segment)
                {
                    return segment.kind == SegmentKind::Speech;
                });
            if (lastSpeech != script.segments.rend())
            {
                lastSpeech->text += cleaned;
            }
        }
        pending.clear();
    };

    std::size_t index = 0;
    while (index < reply.size())
    {
        const char character = reply[index];
        char closing = '\0';
        if (character == '[') { closing = ']'; }
        else if (character == '<') { closing = '>'; }
        else if (character == '*') { closing = '*'; }

        if (closing == '\0')
        {
            pending += character;
            display += character;
            ++index;
            continue;
        }

        const std::size_t end = reply.find(closing, index + 1);
        // An unclosed bracket is ordinary text. Consuming to end-of-string here would
        // let one stray '[' swallow an entire reply.
        if (end == std::string::npos || end == index + 1)
        {
            pending += character;
            display += character;
            ++index;
            continue;
        }

        const std::string inner = reply.substr(index + 1, end - index - 1);
        VocalizationKind kind{};
        if (!VocalizationFromWord(inner, kind))
        {
            // Not a vocalization: leave it exactly as written. Markdown emphasis, a
            // literal bracketed aside, and an HTML-looking fragment all survive.
            const std::string original = reply.substr(index, end - index + 1);
            pending += original;
            display += original;
            index = end + 1;
            continue;
        }

        flushSpeech();
        ScriptSegment segment;
        segment.kind = SegmentKind::Vocalization;
        segment.vocalization = kind;
        script.segments.push_back(segment);
        display += "*" + DisplayLabel(kind) + "*";
        index = end + 1;
    }
    flushSpeech();

    for (const ScriptSegment& segment : script.segments)
    {
        if (segment.kind != SegmentKind::Speech)
        {
            continue;
        }
        if (!script.spokenText.empty())
        {
            script.spokenText += ' ';
        }
        script.spokenText += segment.text;
    }
    script.displayText = Trimmed(RepairPunctuationSpacing(CollapseSpaces(display)));
    return script;
}

std::string ToString(const VocalizationVerdict verdict)
{
    switch (verdict)
    {
        case VocalizationVerdict::Allowed: return "Allowed";
        case VocalizationVerdict::SuppressedByAffect: return "SuppressedByAffect";
        case VocalizationVerdict::SuppressedByRate: return "SuppressedByRate";
        case VocalizationVerdict::SuppressedByMissingClip: return "SuppressedByMissingClip";
    }
    return "SuppressedByRate";
}

VocalizationPolicy::VocalizationPolicy(VocalizationLimits limits)
    : configuration(limits)
{
}

bool VocalizationPolicy::AffectPermits(
    const VocalizationKind kind,
    const revia::runtime::AffectSnapshot& affect)
{
    // Only contradictions are blocked, and only when the state is actually held rather
    // than barely registered. A companion that may laugh only while Pleased is a
    // companion that never laughs, because affect lags the joke.
    const bool stronglyHeld = affect.intensity >= 0.5F;
    switch (kind)
    {
        case VocalizationKind::Laugh:
        case VocalizationKind::SoftLaugh:
            return !(stronglyHeld &&
                (affect.state == AffectState::Concerned || affect.state == AffectState::Confused));
        case VocalizationKind::Sigh:
            return !(stronglyHeld && affect.state == AffectState::Pleased);
        case VocalizationKind::Gasp:
            return !(stronglyHeld && affect.state == AffectState::Focused);
        case VocalizationKind::Hmm:
        case VocalizationKind::Breath:
            // Neither contradicts anything; a thoughtful hum fits every state.
            return true;
    }
    return true;
}

VocalizationVerdict VocalizationPolicy::Evaluate(
    const VocalizationKind kind,
    const revia::runtime::AffectSnapshot& affect,
    const std::chrono::steady_clock::time_point now,
    const bool bClipAvailable)
{
    // Availability is checked first so a missing clip is reported as such instead of
    // being masked by a rate rule that would also have refused it.
    if (!bClipAvailable)
    {
        return VocalizationVerdict::SuppressedByMissingClip;
    }
    if (!AffectPermits(kind, affect))
    {
        return VocalizationVerdict::SuppressedByAffect;
    }
    if (spokenThisReply >= configuration.maximumPerReply)
    {
        return VocalizationVerdict::SuppressedByRate;
    }
    if (bHasSpoken)
    {
        if (configuration.bForbidImmediateRepeat && kind == lastKind)
        {
            return VocalizationVerdict::SuppressedByRate;
        }
        if (now - lastAt < configuration.minimumInterval)
        {
            return VocalizationVerdict::SuppressedByRate;
        }
    }

    // Only an allowed vocalization advances the state. A suppressed one must not push
    // the interval forward, or a rejected laugh would silence the sigh that follows it.
    spokenThisReply += 1;
    bHasSpoken = true;
    lastKind = kind;
    lastAt = now;
    return VocalizationVerdict::Allowed;
}

void VocalizationPolicy::BeginReply()
{
    spokenThisReply = 0;
}

void VocalizationPolicy::Reset()
{
    spokenThisReply = 0;
    bHasSpoken = false;
    lastAt = {};
}

int VocalizationPolicy::SpokenThisReply() const
{
    return spokenThisReply;
}

VocalizationBank::VocalizationBank(std::filesystem::path presetDirectory)
    : directory(std::move(presetDirectory))
{
    Refresh();
}

void VocalizationBank::SetPresetDirectory(std::filesystem::path presetDirectory)
{
    directory = std::move(presetDirectory);
    Refresh();
}

std::filesystem::path VocalizationBank::Directory() const
{
    return directory;
}

void VocalizationBank::Refresh()
{
    clips.clear();
    cursors.clear();
    if (directory.empty())
    {
        return;
    }
    std::error_code error;
    const std::filesystem::path root = directory / "vocalizations";
    if (!std::filesystem::is_directory(root, error))
    {
        return;
    }

    for (const VocalizationKind kind : AllVocalizationKinds())
    {
        const std::string prefix = ToString(kind) + "-";
        std::vector<std::filesystem::path> found;
        for (std::size_t index = 1; index <= maximumVariantsPerKind; ++index)
        {
            const std::filesystem::path candidate =
                root / (prefix + std::to_string(index) + ".wav");
            // Stop at the first gap rather than scanning the whole range: variants are
            // written in order, and a gap means rendering stopped there.
            if (!std::filesystem::is_regular_file(candidate, error))
            {
                break;
            }
            found.push_back(candidate);
        }
        if (!found.empty())
        {
            clips[kind] = std::move(found);
            cursors[kind] = 0;
        }
    }
}

bool VocalizationBank::Has(const VocalizationKind kind) const
{
    const auto found = clips.find(kind);
    return found != clips.end() && !found->second.empty();
}

std::size_t VocalizationBank::VariantCount(const VocalizationKind kind) const
{
    const auto found = clips.find(kind);
    return found == clips.end() ? 0 : found->second.size();
}

std::filesystem::path VocalizationBank::Next(const VocalizationKind kind)
{
    const auto found = clips.find(kind);
    if (found == clips.end() || found->second.empty())
    {
        return {};
    }
    std::size_t& cursor = cursors[kind];
    const std::filesystem::path selected = found->second[cursor % found->second.size()];
    cursor = (cursor + 1) % found->second.size();
    return selected;
}

std::vector<VocalizationKind> VocalizationBank::MissingKinds() const
{
    std::vector<VocalizationKind> missing;
    for (const VocalizationKind kind : AllVocalizationKinds())
    {
        if (!Has(kind))
        {
            missing.push_back(kind);
        }
    }
    return missing;
}

} // namespace revia::speech
