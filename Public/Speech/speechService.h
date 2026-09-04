#pragma once

#include "Library/structLibrary.h"
#include "Runtime/affectTypes.h"
#include "Speech/qwenTtsPool.h"
#include "Speech/orderedSpeechQueue.h"
#include "Speech/voiceActivityMonitor.h"
#include "Speech/vocalization.h"
#include "Speech/voicePresetStore.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace revia::speech
{

struct SpeechEvent
{
    SpeechEvent() = default;
    SpeechEvent(
        std::string inputPhase,
        std::string inputDetail,
        const double inputElapsedMilliseconds = -1.0,
        const int inputQueueDepth = 0,
        const std::uint64_t inputUtteranceId = 0,
        std::string inputDevice = {})
        : phase(std::move(inputPhase)),
          detail(std::move(inputDetail)),
          elapsedMilliseconds(inputElapsedMilliseconds),
          queueDepth(inputQueueDepth),
          utteranceId(inputUtteranceId),
          device(std::move(inputDevice))
    {
    }

    std::string phase;
    std::string detail;
    double elapsedMilliseconds = -1.0;
    int queueDepth = 0;
    // Which reply this event belongs to, so the shell can hold that reply's text until
    // the audio for it actually starts. Zero when the event is not about an utterance.
    std::uint64_t utteranceId = 0;
    std::string device;
    std::vector<latencySample> timings;
};

class SpeechService
{
public:
    using EventHandler = std::function<void(const SpeechEvent&)>;

    SpeechService() = default;
    ~SpeechService();

    SpeechService(const SpeechService&) = delete;
    SpeechService& operator=(const SpeechService&) = delete;

    bool Start(const speechSettings& settings, EventHandler handler);
    void SetActiveProfile(std::string profileId);
    VoiceStudioSnapshot VoiceStudio() const;
    bool HasActiveQwenVoice() const;
    VoiceOperationResult PrepareActiveVoice();
    VoiceOperationResult CreateVoicePreset(
        const std::string& name,
        const std::string& description,
        const std::string& referenceText,
        const std::string& language);
    // How a preset's clip bank gets rendered. A parameter rather than a direct pool
    // call so the step below can be driven by a test without a GPU worker.
    using VocalizationRenderer = std::function<VoiceOperationResult(
        const std::filesystem::path& presetDirectory,
        const std::vector<QwenTtsClient::VocalizationRequest>& requests,
        const std::string& language)>;

    // Saves a finished preset and renders its nonverbal clip bank, as one step.
    //
    // The two belong together. The first version of this unit added the renderer,
    // tested it thoroughly, and never called it from anywhere -- and every test still
    // passed, because the tests all called the renderer directly. Binding the render
    // to the save means a preset that exists without a bank cannot be produced by
    // forgetting a line: dropping the call drops the preset too, which fails loudly.
    //
    // Static, taking its collaborators explicitly, so a test drives the exact sequence
    // preset creation runs rather than a restatement of it.
    static VoiceOperationResult FinishVoicePreset(
        VoicePresetStore& store,
        const VoicePreset& preset,
        const VocalizationRenderer& render);

    VoiceOperationResult PreviewVoice(const std::string& presetId, const std::string& text);
    // Renders the missing nonverbal clips for a voice that already exists.
    //
    // Needed because the bank arrived after the voices did. Every preset created
    // before this feature has no clips and no way to get any, and re-creating a voice
    // to obtain them would replace the reference audio the user already approved.
    // Rendering only what is missing makes this safe to press twice.
    VoiceOperationResult RenderVoiceBank(const std::string& presetId);

    // Points the service at a voice now, outside profile assignment.
    //
    // Public because the voice studio needs it after rendering a bank -- new clips
    // should be usable without a restart -- and because it is the operation a test has
    // to be able to perform to prove the bank follows the voice.
    void UseVoice(std::optional<VoicePreset> preset);

    // Whose sounds are currently loaded. The observable half of the rule above: a bank
    // left on the previous voice is silent about it until you hear the wrong laugh.
    [[nodiscard]] std::filesystem::path ActiveVocalizationDirectory() const;
    VoiceOperationResult AssignVoice(const std::string& profileId, const std::string& presetId);
    void SetEnabled(bool enabled);
    bool IsEnabled() const;
    [[nodiscard]] bool HasPendingSpeech() const;
    [[nodiscard]] std::size_t FirstFragmentCharacters() const;
    [[nodiscard]] std::size_t PreferredFragmentCharacters() const;
    // utteranceId correlates the resulting Speaking event back to the reply, so the shell
    // can reveal text in step with the audio instead of well before it.
    void Speak(
        std::string text,
        runtime::AffectSnapshot affect,
        std::uint64_t utteranceId = 0,
        bool latencyCritical = true);
    void StopSpeaking();

    // Barge-in. Arms a microphone energy monitor only for the duration of each utterance,
    // and yields the floor when the user starts talking over Revia.
    void ConfigureBargeIn(const bargeInSettings& settings, int sampleRate);
    void SetBargeInHandler(std::function<void()> handler);
    void SetBargeInEnabled(bool enabled);
    [[nodiscard]] bool IsBargeInEnabled() const;
    // Stops talking without cancelling the Qwen request that produced the audio. Killing
    // that worker would make the next reply pay a full model reload, which is far too
    // expensive a price for the user having spoken.
    void YieldToUser();

    // Shutdown-only escape hatch for a model load or synthesis blocked inside the
    // Python worker. Normal Stop/Yield deliberately keep the warm workers alive.
    void CancelVoiceOperationsForShutdown();

    void Shutdown();

    // keepVocalizations is true only for a backend that performs an inline nonverbal
    // cue itself. Windows SAPI cannot, and for it the tag is dropped rather than
    // flattened, because reading the word "laughs" aloud is worse than silence.
    static std::string NormalizeForSpeech(
        const std::string& text,
        std::size_t maxCharacters,
        bool keepVocalizations = false);

    // Whether the pipeline owns an audio file or is only borrowing it.
    enum class AudioLifetime
    {
        // A scratch file synthesised for one playback. Deleted when done with.
        Temporary,
        // A clip from the voice's bank. Every later reply reuses it, so nothing in the
        // speech pipeline may delete it.
        Persistent
    };

    // Which one a planned item is. The single place the question is answered: three
    // separate cleanup sites ask it, and a bank clip only needs one of them to answer
    // wrongly to be gone for good.
    [[nodiscard]] static AudioLifetime LifetimeOf(SegmentKind kind);

    // Deletes an item's audio only when it was ours to delete. A no-op for a bank clip
    // and for an empty path.
    static void ReleaseAudio(
        const std::filesystem::path& path, AudioLifetime lifetime);

    // Whether a queued item still belongs to the reply currently being spoken.
    //
    // Asked identically of a phrase and of a cue, because a cue that skipped it is the
    // late chuckle: the user interrupted, the sentence stopped, and a laugh arrived
    // afterwards attached to nothing.
    [[nodiscard]] static bool StillCurrent(
        std::uint64_t itemGeneration, std::uint64_t currentGeneration, bool voiceEnabled);

    // One item of the ordered plan a reply becomes.
    struct PlannedSegment
    {
        SegmentKind kind = SegmentKind::Speech;
        // Speech only, already normalised for synthesis.
        std::string text;
        // Vocalization only.
        VocalizationKind vocalization = VocalizationKind::Laugh;
    };

    // Turns a reply into the ordered plan the speech queue receives.
    //
    // "Yeah. *chuckles* Nice try." becomes speech, sound, speech, in that order, so the
    // laugh lands where the model put it rather than after the sentence has finished.
    //
    // Pure and static so a test drives the decision the live path makes instead of
    // restating it. `decide` answers whether a cue is actually heard -- it is where the
    // clip bank and the rate/affect policy are consulted -- and a suppressed cue leaves
    // the speech on either side of it completely untouched.
    [[nodiscard]] static std::vector<PlannedSegment> PlanSpeech(
        const std::string& reply,
        const speechSettings& settings,
        const std::function<VocalizationVerdict(VocalizationKind)>& decide);

    // The exact preparation the live speech path applies before an utterance is
    // queued, in one pure function.
    //
    // Pure and static on purpose. The vocalization decision used to be a literal at
    // the call site with a test that restated the same literal, which meant the test
    // passed whatever the live path actually did. Mutating the call site proved it:
    // the tag was allowed through to the synthesiser and every test still passed.
    // The decision now lives where a test can hold it.
    [[nodiscard]] static std::string PrepareForSynthesis(
        const std::string& text,
        const speechSettings& settings);

private:
    struct Utterance
    {
        std::string text;
        runtime::AffectSnapshot affect;
        std::uint64_t generation = 0;
        std::uint64_t utteranceId = 0;
        std::uint64_t sequence = 0;
        bool latencyCritical = true;
        std::chrono::steady_clock::time_point queuedAt =
            std::chrono::steady_clock::now();
        std::optional<VoicePreset> preset;
        // Set when this item is a nonverbal cue rather than a phrase. It occupies a
        // sequence slot like any other item, which is what keeps it between the two
        // phrases it was written between.
        std::optional<VocalizationKind> vocalization;
        // The bank clip chosen for that cue. Resolved when the reply is planned, so a
        // reply uses its variants in the order it was written.
        std::filesystem::path clipPath;
    };

    struct PreparedUtterance
    {
        Utterance utterance;
        std::filesystem::path audioPath;
        VoiceOperationResult result;
        bool qwenAttempted = false;
        std::uintmax_t bufferedBytes = 0;
        // Deleting a bank clip would empty the voice a sound at a time, and it would
        // go quiet again with nothing to show why.
        AudioLifetime lifetime = AudioLifetime::Temporary;
    };

    void Run(std::stop_token stopToken);
    void Generate(std::stop_token stopToken);
    // Synthesizes one phrase on its own and publishes it. Also the fallback body for a
    // batch that could not run, so the two paths cannot drift apart in what they
    // publish or in how they account for a phrase still being generated.
    void SynthesizeOne(Utterance utterance, int depth);
    // Attempts one generation call covering the whole group, in order.
    //
    // False means nothing was published and the caller must fall back to synthesizing
    // the group one phrase at a time. Every refusal is reported that way rather than by
    // throwing: a batch that will not fit is an ordinary condition on a shared card,
    // not an error, and the per-phrase path is always available.
    bool SynthesizeBatch(std::vector<Utterance>& group, int depth);
    // Takes the phrases that may ride along with `leader` off the queue.
    //
    // Caller holds the mutex. Only complete phrases already waiting are taken: nothing
    // is held back to build a bigger batch, because the phrases are still arriving from
    // the model and waiting for more would delay the ones already here.
    std::vector<Utterance> CollectBatchCompanions(const Utterance& leader);
    void PublishGenerated(const Utterance& utterance, PreparedUtterance item, int depth);
    // Compares the path the first real phrase ran on against the configured one, once
    // per session. Graph capture is deferred to that phrase, so this is the earliest
    // point at which the answer is an observation rather than a setting.
    void VerifyInferenceBackend(
        const VoiceOperationResult& result, std::uint64_t utteranceId);
    bool PlayPreparedQwen(const PreparedUtterance& prepared);
    // Plays one bank clip in its place in the reply, and never deletes it.
    void PlayVocalizationClip(const PreparedUtterance& prepared);
    // The body of UseVoice, for callers already holding the mutex.
    //
    // Sets the active voice and repoints the clip bank with it, together.
    //
    // One function so the two cannot drift apart. A bank still pointing at the previous
    // voice plays the wrong laugh, and nothing about that looks like a bug until you
    // hear it -- so changing the voice without moving the bank is made impossible
    // rather than merely discouraged. Caller holds the mutex.
    void SetActiveVoiceLocked(std::optional<VoicePreset> preset);
    void Notify(SpeechEvent event) const;
    void ArmBargeIn();
    void DisarmBargeIn();

    mutable std::mutex mutex;
    std::condition_variable_any condition;
    std::deque<Utterance> queue;
    OrderedSpeechQueue playbackOrder;
    std::map<std::uint64_t, PreparedUtterance> prepared;
    std::size_t generatingCount = 0;
    std::uintmax_t bufferedAudioBytes = 0;
    speechSettings configuration;
    std::string activeProfile;
    std::optional<VoicePreset> activePreset;
    EventHandler eventHandler;
    VoicePresetStore presetStore;
    // Whose laugh it is. Follows the active preset, because a voice playing another
    // voice's clips is worse than a voice with none.
    VocalizationBank vocalizationBank;
    // How often she is allowed to. The model chooses where a cue belongs; this decides
    // whether it is heard, and it is deliberately not the model's to overrule.
    VocalizationPolicy vocalizationPolicy;
    QwenTtsPool qwenPool;
    VoiceActivityMonitor bargeInMonitor;
    std::function<void()> bargeInHandler;
    std::jthread worker;
    std::vector<std::jthread> generationWorkers;
    std::atomic<bool> enabled = false;
    std::atomic<bool> ready = false;
    std::atomic<std::uint64_t> generation = 1;
    std::atomic<std::uint64_t> nextSequence = 1;
    // Stamped onto every event while an utterance is in flight, so callers do not have to
    // thread the id through each Notify call site.
    std::atomic<std::uint64_t> activeUtteranceId = 0;
    // Latched by the first synthesised phrase, so the backend check is reported once
    // rather than on every request.
    std::atomic<bool> backendVerified = false;
};

} // namespace revia::speech
