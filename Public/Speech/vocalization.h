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

// Reads inline tags out of a reply.
//
// Liberal in what it accepts, because the thing writing these tags is a small local
// model that will not be consistent: [laugh], <laugh>, and *laughs* all parse, as do
// the obvious inflections. Strict in one place -- the asterisk form is accepted ONLY
// for known vocalization words, so ordinary markdown emphasis survives untouched.
//
// Anything unrecognised is left exactly where it was found. A bracketed word Revia
// meant literally must not silently disappear from her own sentence.
[[nodiscard]] SpokenScript ParseVocalizations(const std::string& reply);

// The single spelling handed to the TTS and shown in chat. Qwen3-TTS renders a
// nonverbal cue written inline in the text it is given, so a vocalization needs no
// pre-rendered clip -- it only needs to survive in one spelling the model recognises.
// Every accepted synonym is canonicalised to this form. If the model turns out to want
// a different delimiter, this is the one place that has to change.
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

// Keeps the sound effects and removes the theatre.
//
// A vocalization is a SOUND the voice can actually make, and the six kinds above are
// the whole list. Anything else an asterisk pair contains is prose the model wrote
// about itself, which the TTS would read aloud word by word and which nobody asked
// for. So: recognised cues are canonicalised and capped, multi-word asterisk spans are
// deleted, and single-word emphasis such as *really* is left alone because that is
// ordinary markdown and not a stage direction.
//
// Bracket and angle forms are deliberately NOT stripped when unrecognised: "[section 4]"
// is something Revia meant literally, and deleting it would edit her own sentence.
[[nodiscard]] VocalizationShaping ShapeVocalizations(
    const std::string& reply,
    int maximumKept);

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
    // The same sound twice in a row is the most obvious tell that it is a recording.
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
    VocalizationVerdict Evaluate(
        VocalizationKind kind,
        const revia::runtime::AffectSnapshot& affect,
        std::chrono::steady_clock::time_point now,
        bool bClipAvailable);

    // Call between replies. Resets the per-reply count without forgetting the interval,
    // so a laugh at the end of one reply still blocks one at the start of the next.
    void BeginReply();
    void Reset();

    [[nodiscard]] int SpokenThisReply() const;
    [[nodiscard]] static bool AffectPermits(
        VocalizationKind kind, const revia::runtime::AffectSnapshot& affect);

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
