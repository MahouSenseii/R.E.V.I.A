#pragma once

#include "testSupport.h"
#include "Runtime/reviaSession.h"

namespace revia::runtime
{

struct ReviaSessionTestAccess
{
    static speech::SpeechService& Speech(ReviaSession& session) { return session.speechService; }
    static emotion::EmotionRuntime& Emotions(ReviaSession& session) { return session.emotionRuntime; }
    static AffectController& LegacyAffect(ReviaSession& session) { return session.affectController; }
    static identity::RelationshipRegistry& People(ReviaSession& session) { return session.relationships; }

    static void MaintenanceEvery(ReviaSession& session, std::chrono::milliseconds emotionInterval,
        std::chrono::milliseconds relationshipQuiet, std::chrono::milliseconds conversationQuiet)
    {
        session.emotionSettleInterval = emotionInterval;
        session.relationshipQuietInterval = relationshipQuiet;
        session.quietConversationInterval = conversationQuiet;
    }

    static goals::Goal RunGoal(ReviaSession& session, goals::Goal goal, bool cancelBeforeRun = false)
    {
        std::lock_guard lock(session.operationMutex);
        (void)session.BeginOperation();
        if (cancelBeforeRun) session.RequestStop();
        return session.RunGoalUnlocked(std::move(goal));
    }

    static std::filesystem::path ActiveVoiceDirectory(const ReviaSession& session)
    { return session.speechService.ActiveVocalizationDirectory(); }

    static agents::MemoryAgent& Memory(ReviaSession& session)
    { return session.turnCoordinator.Memory(); }

    static void SubmitMemoryEvaluation(ReviaSession& session, std::string input, std::uint64_t turnId)
    { session.turnCoordinator.Memory().Submit(session.router, std::move(input), "", turnId); }

    static agents::LearnedFindingResult SubmitLearning(ReviaSession& session, memoryDecision decision)
    { return session.turnCoordinator.SubmitLearnedFinding(session.router, std::move(decision)); }

    static agents::LearnedFindingResult SubmitLearning(ReviaSession& session,
        const messageRouter& router, memoryDecision decision)
    {
        return session.turnCoordinator.SubmitLearnedFinding(router, std::move(decision), 47);
    }

    static void IdentitySaveEvery(ReviaSession& session, std::chrono::milliseconds interval)
    {
        session.identitySaveInterval = interval;
    }

    static void RememberIdentity(ReviaSession& session, const std::string& name)
    {
        session.relationships.SetDisplayName(identity::LocalUserEntityId(), name);
        session.relationships.ReinforcePreference("fixture astronomy", true,
            identity::PreferenceSource::Observed);
        auto development = session.relationships.Development();
        development.delta[identity::Trait::Patience] = 0.07F;
        session.relationships.SetDevelopment(development);
        auto mood = session.emotionRuntime.Mood();
        mood.valence = 0.31F;
        session.emotionRuntime.SetMood(mood);
    }

    static std::unique_lock<std::mutex> HoldForeground(ReviaSession& session)
    {
        return std::unique_lock(session.operationMutex);
    }

    static std::stop_token OperationToken(ReviaSession& session)
    {
        return session.CurrentOperationToken();
    }

    static void PrepareActions(ReviaSession& session, const std::filesystem::path& root)
    {
        std::string error;
        const bool initialized = session.actionRuntime.Initialize(
            root / "capabilities.json", root / "session-audit.jsonl", error);
        tests::Check(initialized, error);
        (void)session.BeginOperation();
    }

    static SessionResult Execute(ReviaSession& session, actions::ActionRequest request)
    {
        return session.ExecuteAction(std::move(request));
    }

    static void ObserveActions(ReviaSession& session, actions::ActionRuntime::DispatchObserver observer)
    {
        session.actionRuntime.SetDispatchObserver(std::move(observer));
    }
};

} // namespace revia::runtime
