#pragma once
#include "Runtime/reviaSession.h"
#include <nlohmann/json.hpp>

namespace revia::runtime
{
struct ReviaSessionTestAccess
{
    // A copied transport owns the env credential. Saved session settings never contain it.
    static void QualificationTransport(ReviaSession& session, const std::string& key, bool retrieval)
    {
        auto endpoint = session.settings.llm;
        endpoint.apiKey = key;
        auto profile = session.profile;
        profile.bMemoryEnabled = retrieval;
        session.router.ApplyLLMSettings(endpoint, endpoint, endpoint, session.settings.embedding, profile, false, false);
    }

    static nlohmann::json DisabledHosts(const appSettings& settings)
    {
        const auto& s = settings;
        return {{"llm_autostart", s.llm.bAutoStartServer}, {"cloud", s.llm.cloud.bEnabled},
            {"intelligence", s.intelligence.bEnabled}, {"fast", s.intelligence.fast.bEnabled},
            {"fast_warm", s.intelligence.fast.bWarmAtStartup}, {"expert", s.intelligence.expert.bEnabled},
            {"expert_warm", s.intelligence.expert.bWarmAtStartup}, {"embedding", s.embedding.bEnabled},
            {"embedding_autostart", s.embedding.bAutoStartServer}, {"reranker", s.memoryReranker.enabled},
            {"nli", s.memoryNli.enabled}, {"speech", s.speech.bEnabled}, {"greeting", s.speech.bSpeakGreeting},
            {"recognition", s.speechRecognition.bEnabled}, {"hands_free", s.speechRecognition.bHandsFree},
            {"presence", s.presence.bEnabled}, {"avatar", s.presence.bAvatarBridgeEnabled},
            {"adapters", s.presence.bExternalAdaptersEnabled}, {"vision", s.vision.bEnabled},
            {"llm_vision", s.llm.bVisionEnabled}, {"perception", s.perception.bEnabled},
            {"initiative", s.initiative.bEnabled}, {"curiosity", s.initiative.bCuriosityEnabled},
            {"barge_in", s.bargeIn.bEnabled}, {"image", s.image.bEnabled}, {"video", s.video.bEnabled},
            {"performance", s.performance.bEnabled}, {"improvement", s.improvement.bEnabled},
            {"local_api", s.localApi.bEnabled}, {"archive", s.conversation.bArchiveEnabled},
            {"self_inquiry", s.conversation.bSelfInquiryEnabled}, {"ai_review", s.responseFilter.bAiReviewEnabled},
            {"mcp", !s.mcp.servers.empty()}};
    }

    static nlohmann::json DisabledHosts(const ReviaSession& session) { return DisabledHosts(session.settings); }
    static std::size_t QualificationContextSize(const ReviaSession& session) { return session.context.GetRecentMessages().size(); }
    // Interactive and learning memory work still queued; a running task is not counted.
    static std::size_t QueuedMemoryWork(const ReviaSession& session)
    {
        const auto depths = session.turnCoordinator.Memory().Depths();
        return depths.interactive + depths.learning;
    }
    static bool QualificationMemoryEnabled(const ReviaSession& session) { return session.profile.bMemoryEnabled; }
};
}
