#include "Presence/presenceRuntime.h"

#include <algorithm>
#include <cctype>
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

    bool AtomicJsonWrite(const std::filesystem::path& path, const nlohmann::json& document)
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            return false;
        }
        const std::filesystem::path temporary = path.string() + ".tmp";
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
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
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
        inboxRoot = settings.inboxPath;
        outboxRoot = settings.outboxPath;
        stateFile = settings.statePath;
        eventFile = settings.eventPath;
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
            ? "Avatar state is available at " + settings.statePath + "."
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
    std::uint64_t sequence = 0;
    {
        std::lock_guard lock(mutex);
        settings = configuration;
        sequence = ++snapshot.sequence;
    }
    if (!settings.bExternalAdaptersEnabled)
    {
        return;
    }
    const std::string safeId = SafeToken(request.id).empty()
        ? std::to_string(sequence)
        : SafeToken(request.id);
    const nlohmann::json response = {
        {"version", 1}, {"id", safeId}, {"source", request.source},
        {"channel", request.channel}, {"succeeded", succeeded},
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
        write = snapshot.enabled && snapshot.avatarBridgeEnabled;
        snapshot.phase = "offline";
        ++snapshot.sequence;
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
        if (!parsed)
        {
            Notify({"Adapters", "Rejected", parseError});
            continue;
        }
        if (!RateLimitAllows(std::chrono::steady_clock::now()))
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
    outEvent.id = SafeToken(document.value("id", path.stem().string()));
    outEvent.source = Lower(SafeToken(document.value("source", "")));
    outEvent.channel = SafeToken(document.value("channel", "default"));
    outEvent.author = document.value("author", "user");
    outEvent.text = document.value("text", "");
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
    if (appendEvent && !eventsPath.empty())
    {
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
