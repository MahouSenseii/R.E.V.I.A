#pragma once

#include "Library/structLibrary.h"
#include "Perception/windowEventMonitor.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace revia::initiative
{

enum class StarterCueKind
{
    FocusCompleted,
    ReturnedToApplication,
    ContextSwitching,
    SelfDirectedCuriosity
};

struct StarterCue
{
    StarterCueKind kind = StarterCueKind::FocusCompleted;
    std::string messageIntent;
    std::string evidence;
    float confidence = 0.0f;
    std::chrono::system_clock::time_point occurredAt = std::chrono::system_clock::now();
};

// Turns admitted desktop transitions into bounded reasons to begin a conversation.
//
// It never decides whether Revia may interrupt and never generates dialogue. It only
// recognizes meaningful patterns in event order. AttentionPolicy owns permission and
// ConversationRuntime owns the eventual wording.
class ConversationStarter
{
public:
    void Configure(initiativeSettings settings);
    void UpdateSettings(initiativeSettings settings);
    void Observe(const perception::WindowObservation& observation);
    // One-shot opportunities. Once the attention policy considers a cue, a timer cannot
    // revive it later after its conversational moment has passed.
    [[nodiscard]] std::vector<StarterCue> RecentCues(
        std::chrono::system_clock::time_point now);
    void Clear();

private:
    struct Transition
    {
        std::string application;
        std::chrono::system_clock::time_point occurredAt;
    };

    void RememberCue(StarterCue cue);
    [[nodiscard]] static std::string FriendlyApplication(const std::string& executable);

    mutable std::mutex mutex;
    initiativeSettings configuration;
    std::string currentApplication;
    std::chrono::system_clock::time_point currentStarted{};
    std::unordered_map<std::string, std::chrono::system_clock::time_point> lastSeen;
    std::deque<Transition> transitions;
    std::deque<StarterCue> cues;
    std::string lastCueEvidence;
};

} // namespace revia::initiative
