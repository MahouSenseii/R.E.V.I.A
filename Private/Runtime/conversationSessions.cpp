#include "Runtime/reviaSession.h"
#include "Actions/actionTypes.h"

#include <algorithm>
#include <set>

namespace revia::runtime
{

namespace
{
constexpr const char* BusyRefusal = "Revia is still working on something. Try again once she is idle.";
}

bool ReviaSession::ConversationBrowsingAdmitted(std::string& outReason) const
{
    outReason.clear();
    if (!started.load())
        outReason = "Past chats are available once Revia has started.";
    else if (!settings.conversation.bArchiveEnabled)
        outReason = "Conversation history is off, so there are no past chats to show.";
    else if (CurrentConversationId().empty())
        outReason = "The conversation archive could not be opened.";
    else if (Audience().kind != identity::AudienceKind::Private)
        outReason = "Past chats are hidden while the audience is not private.";
    return outReason.empty();
}

std::string ReviaSession::CurrentConversationId() const
{
    std::lock_guard idLock(conversationIdMutex);
    return conversationSessionId;
}

std::vector<memory::ArchivedSession> ReviaSession::BrowseConversations(
    const std::string& query, const std::size_t maxSessions, std::string& outError) const
{
    if (!ConversationBrowsingAdmitted(outError))
        return {};
    const std::string current = CurrentConversationId();
    std::vector<memory::ArchivedSession> sessions =
        conversationArchive.RecentSessions(static_cast<std::size_t>(std::max(1, settings.conversation.maxSessions)));
    // A start that never said anything leaves an empty session; listing those would
    // bury the real conversations under one row per launch.
    std::erase_if(sessions, [&current](const memory::ArchivedSession& session) { return session.turns == 0 && session.id != current; });
    if (!query.empty())
    {
        std::set<std::string> matching;
        for (const memory::ArchivedTurn& turn : conversationArchive.Search(query, 400))
            matching.insert(turn.sessionId);
        std::erase_if(sessions, [&matching](const memory::ArchivedSession& session) { return !matching.contains(session.id); });
    }
    if (sessions.size() > maxSessions)
        sessions.resize(maxSessions);
    return sessions;
}

std::vector<memory::ArchivedTurn> ReviaSession::ConversationTranscript(const std::string& sessionId, std::string& outError) const
{
    if (!ConversationBrowsingAdmitted(outError))
        return {};
    if (!conversationArchive.HasSession(sessionId))
    {
        outError = "That conversation is no longer in the archive.";
        return {};
    }
    return conversationArchive.LoadSession(sessionId, static_cast<std::size_t>(std::clamp(settings.conversation.maxTurnsPerSession, 1, 5000)));
}

bool ReviaSession::ExportConversation(const std::string& sessionId, const bool json, std::string& outText, std::string& outError) const
{
    outText.clear();
    const std::vector<memory::ArchivedTurn> turns = ConversationTranscript(sessionId, outError);
    if (!outError.empty())
        return false;
    memory::ArchivedSession described;
    described.id = sessionId;
    for (const memory::ArchivedSession& session : conversationArchive.RecentSessions(static_cast<std::size_t>(std::max(1, settings.conversation.maxSessions))))
        if (session.id == sessionId)
            described = session;
    outText = json ? memory::RenderTranscriptJson(described, turns, DisplayName())
                   : memory::RenderTranscriptMarkdown(described, turns, DisplayName());
    return true;
}

bool ReviaSession::StartNewConversation(std::string& outError)
{
    // Unlike browsing, this works with history off: clearing the working context is
    // still a fresh chat even when nothing is archived.
    outError.clear();
    if (!started.load())
    {
        outError = "A new chat can be started once Revia has started.";
        return false;
    }
    std::unique_lock operationLock(operationMutex, std::try_to_lock);
    if (!operationLock.owns_lock() || busy.load())
    {
        outError = BusyRefusal;
        return false;
    }
    if (Audience().kind != identity::AudienceKind::Private)
    {
        outError = "A new chat can only be started while the audience is private.";
        return false;
    }
    // The working context and its continuity notes only. Long-term memory is a separate
    // store and stays exactly as it was.
    conversationRuntime.ResetParticipantContinuity();
    // Marked as already restored for the person typing, so the first turn of the new chat
    // does not pull the previous chat straight back in.
    {
        const std::scoped_lock lock(speakerMutex, audienceMutex);
        privateContextParticipant = currentSpeakerId;
        privateContextAudience = configuredAudience.audienceId;
        audienceHistory.clear();
    }
    if (!settings.conversation.bArchiveEnabled || CurrentConversationId().empty())
        return true;
    conversationArchive.EndSession(conversationSessionId);
    const std::string next = actions::NewActionId();
    std::string error;
    std::lock_guard idLock(conversationIdMutex);
    if (!conversationArchive.BeginSession(next, error))
    {
        // The context is already fresh; only archiving stops until the next start.
        appLogger.Warning("A new conversation could not be archived: " + error);
        conversationSessionId.clear();
        outError = "The new chat started, but it will not be saved: " + error;
        return true;
    }
    conversationSessionId = next;
    appLogger.Log("Started a new conversation; the working context was cleared and long-term memory left unchanged.");
    return true;
}

bool ReviaSession::ContinueConversation(const std::string& sessionId, std::string& outError)
{
    if (!ConversationBrowsingAdmitted(outError))
        return false;
    std::unique_lock operationLock(operationMutex, std::try_to_lock);
    if (!operationLock.owns_lock() || busy.load())
    {
        outError = BusyRefusal;
        return false;
    }
    if (sessionId == CurrentConversationId())
        return true;
    // The scope a typed turn would carry right now. The archive returns only turns that
    // belong to it, so another person's half of an old chat never enters this context.
    memory::MemoryScope scope;
    {
        const std::scoped_lock lock(speakerMutex, audienceMutex);
        scope.participantId = currentSpeakerId;
        scope.audience = configuredAudience;
        scope.companionId = companionPaths.Descriptor().id;
        if (!currentSpeakerId.empty() && currentSpeakerId != "local:user")
            scope.participantSource = identity::SpeakerSource::ExplicitIntroduction;
    }
    if (!memory::IsAttributedPrivateScope(scope))
    {
        outError = "Continuing a chat needs Revia to know who is typing. Introduce yourself first; the chat can still be read.";
        return false;
    }
    const std::vector<memory::ArchivedTurn> tail = conversationArchive.LoadSessionTail(
        sessionId, static_cast<std::size_t>(std::clamp(settings.conversation.maxTurnsPerSession, 1, 500)), scope);
    if (tail.empty())
    {
        outError = "Nothing in that chat belongs to the person typing now, so there is nothing to continue.";
        return false;
    }
    std::vector<conversationMessage> restored;
    for (const auto& turn : tail)
        restored.push_back({turn.role, turn.content, turn.scope.participantId, turn.scope});
    conversationRuntime.ResetParticipantContinuity();
    const int wanted = settings.conversation.restoreTurns > 0 ? settings.conversation.restoreTurns : 12;
    context.RestoreMessages(restored, static_cast<std::size_t>(wanted));
    {
        const std::scoped_lock lock(speakerMutex, audienceMutex);
        privateContextParticipant = scope.participantId;
        privateContextAudience = scope.audience.audienceId;
        audienceHistory.clear();
    }
    conversationArchive.EndSession(CurrentConversationId());
    {
        std::lock_guard idLock(conversationIdMutex);
        conversationSessionId = sessionId;
    }
    appLogger.Log("Continued an archived conversation with " + std::to_string(tail.size()) + " of the speaker's own turns.");
    PublishComponent("Conversation history", "Restored", "Continued an earlier chat with the current speaker's own turns.", -1.0,
        static_cast<int>(tail.size()));
    return true;
}

bool ReviaSession::RenameConversation(const std::string& sessionId, const std::string& title, std::string& outError)
{
    if (!ConversationBrowsingAdmitted(outError))
        return false;
    return conversationArchive.RenameSession(sessionId, title, outError);
}

bool ReviaSession::DeleteConversation(const std::string& sessionId, std::string& outError)
{
    if (!ConversationBrowsingAdmitted(outError))
        return false;
    if (!conversationArchive.HasSession(sessionId))
    {
        outError = "That conversation is no longer in the archive.";
        return false;
    }
    if (sessionId != CurrentConversationId())
    {
        conversationArchive.ForgetSession(sessionId);
        if (conversationArchive.HasSession(sessionId))
        {
            outError = "The conversation archive could not delete that chat.";
            return false;
        }
        return true;
    }
    // Deleting the chat in progress also clears what the prompt still carries from it;
    // a delete that leaves the same turns in the working context is not one.
    std::unique_lock operationLock(operationMutex, std::try_to_lock);
    if (!operationLock.owns_lock() || busy.load())
    {
        outError = BusyRefusal;
        return false;
    }
    conversationArchive.ForgetSession(sessionId);
    conversationRuntime.ResetParticipantContinuity();
    std::string error;
    if (!conversationArchive.BeginSession(sessionId, error))
    {
        std::lock_guard idLock(conversationIdMutex);
        conversationSessionId.clear();
        outError = "The chat was deleted, but new turns will not be saved: " + error;
    }
    return true;
}

} // namespace revia::runtime
