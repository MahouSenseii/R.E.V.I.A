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
    else if (!ConversationSnapshot().bArchiveEnabled)
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
    return conversationArchive.LoadSession(
        sessionId, static_cast<std::size_t>(std::clamp(settings.conversation.maxTurnsPerSession, 1, 5000)));
}

bool ReviaSession::ExportConversation(const std::string& sessionId, const bool json, std::string& outText, std::string& outError) const
{
    outText.clear();
    const std::vector<memory::ArchivedTurn> turns = ConversationTranscript(sessionId, outError);
    if (!outError.empty())
        return false;
    memory::ArchivedSession described;
    described.id = sessionId;
    for (const memory::ArchivedSession& session :
        conversationArchive.RecentSessions(static_cast<std::size_t>(std::max(1, settings.conversation.maxSessions))))
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
    lastExchange.reset();
    // A task delegated from the chat being left never reports into the new one.
    conversationEpoch.fetch_add(1);
    // Marked as already restored for the person typing, so the first turn of the new chat
    // does not pull the previous chat straight back in.
    {
        const std::scoped_lock lock(speakerMutex, audienceMutex);
        privateContextParticipant = currentSpeakerId;
        privateContextAudience = configuredAudience.audienceId;
        audienceHistory.clear();
    }
    if (!ConversationSnapshot().bArchiveEnabled || CurrentConversationId().empty())
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
    {
        conversationMessage message{turn.role, turn.content, turn.scope.participantId, turn.scope};
        message.mediaReferences = turn.mediaReferences;
        restored.push_back(std::move(message));
    }
    conversationRuntime.ResetParticipantContinuity();
    const int wanted = settings.conversation.restoreTurns > 0 ? settings.conversation.restoreTurns : 12;
    context.RestoreMessages(restored, static_cast<std::size_t>(wanted));
    lastExchange.reset();
    conversationEpoch.fetch_add(1);
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
        outError = WithdrawLiveCaptures("conversation:" + sessionId);
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
    lastExchange.reset();
    const std::string withdrawal = WithdrawLiveCaptures("conversation:" + sessionId);
    // The id is reused for the fresh chat, so the epoch is what tells a delegated task apart.
    conversationEpoch.fetch_add(1);
    std::string error;
    if (!conversationArchive.BeginSession(sessionId, error))
    {
        std::lock_guard idLock(conversationIdMutex);
        conversationSessionId.clear();
        outError = "The chat was deleted, but new turns will not be saved: " + error;
    }
    if (!withdrawal.empty())
        outError = outError.empty() ? withdrawal : outError + " " + withdrawal;
    return true;
}

void ReviaSession::RememberExchange(const agents::InputBatch& batch, const SessionResult& result, const bool privateContext)
{
    RetractableExchange exchange;
    exchange.privateContext = privateContext;
    exchange.audienceRevision = batch.context.audience.revision;
    exchange.participantId = batch.context.participantId;
    exchange.input = batch.text;
    exchange.conversationEpoch = conversationEpoch.load();
    if (privateContext)
    {
        // Read back from the context itself, which may have bounded either message, so
        // the retraction later compares against exactly what is stored.
        const std::vector<conversationMessage> messages = context.GetRecentMessages();
        const bool answered = result.succeeded && result.fromAssistant && !result.text.empty() && messages.size() >= 2 &&
                              messages[messages.size() - 2].role == "user" && messages.back().role == "assistant";
        const bool failed = !result.succeeded && !messages.empty() && messages.back().role == "user";
        if (!answered && !failed)
            return;
        const auto& user = messages[messages.size() - (answered ? 2 : 1)];
        exchange.contextUser = user.content;
        exchange.mediaReferences = user.mediaReferences;
        exchange.hasAssistant = answered;
        if (answered)
        {
            exchange.contextAssistant = messages.back().content;
            exchange.archivedAssistant = result.text;
            exchange.generatedMediaReferences = messages.back().mediaReferences;
        }
    }
    else
    {
        if (!result.succeeded || !result.fromAssistant || result.text.empty())
            return;
        exchange.contextUser = batch.text;
        exchange.contextAssistant = result.text;
    }
    lastExchange = std::move(exchange);
}

SessionResult ReviaSession::RegenerateLast(bool& outRetracted)
{
    return RetryLastExchange({}, outRetracted);
}

SessionResult ReviaSession::ReplaceLastUserTurn(const std::string& replacement, bool& outRetracted)
{
    outRetracted = false;
    if (replacement.empty())
    {
        SessionResult refused;
        refused.succeeded = false;
        refused.text = refused.reason = "The edited message is empty.";
        return refused;
    }
    return RetryLastExchange(replacement, outRetracted);
}

SessionResult ReviaSession::RetryLastExchange(const std::string& replacement, bool& outRetracted)
{
    outRetracted = false;
    SessionResult refused;
    refused.succeeded = false;
    refused.fromAssistant = false;
    const auto refuse = [&refused](const std::string& why)
    {
        refused.text = why;
        refused.reason = why;
        return refused;
    };
    if (!started.load())
        return refuse("Revia is not ready yet.");
    std::string input;
    std::vector<conversationMediaReference> replayMedia;
    std::function<bool()> originAdmission;
    {
        std::lock_guard operationLock(operationMutex);
        DrainForgottenMedia();
        if (!lastExchange)
            return refuse("There is no reply to replace. Only Revia's latest conversational reply can be regenerated or edited.");
        const RetractableExchange exchange = *lastExchange;
        // The same capture Submit makes, so the replacement runs for the same audience
        // or not at all.
        const agents::InputContext current = CaptureInputContext(agents::InputSource::Typed);
        if (conversationEpoch.load() != exchange.conversationEpoch || current.audience.revision != exchange.audienceRevision ||
            current.participantId != exchange.participantId ||
            (current.audience.kind == identity::AudienceKind::Private) != exchange.privateContext)
            return refuse("The audience changed since that reply, so it was left as it was.");
        if (exchange.privateContext)
        {
            const std::vector<conversationMessage> messages = context.GetRecentMessages();
            const auto required = exchange.hasAssistant ? 2u : 1u;
            if (messages.size() < required ||
                (exchange.hasAssistant && (messages.back().role != "assistant" || messages.back().content != exchange.contextAssistant ||
                                              messages.back().mediaReferences != exchange.generatedMediaReferences)) ||
                messages[messages.size() - required].role != "user" ||
                messages[messages.size() - required].content != exchange.contextUser ||
                messages[messages.size() - required].mediaReferences != exchange.mediaReferences)
                return refuse("The conversation has moved on since that reply, so it was left as it was.");
            if (exchange.hasAssistant)
                (void)context.RemoveLastMessageIf("assistant", exchange.contextAssistant);
            (void)context.RemoveLastMessageIf("user", exchange.contextUser);
            const std::string archiveId = CurrentConversationId();
            if (!archiveId.empty())
            {
                // Each store retracts only its own exact newest turn. One the archive
                // withheld is simply not found, and nothing older is touched.
                if (exchange.hasAssistant)
                    (void)conversationArchive.RemoveLastTurnIf(archiveId, "assistant", exchange.archivedAssistant);
                (void)conversationArchive.RemoveLastTurnIf(archiveId, "user", exchange.input);
            }
        }
        else
        {
            std::lock_guard lock(audienceMutex);
            const std::size_t size = audienceHistory.size();
            if (size < 2 || audienceHistory[size - 1].role != "assistant" ||
                audienceHistory[size - 1].content != exchange.contextAssistant || audienceHistory[size - 2].role != "user" ||
                audienceHistory[size - 2].content != exchange.contextUser)
                return refuse("The conversation has moved on since that reply, so it was left as it was.");
            audienceHistory.resize(size - 2);
        }
        lastExchange.reset();
        input = replacement.empty() ? exchange.input : replacement;
        replayMedia = exchange.mediaReferences;
        originAdmission = [this, current, epoch = exchange.conversationEpoch]()
        {
            return conversationEpoch.load() == epoch && InputContextCurrent(current) &&
                   current.stamp.policyVersion == companionAuthority->Revision();
        };
    }
    outRetracted = true;
    return SubmitTurn(input, agents::InputSource::Typed, {}, std::move(replayMedia), std::move(originAdmission));
}

} // namespace revia::runtime
