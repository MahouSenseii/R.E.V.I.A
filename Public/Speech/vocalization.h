#pragma once

#include "Runtime/affectTypes.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace revia::speech
{

// The nonverbal sounds Revia can make. Deliberately small: every kind costs a rendered
// clip per voice preset, and a companion that has six recognisable reactions reads as
// characterful, while one with thirty reads as a soundboard.
enum class VocalizationKind
{
    Laugh,
    SoftLaugh,
    Sigh,
    Hmm,
    Gasp,
    Breath
};

[[nodiscard]] std::string ToString(VocalizationKind kind);
[[nodiscard]] std::string DisplayLabel(VocalizationKind kind);
[[nodiscard]] std::string StyleInstruction(VocalizationKind kind);
[[nodiscard]] std::vector<VocalizationKind> AllVocalizationKinds();

// Checks RIFF/WAVE and a present complete data chunk, rejecting empty or truncated clips.
// Completeness check only; does not validate or decode the audio format.
[[nodiscard]] bool IsPlayableWavFile(const std::filesystem::path& path);

// Maps one written form to a kind. Accepts the synonyms a small local model actually
// emits, not only the canonical spelling.
[[nodiscard]] bool VocalizationFromWord(const std::string& word, VocalizationKind& outKind);

enum class SegmentKind
{
    Speech,
    Vocalization
};

struct ScriptSegment
{
    SegmentKind kind = SegmentKind::Speech;
    // Populated for Speech segments.
    std::string text;
    // Populated for Vocalization segments.
    VocalizationKind vocalization = VocalizationKind::Laugh;
};

// The result of reading a reply: what to say, in order, plus the two rendered forms of
// the same reply -- one for the ear and one for the eye.
struct SpokenScript
{
    std::vector<ScriptSegment> segments;
    // Tags removed, spacing repaired. This is what reaches the TTS.
    std::string spokenText;
    // Tags replaced with their stage direction, e.g. "*laughs*". This is what reaches
    // the chat bubble, where the desktop shell styles the asterisk form.
    std::string displayText;
    [[nodiscard]] bool HasVocalization() const;
};

// Recognizes known cues in bracket, angle and inflected asterisk forms.
// Unknown text remains unchanged; asterisks require known cue words so markdown survives.
[[nodiscard]] SpokenScript ParseVocalizations(const std::string& reply);

// Canonical chat spelling for cues; recognized tags are removed before ordinary TTS.
// The runtime plays pre-rendered voice-bank clips at the model-selected positions.
[[nodiscard]] std::string InlineTag(VocalizationKind kind);

struct VocalizationShaping
{
    std::string text;
    int kept = 0;
    // Recognised vocalizations removed because the reply had already used its budget.
    int droppedOverBudget = 0;
    // Prose in asterisks -- "*Softens, leaning forward slightly.*" -- removed entirely.
    int strippedStageDirections = 0;
    bool changed = false;
};

// Canonicalizes/caps six recognized sound cues, removes multiword asterisk theatre, and keeps single-word emphasis.
// Unknown bracket/angle forms remain literal text.
[[nodiscard]] VocalizationShaping ShapeVocalizations(const std::string& reply, int maximumKept);

enum class VocalizationVerdict
{
    Allowed,
    // The current affect contradicts it. Laughing while Concerned does not read as
    // playful, it reads as broken.
    SuppressedByAffect,
    // Too soon after the last one, or too many in one reply.
    SuppressedByRate,
    // Nothing rendered for this kind in the active voice's bank.
    SuppressedByMissingClip
};

[[nodiscard]] std::string ToString(VocalizationVerdict verdict);

struct VocalizationLimits
{
    // A companion that punctuates every other sentence with a laugh stops being
    // expressive and becomes a tic.
    int maximumPerReply = 2;
    std::chrono::seconds minimumInterval{8};
    // Avoid the same sound twice within one reply. The cross-reply cooldown below
    // must not ban a kind forever just because no different sound followed it.
    bool bForbidImmediateRepeat = true;
};

// Decides whether a tag the model asked for should actually be heard.
//
// The model proposes; this disposes. That split matters: the model knows what it just
// said, and nothing else in the system does, so it is the right thing to choose WHERE a
// laugh belongs. It is also the least reliable component in the process, so it is the
// wrong thing to be trusted with HOW OFTEN.
class VocalizationPolicy
{
public:
    explicit VocalizationPolicy(VocalizationLimits limits = {});

    // 'now' is passed in rather than read from the clock so the decision is a pure
    // function of its inputs and can be tested without sleeping.
    VocalizationVerdict Evaluate(VocalizationKind kind, const revia::runtime::AffectSnapshot& affect,
        std::chrono::steady_clock::time_point now, bool bClipAvailable);

    // Call between replies. Resets the per-reply count without forgetting the interval,
    // so a laugh at the end of one reply still blocks one at the start of the next.
    void BeginReply();
    void Reset();

    [[nodiscard]] int SpokenThisReply() const;
    [[nodiscard]] static bool AffectPermits(VocalizationKind kind, const revia::runtime::AffectSnapshot& affect);

private:
    VocalizationLimits configuration;
    int spokenThisReply = 0;
    bool bHasSpoken = false;
    VocalizationKind lastKind = VocalizationKind::Laugh;
    std::chrono::steady_clock::time_point lastAt{};
};

// Finds the rendered clip for a kind, rotating through the variants a voice has.
//
// Layout: <voiceRoot>/<presetId>/vocalizations/<kind>-<index>.wav, produced once when
// the preset is created. Runtime never synthesises a vocalization: playing a file that
// already exists is the only way a laugh arrives with no perceptible delay, which is
// the entire difference between it landing and it feeling like a machine catching up.
class VocalizationBank
{
public:
    VocalizationBank() = default;
    explicit VocalizationBank(std::filesystem::path presetDirectory);

    void SetPresetDirectory(std::filesystem::path presetDirectory);
    // Rescans the directory. Cheap, and called when a preset is assigned.
    void Refresh();

    [[nodiscard]] bool Has(VocalizationKind kind) const;
    [[nodiscard]] std::size_t VariantCount(VocalizationKind kind) const;
    // Returns the next variant path, or an empty path when the kind has none. Rotation
    // is round-robin rather than random: reproducible in tests, and it guarantees every
    // variant is used before any repeats, which random sampling does not.
    [[nodiscard]] std::filesystem::path Next(VocalizationKind kind);
    [[nodiscard]] std::vector<VocalizationKind> MissingKinds() const;
    [[nodiscard]] std::filesystem::path Directory() const;

    static constexpr std::size_t maximumVariantsPerKind = 8;

private:
    std::filesystem::path directory;
    std::map<VocalizationKind, std::vector<std::filesystem::path>> clips;
    std::map<VocalizationKind, std::size_t> cursors;
};

} // namespace revia::speech
