#pragma once

#include "Library/structLibrary.h"
#include "Runtime/runtimeEvents.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_set>

namespace revia::presence
{

struct ExternalAdapterEvent
{
    int version = 1;
    std::string id;
    std::string source;
    std::string channel;
    std::string authorId;
    std::string author;
    std::string role = "viewer";
    std::string text;
    bool addressedToRevia = false;
};

struct PresenceNotice
{
    std::string component;
    std::string phase;
    std::string detail;
    int queueDepth = 0;
};

struct PresenceSnapshot
{
    bool enabled = false;
    bool avatarBridgeEnabled = false;
    bool adaptersEnabled = false;
    std::string phase = "offline";
    runtime::AffectState affect = runtime::AffectState::Neutral;
    float affectIntensity = 0.0F;
    float conversationMomentum = 0.0F;
    std::string attention = "local user";
    std::uint64_t sequence = 0;
    std::size_t pendingAdapterEvents = 0;
};

// Coordinates presentation state and narrow local adapter I/O. It consumes runtime
// events but never calls the LLM or action runtime. External messages are handed to the
// session through a callback, where they use a conversation-only execution path.
class PresenceRuntime
{
public:
    using NoticeHandler = std::function<void(const PresenceNotice&)>;
    using AdapterHandler = std::function<void(const ExternalAdapterEvent&)>;

    PresenceRuntime() = default;
    ~PresenceRuntime();

    PresenceRuntime(const PresenceRuntime&) = delete;
    PresenceRuntime& operator=(const PresenceRuntime&) = delete;

    bool Start(
        const presenceSettings& settings,
        NoticeHandler noticeHandler,
        AdapterHandler adapterHandler);
    void Observe(const runtime::RuntimeEvent& event);
    void RecordUserInput(const std::string& sourceLabel);
    void PublishAdapterReply(
        const ExternalAdapterEvent& request,
        const std::string& text,
        bool succeeded,
        const std::string& reason = {});
    [[nodiscard]] PresenceSnapshot Snapshot() const;
    void Shutdown();

private:
    void RunAdapterInbox(std::stop_token stopToken);
    void ScanAdapterInbox();
    bool ParseAdapterFile(
        const std::filesystem::path& path,
        ExternalAdapterEvent& outEvent,
        std::string& outError) const;
    bool RateLimitAllows(std::chrono::steady_clock::time_point now);
    bool RememberAdapterEvent(const ExternalAdapterEvent& event);
    bool StreamPolicyAllows(
        const ExternalAdapterEvent& event,
        std::chrono::steady_clock::time_point now,
        std::string& outReason);
    void RotateAvatarEventsIfNeeded(const std::filesystem::path& path, int maximumBytes);
    void UpdatePhase(std::string phase, std::string attention = {});
    void WriteAvatarState(bool appendEvent);
    void Notify(PresenceNotice notice) const;

    mutable std::mutex mutex;
    presenceSettings configuration;
    NoticeHandler noticeHandler;
    AdapterHandler adapterHandler;
    PresenceSnapshot snapshot;
    std::chrono::steady_clock::time_point lastConversationActivity{};
    std::chrono::steady_clock::time_point lastStreamReply{};
    std::deque<std::chrono::steady_clock::time_point> adapterAdmissions;
    std::deque<std::string> recentAdapterIdOrder;
    std::unordered_set<std::string> recentAdapterIds;
    std::filesystem::path inboxRoot;
    std::filesystem::path outboxRoot;
    std::filesystem::path stateFile;
    std::filesystem::path eventFile;
    std::jthread adapterWorker;
};

} // namespace revia::presence
