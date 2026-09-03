#include "Presence/adapterArchivePolicy.h"
#include "Presence/presenceRuntime.h"
#include "Core/runtimePath.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>

namespace revia::presence
{

namespace
{
    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

    std::string SafeToken(std::string value)
    {
        value.erase(std::remove_if(value.begin(), value.end(),
            [](const unsigned char character)
            {
                return !(std::isalnum(character) || character == '-' || character == '_');
            }), value.end());
        return value.substr(0, 80);
    }

    std::string IsoTimestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t value = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &value);
#else
        gmtime_r(&value, &utc);
#endif
        std::ostringstream stream;
        stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        return stream.str();
    }

    std::string Expression(const runtime::AffectState state)
    {
        switch (state)
        {
            case runtime::AffectState::Curious: return "curious";
            case runtime::AffectState::Pleased: return "happy";
            case runtime::AffectState::Excited: return "excited";
            case runtime::AffectState::Playful: return "playful";
            case runtime::AffectState::Bored: return "bored";
            case runtime::AffectState::Sulky: return "sulky";
            case runtime::AffectState::Sad: return "sad";
            case runtime::AffectState::Melancholy: return "melancholy";
            case runtime::AffectState::Angry: return "angry";
            case runtime::AffectState::Lonely: return "lonely";
            case runtime::AffectState::Frustrated: return "frustrated";
            case runtime::AffectState::Concerned: return "concerned";
            case runtime::AffectState::Confused: return "confused";
            case runtime::AffectState::Focused: return "focused";
            default: return "neutral";
        }
    }

    std::uint64_t ExistingSequence(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        if (!file.is_open()) return 0;
        try
        {
            nlohmann::json document;
            file >> document;
            return document.value("sequence", std::uint64_t{0});
        }
        catch (...)
        {
            return 0;
        }
    }

    bool ContainsUnsafeControl(const std::string& text)
    {
        return std::any_of(text.begin(), text.end(), [](const unsigned char character)
        {
            return character < 0x20 && character != '\n' && character != '\r' &&
                character != '\t';
        });
    }

    bool ContainsSpamRun(const std::string& text)
    {
        std::size_t run = 1;
        for (std::size_t index = 1; index < text.size(); ++index)
        {
            run = text[index] == text[index - 1] ? run + 1 : 1;
            if (run > 24) return true;
        }
        return false;
    }

    bool AtomicJsonWrite(const std::filesystem::path& path, const nlohmann::json& document)
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            return false;
        }
        // Unique per write, not a shared "<path>.tmp".
        //
        // Avatar state is written from several threads -- speech phase changes, presence
        // observation, affect updates -- and they all used the same temporary name. Two
        // overlapping writes would truncate each other's file, the first rename would
        // move it away, and the second would fail because its temporary no longer
        // existed. That produced a steady stream of "the avatar state file could not be
        // updated" while nothing was actually wrong with the destination.
        static std::atomic<std::uint64_t> writeCounter{0};
        const std::filesystem::path temporary = path.string() + "." +
            std::to_string(writeCounter.fetch_add(1)) + ".tmp";
        {
            std::ofstream file(temporary, std::ios::trunc);
            if (!file.is_open())
            {
                return false;
            }
            file << document.dump(2) << '\n';
            if (!file.good())
            {
                return false;
            }
        }
        // No pre-remove. Deleting the destination first opens a window where a reader
        // sees no file at all, and std::filesystem::rename already replaces an existing
        // regular file on both platforms this runs on.
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            // A stale temporary would otherwise accumulate one file per failed write.
            std::error_code cleanup;
            std::filesystem::remove(temporary, cleanup);
        }
        if (error)
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }
}

PresenceRuntime::~PresenceRuntime()
{
    Shutdown();
}

bool PresenceRuntime::Start(
    const presenceSettings& settings,
    NoticeHandler inputNoticeHandler,
    AdapterHandler inputAdapterHandler)
{
    Shutdown();
    const std::uint64_t previousSequence =
        ExistingSequence(core::ResolveRuntimeWritePath(settings.statePath));
    {
        std::lock_guard lock(mutex);
        configuration = settings;
        noticeHandler = std::move(inputNoticeHandler);
        adapterHandler = std::move(inputAdapterHandler);
        snapshot = {};
        snapshot.enabled = settings.bEnabled;
        snapshot.avatarBridgeEnabled = settings.bAvatarBridgeEnabled;
        snapshot.adaptersEnabled = settings.bExternalAdaptersEnabled;
        snapshot.phase = settings.bEnabled ? "idle" : "disabled";
        snapshot.sequence = previousSequence + 1;
        adapterAdmissions.clear();
        recentAdapterIdOrder.clear();
        recentAdapterIds.clear();
        lastStreamReply = {};
        // Anchored to the canonical runtime root, not to the process working
        // directory. These files do not exist on a first run, so the read-side
        // resolver would have fallen through to a CWD-relative absolute path and a
        // shortcut, a startup entry, or any launcher started elsewhere would have
        // created a second RuntimeData tree there. Absolute configured paths are
        // returned unchanged.
        inboxRoot = core::ResolveRuntimeWritePath(settings.inboxPath);
        outboxRoot = core::ResolveRuntimeWritePath(settings.outboxPath);
        stateFile = core::ResolveRuntimeWritePath(settings.statePath);
        eventFile = core::ResolveRuntimeWritePath(settings.eventPath);
    }

    // The runtime folders are part of the install contract even when their optional
    // consumers are disabled. This makes adapter and renderer setup discoverable.
    std::error_code error;
    std::filesystem::create_directories(inboxRoot, error);
    if (!error) std::filesystem::create_directories(outboxRoot, error);
    if (!error) std::filesystem::create_directories(inboxRoot / "Processed", error);
    if (!error) std::filesystem::create_directories(inboxRoot / "Rejected", error);
    if (error)
    {
        Notify({"Presence", "Error", "Presence runtime folders could not be created: " +
            error.message()});
        return false;
    }

    WriteAvatarState(true);
    Notify({"Presence", settings.bEnabled ? "Ready" : "Disabled",
        settings.bEnabled
            ? "Presence coordination is ready."
            : "Presence coordination is disabled."});
    Notify({"Avatar", settings.bAvatarBridgeEnabled ? "Ready" : "Disabled",
        settings.bAvatarBridgeEnabled
            ? "Avatar state is available at " + stateFile.string() + "."
            : "The avatar bridge is disabled."});
    Notify({"Adapters", settings.bExternalAdaptersEnabled ? "Watching" : "Disabled",
        settings.bExternalAdaptersEnabled
            ? "Watching the bounded local adapter inbox."
            : "Discord, stream, and game adapters are disabled."});

    if (settings.bEnabled && settings.bExternalAdaptersEnabled)
    {
        adapterWorker = std::jthread(
            [this](const std::stop_token stopToken) { RunAdapterInbox(stopToken); });
    }
    return true;
}

void PresenceRuntime::Observe(const runtime::RuntimeEvent& event)
{
    PresenceSnapshot before;
    PresenceSnapshot after;
    {
        std::lock_guard lock(mutex);
        if (!snapshot.enabled || event.component == "Presence" ||
            event.component == "Avatar" || event.component == "Adapters")
        {
            return;
        }
        before = snapshot;
        if (event.kind == runtime::RuntimeEventKind::AffectChanged)
        {
            snapshot.affect = event.affect;
            snapshot.affectIntensity = std::clamp(event.affectIntensity, 0.0F, 1.0F);
        }
        if (event.kind == runtime::RuntimeEventKind::StateChanged)
        {
            switch (event.state)
            {
                case runtime::RuntimeState::Thinking:
                case runtime::RuntimeState::Remembering: snapshot.phase = "thinking"; break;
                case runtime::RuntimeState::Acting: snapshot.phase = "acting"; break;
                case runtime::RuntimeState::WaitingForConfirmation: snapshot.phase = "waiting"; break;
                case runtime::RuntimeState::Blocked: snapshot.phase = "blocked"; break;
                case runtime::RuntimeState::Error: snapshot.phase = "error"; break;
                case runtime::RuntimeState::Stopping:
                case runtime::RuntimeState::Offline: snapshot.phase = "offline"; break;
                case runtime::RuntimeState::Starting: snapshot.phase = "starting"; break;
                case runtime::RuntimeState::Responding: snapshot.phase = "responding"; break;
                case runtime::RuntimeState::Idle: snapshot.phase = "idle"; break;
            }
        }
        if (event.kind == runtime::RuntimeEventKind::ComponentStatus && event.component == "Voice")
        {
            if (event.phase == "Speaking") snapshot.phase = "speaking";
            else if (event.phase == "Generating" || event.phase == "Queued") snapshot.phase = "responding";
            else if (event.phase == "Error") snapshot.phase = "error";
            else if (event.phase == "Ready" || event.phase == "Stopped" ||
                event.phase == "Interrupted") snapshot.phase = "idle";
        }
        if (event.kind == runtime::RuntimeEventKind::ComponentStatus &&
            event.component == "Microphone")
        {
            if (event.phase == "Recording" || event.phase == "SpeechDetected")
                snapshot.phase = "listening";
            else if (event.phase == "Transcribing") snapshot.phase = "thinking";
        }
        if (event.kind == runtime::RuntimeEventKind::ComponentStatus &&
            event.component == "Perception" && !event.message.empty())
        {
            snapshot.attention = event.message.substr(0, 160);
        }
        if (event.kind == runtime::RuntimeEventKind::AssistantMessage ||
            event.kind == runtime::RuntimeEventKind::ReplyFragment)
        {
            lastConversationActivity = std::chrono::steady_clock::now();
            snapshot.conversationMomentum = std::min(1.0F,
                snapshot.conversationMomentum + (event.kind == runtime::RuntimeEventKind::ReplyFragment
                    ? 0.08F : 0.16F));
        }
        if (lastConversationActivity.time_since_epoch().count() != 0)
        {
            const double idleSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - lastConversationActivity).count();
            snapshot.conversationMomentum = std::max(0.0F,
                snapshot.conversationMomentum - static_cast<float>(idleSeconds / 1800.0));
            lastConversationActivity = std::chrono::steady_clock::now();
        }
        if (snapshot.phase != before.phase || snapshot.affect != before.affect ||
            snapshot.affectIntensity != before.affectIntensity ||
            snapshot.attention != before.attention ||
            snapshot.conversationMomentum != before.conversationMomentum)
        {
            ++snapshot.sequence;
        }
        after = snapshot;
    }
    if (after.sequence != before.sequence)
    {
        WriteAvatarState(true);
    }
}

void PresenceRuntime::RecordUserInput(const std::string& sourceLabel)
{
    {
        std::lock_guard lock(mutex);
        if (!snapshot.enabled) return;
        snapshot.attention = sourceLabel.empty() ? "local user" : sourceLabel;
        snapshot.conversationMomentum = std::min(1.0F, snapshot.conversationMomentum + 0.22F);
        snapshot.phase = "listening";
        lastConversationActivity = std::chrono::steady_clock::now();
        ++snapshot.sequence;
    }
    WriteAvatarState(true);
}

void PresenceRuntime::PublishAdapterReply(
    const ExternalAdapterEvent& request,
    const std::string& text,
    const bool succeeded,
    const std::string& reason)
{
    presenceSettings settings;
    {
        std::lock_guard lock(mutex);
        settings = configuration;
    }
    if (!settings.bExternalAdaptersEnabled)
    {
        return;
    }
    const std::string safeId = SafeToken(request.id).empty()
        ? "missing-id"
        : SafeToken(request.id);
    const nlohmann::json response = {
        {"version", 1}, {"id", safeId}, {"source", request.source},
        {"channel", request.channel}, {"author_id", request.authorId},
        {"succeeded", succeeded},
        {"text", text}, {"reason", reason}, {"timestamp", IsoTimestamp()}
    };
    const std::filesystem::path target = outboxRoot /
        (request.source + "-reply-" + safeId + ".json");
    if (!AtomicJsonWrite(target, response))
    {
        Notify({"Adapters", "Error", "An adapter reply could not be written."});
        return;
    }
    Notify({"Adapters", succeeded ? "Replied" : "Error",
        succeeded ? "Wrote a conversation reply for " + request.source + "."
                  : "The " + request.source + " conversation turn failed."});
}

PresenceSnapshot PresenceRuntime::Snapshot() const
{
    std::lock_guard lock(mutex);
    return snapshot;
}

void PresenceRuntime::Shutdown()
{
    if (adapterWorker.joinable())
    {
        adapterWorker.request_stop();
        adapterWorker.join();
    }
    bool write = false;
    {
        std::lock_guard lock(mutex);
        write = snapshot.enabled && snapshot.avatarBridgeEnabled &&
            snapshot.phase != "offline";
        if (write)
        {
            snapshot.phase = "offline";
            ++snapshot.sequence;
        }
    }
    if (write)
    {
        WriteAvatarState(true);
    }
}

void PresenceRuntime::RunAdapterInbox(const std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        ScanAdapterInbox();
        const int interval = [&]
        {
            std::lock_guard lock(mutex);
            return configuration.adapterPollMs;
        }();
        const int slices = std::max(1, interval / 25);
        for (int slice = 0; slice < slices && !stopToken.stop_requested(); ++slice)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
}

void PresenceRuntime::PruneAdapterArchive(const std::filesystem::path& directory)
{
    int maximumFiles = 0;
    int maximumAgeDays = 0;
    {
        std::lock_guard lock(mutex);
        maximumFiles = configuration.adapterArchiveMaximumFiles;
        maximumAgeDays = configuration.adapterArchiveMaximumAgeDays;
    }
    if (maximumFiles <= 0 && maximumAgeDays <= 0) return;

    std::error_code error;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> kept;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (!entry.is_regular_file(error) || error) { error.clear(); continue; }
        const auto written = entry.last_write_time(error);
        if (error) { error.clear(); continue; }
        kept.emplace_back(written, entry.path());
    }
    if (error) return;

    // Policy lives in SelectExpiredArchiveFiles so it can be tested without a
    // filesystem; this function only walks the directory and deletes what it names.
    for (const std::filesystem::path& stale : SelectExpiredArchiveFiles(
            std::move(kept), maximumFiles, maximumAgeDays,
            std::filesystem::file_time_type::clock::now()))
    {
        std::error_code removeError;
        std::filesystem::remove(stale, removeError);
    }
}

void PresenceRuntime::ScanAdapterInbox()
{
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(inboxRoot, error))
    {
        if (entry.is_regular_file() && Lower(entry.path().extension().string()) == ".json")
        {
            files.push_back(entry.path());
        }
    }
    if (error) return;
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& path : files)
    {
        ExternalAdapterEvent event;
        std::string parseError;
        const bool parsed = ParseAdapterFile(path, event, parseError);
        const std::filesystem::path destinationRoot = inboxRoot /
            (parsed ? "Processed" : "Rejected");
        const std::filesystem::path destination = destinationRoot / path.filename();
        std::filesystem::rename(path, destination, error);
        if (error)
        {
            error.clear();
            std::filesystem::remove(path, error);
        }
        // Pruned after the move, and only the archive the file just landed in. The
        // inbox itself is never touched here: a file still arriving must not be
        // considered for retention.
        PruneAdapterArchive(destinationRoot);
        if (!parsed)
        {
            Notify({"Adapters", "Rejected", parseError});
            continue;
        }
        if (!RememberAdapterEvent(event))
        {
            Notify({"Adapters", "Ignored", "A replayed adapter event was ignored."});
            continue;
        }
        std::string policyReason;
        const auto now = std::chrono::steady_clock::now();
        if (!StreamPolicyAllows(event, now, policyReason))
        {
            Notify({"Adapters", "Ignored", policyReason});
            continue;
        }
        if (!RateLimitAllows(now))
        {
            Notify({"Adapters", "RateLimited", "The adapter event limit was reached."});
            continue;
        }
        AdapterHandler handler;
        {
            std::lock_guard lock(mutex);
            snapshot.attention = event.source + ":" + event.channel;
            snapshot.phase = "listening";
            snapshot.conversationMomentum = std::min(1.0F,
                snapshot.conversationMomentum + 0.2F);
            ++snapshot.sequence;
            handler = adapterHandler;
        }
        WriteAvatarState(true);
        Notify({"Adapters", "Received",
            "Accepted a bounded " + event.source + " conversation event."});
        if (handler) handler(event);
    }
}

bool PresenceRuntime::ParseAdapterFile(
    const std::filesystem::path& path,
    ExternalAdapterEvent& outEvent,
    std::string& outError) const
{
    presenceSettings settings;
    {
        std::lock_guard lock(mutex);
        settings = configuration;
    }
    std::ifstream file(path);
    nlohmann::json document;
    try
    {
        file >> document;
    }
    catch (...)
    {
        outError = "Adapter input was not valid JSON.";
        return false;
    }
    if (!document.is_object())
    {
        outError = "Adapter input must be one JSON object.";
        return false;
    }
    try
    {
        outEvent.version = document.value("version", 1);
        outEvent.id = SafeToken(document.value("id", path.stem().string()));
        outEvent.source = Lower(SafeToken(document.value("source", "")));
        outEvent.channel = SafeToken(document.value("channel", "default"));
        outEvent.author = document.value("author", "user");
        outEvent.authorId = SafeToken(document.value("author_id", ""));
        outEvent.role = Lower(SafeToken(document.value("role", "viewer")));
        outEvent.text = document.value("text", "");
        outEvent.addressedToRevia = document.value("addressed_to_revia", false);
    }
    catch (...)
    {
        outError = "Adapter input fields had invalid types.";
        return false;
    }
    if (outEvent.version != 1)
    {
        outError = "Adapter input used an unsupported contract version.";
        return false;
    }
    const bool allowed = std::any_of(
        settings.allowedAdapters.begin(), settings.allowedAdapters.end(),
        [&outEvent](const std::string& value) { return Lower(value) == outEvent.source; });
    if (!allowed)
    {
        outError = "Adapter source is not allowlisted.";
        return false;
    }
    if (outEvent.id.empty() || outEvent.text.empty() ||
        outEvent.text.size() > static_cast<std::size_t>(settings.maxAdapterTextCharacters))
    {
        outError = "Adapter id or text was empty, unsafe, or over the configured limit.";
        return false;
    }
    if (outEvent.source == "stream" && outEvent.authorId.empty())
    {
        outError = "Stream input requires a stable author_id separate from display name.";
        return false;
    }
    if (outEvent.authorId.empty()) outEvent.authorId = SafeToken(outEvent.author);
    if (outEvent.authorId.empty()) outEvent.authorId = "anonymous";
    if (outEvent.role != "viewer" && outEvent.role != "moderator" &&
        outEvent.role != "broadcaster")
    {
        outError = "Adapter role was not viewer, moderator, or broadcaster.";
        return false;
    }
    if (ContainsUnsafeControl(outEvent.author) || ContainsUnsafeControl(outEvent.text) ||
        ContainsSpamRun(outEvent.text))
    {
        outError = "Adapter text contained unsafe control data or repeated-character spam.";
        return false;
    }
    if (!outEvent.text.empty() && outEvent.text.front() == '/')
    {
        outError = "External adapters cannot invoke Revia slash commands.";
        return false;
    }
    outEvent.author = outEvent.author.substr(0, 120);
    return true;
}

bool PresenceRuntime::RateLimitAllows(const std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock(mutex);
    while (!adapterAdmissions.empty() &&
        now - adapterAdmissions.front() > std::chrono::minutes(1))
    {
        adapterAdmissions.pop_front();
    }
    if (adapterAdmissions.size() >=
        static_cast<std::size_t>(configuration.maxAdapterEventsPerMinute))
    {
        return false;
    }
    adapterAdmissions.push_back(now);
    return true;
}

bool PresenceRuntime::RememberAdapterEvent(const ExternalAdapterEvent& event)
{
    std::lock_guard lock(mutex);
    const std::string key = event.source + ":" + event.id;
    if (recentAdapterIds.contains(key)) return false;
    recentAdapterIds.insert(key);
    recentAdapterIdOrder.push_back(key);
    while (recentAdapterIdOrder.size() >
        static_cast<std::size_t>(configuration.rememberedAdapterIds))
    {
        recentAdapterIds.erase(recentAdapterIdOrder.front());
        recentAdapterIdOrder.pop_front();
    }
    return true;
}

bool PresenceRuntime::StreamPolicyAllows(
    const ExternalAdapterEvent& event,
    const std::chrono::steady_clock::time_point now,
    std::string& outReason)
{
    if (event.source != "stream") return true;
    std::lock_guard lock(mutex);
    if (configuration.bRequireAddressedStreamMessages && event.role == "viewer" &&
        !event.addressedToRevia)
    {
        outReason = "The stream message was not addressed to Revia.";
        return false;
    }
    const auto cooldown = std::chrono::seconds(configuration.streamReplyCooldownSeconds);
    if (event.role == "viewer" && lastStreamReply != std::chrono::steady_clock::time_point{} &&
        now - lastStreamReply < cooldown)
    {
        outReason = "The stream reply cooldown is still active.";
        return false;
    }
    lastStreamReply = now;
    return true;
}

void PresenceRuntime::RotateAvatarEventsIfNeeded(
    const std::filesystem::path& path,
    const int maximumBytes)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size < static_cast<std::uintmax_t>(maximumBytes)) return;
    const std::filesystem::path backup = path.string() + ".1";
    std::filesystem::remove(backup, error);
    error.clear();
    std::filesystem::rename(path, backup, error);
    if (error)
    {
        Notify({"Avatar", "Error", "The avatar event stream could not be rotated."});
    }
}

void PresenceRuntime::UpdatePhase(std::string phase, std::string attention)
{
    {
        std::lock_guard lock(mutex);
        snapshot.phase = std::move(phase);
        if (!attention.empty()) snapshot.attention = std::move(attention);
        ++snapshot.sequence;
    }
    WriteAvatarState(true);
}

void PresenceRuntime::WriteAvatarState(const bool appendEvent)
{
    // Writer serialization is acquired BEFORE the snapshot is captured, and that order
    // is the whole fix.
    //
    // Capturing first and writing afterwards let two threads interleave: A copies
    // sequence 40 and stalls, B copies 41 and writes it, then A writes 40 over the top.
    // Each write was atomic and the file still went backwards. Taking writer ownership
    // first means whoever is about to write reads the newest state that exists at that
    // moment, so writes are monotonic by construction rather than by luck.
    //
    // The state mutex is still only held for the copy. Disk I/O happens under the
    // writer lock alone, so a slow write never blocks an affect or phase update.
    std::lock_guard writerLock(writerMutex);

    PresenceSnapshot current;
    std::filesystem::path statePath;
    std::filesystem::path eventsPath;
    {
        std::lock_guard lock(mutex);
        current = snapshot;
        statePath = stateFile;
        eventsPath = eventFile;
    }
    if (!current.avatarBridgeEnabled || statePath.empty()) return;
    // Second line of defence, independent of the ordering above. A sequence that is not
    // newer than what is already on disk is not written at all: the file is a current
    // state, and replacing it with an older one is worse than skipping the write.
    if (lastWrittenSequence != 0 && current.sequence <= lastWrittenSequence) return;
    const nlohmann::json document = {
        {"version", 1}, {"sequence", current.sequence}, {"timestamp", IsoTimestamp()},
        {"phase", current.phase}, {"expression", Expression(current.affect)},
        {"affect_intensity", current.affectIntensity},
        {"conversation_momentum", current.conversationMomentum},
        {"attention", current.attention}, {"speaking", current.phase == "speaking"},
        {"listening", current.phase == "listening"},
        {"mouth", current.phase == "speaking" ? 1.0 : 0.0},
        {"gaze_target", current.attention}
    };
    if (!AtomicJsonWrite(statePath, document))
    {
        Notify({"Avatar", "Error", "The avatar state file could not be updated."});
        return;
    }
    // Recorded only after the write succeeded, so a failed write does not bar the
    // retry that follows it.
    lastWrittenSequence = current.sequence;
    if (appendEvent && !eventsPath.empty())
    {
        int maximumBytes = 0;
        {
            std::lock_guard lock(mutex);
            maximumBytes = configuration.maxAvatarEventBytes;
        }
        RotateAvatarEventsIfNeeded(eventsPath, maximumBytes);
        std::error_code error;
        std::filesystem::create_directories(eventsPath.parent_path(), error);
        std::ofstream stream(eventsPath, std::ios::app);
        if (stream.is_open()) stream << document.dump() << '\n';
    }
}

void PresenceRuntime::Notify(PresenceNotice notice) const
{
    NoticeHandler handler;
    {
        std::lock_guard lock(mutex);
        handler = noticeHandler;
    }
    if (handler) handler(notice);
}

} // namespace revia::presence
