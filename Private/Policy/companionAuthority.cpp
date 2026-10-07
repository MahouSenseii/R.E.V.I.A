#include "Policy/companionAuthority.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <system_error>
#include <set>
#include <utility>

namespace revia::policy
{
namespace
{

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return std::tolower(ch); });
    return value;
}

bool WithinRoot(const std::filesystem::path& path, const std::filesystem::path& root)
{
    if (path.empty() || !root.is_absolute())
        return false;
    std::error_code error;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error)
        return false;
    auto pathPart = path.begin();
    for (auto rootPart = canonicalRoot.begin(); rootPart != canonicalRoot.end(); ++rootPart, ++pathPart)
    {
        if (pathPart == path.end())
            return false;
        auto pathValue = pathPart->wstring();
        auto rootValue = rootPart->wstring();
#ifdef _WIN32
        const auto lower = [](const wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); };
        std::transform(pathValue.begin(), pathValue.end(), pathValue.begin(), lower);
        std::transform(rootValue.begin(), rootValue.end(), rootValue.begin(), lower);
#endif
        if (pathValue != rootValue)
            return false;
    }
    return true;
}

bool Matches(const AuthorityPermissions& permissions, const actions::ActionRequest& request, const actions::PolicyDecision& decision,
    const std::string& resource)
{
    if (permissions.withinMachineCeiling)
        return true;
    if (std::find(permissions.operations.begin(), permissions.operations.end(), request.type) == permissions.operations.end())
        return false;
    if (request.type == actions::ActionType::GenerateImage)
        return !decision.canonicalDestination.empty() && (permissions.roots.empty() ||
            std::any_of(permissions.roots.begin(), permissions.roots.end(),
                [&](const auto& root) { return WithinRoot(decision.canonicalDestination, root); }));
    const bool filesystemOperation =
        request.type == actions::ActionType::ListDirectory || request.type == actions::ActionType::ReadTextFile ||
        request.type == actions::ActionType::CreateDirectory || request.type == actions::ActionType::CopyFile ||
        request.type == actions::ActionType::MoveFile || request.type == actions::ActionType::RenamePath ||
        request.type == actions::ActionType::MoveToRecycleBin || request.type == actions::ActionType::WriteTextFile;
    if (filesystemOperation && decision.canonicalSource.empty())
        return false;
    if (!decision.canonicalSource.empty() || !decision.canonicalDestination.empty())
    {
        const auto admitted = [&](const std::filesystem::path& path)
        {
            return path.empty() || std::any_of(permissions.roots.begin(), permissions.roots.end(),
                                       [&](const auto& root) { return WithinRoot(path, root); });
        };
        if (!admitted(decision.canonicalSource) || !admitted(decision.canonicalDestination))
            return false;
    }
    if (filesystemOperation)
        return true;
    if (request.type == actions::ActionType::WebSearch)
    {
        return !permissions.internetHosts.empty() &&
               (resource.empty() || std::any_of(permissions.internetHosts.begin(), permissions.internetHosts.end(),
                                        [&](const auto& host) { return Lower(host) == Lower(resource); }));
    }
    const auto application = request.type == actions::ActionType::ExecuteProcess
        ? actions::PathToUtf8(decision.canonicalExecutable)
        : request.application.empty() ? std::string("desktop") : request.application;
    return std::any_of(permissions.applications.begin(), permissions.applications.end(),
        [&](const auto& admitted) { return Lower(admitted) == Lower(application); });
}

bool MatchesSubject(const runtime::RuntimeStamp& scope, const runtime::RuntimeStamp& subject)
{
    return scope.SameSession(subject) && scope.taskId == subject.taskId &&
           (scope.attemptId.empty() || scope.attemptId == subject.attemptId);
}

bool Denies(const AuthorityPermissions& permissions, const actions::ActionRequest& request, const actions::PolicyDecision& decision,
    const std::string& resource)
{
    if (!decision.canonicalSource.empty() && !decision.canonicalDestination.empty())
    {
        auto sourceOnly = decision;
        sourceOnly.canonicalDestination.clear();
        auto destinationOnly = decision;
        destinationOnly.canonicalSource = decision.canonicalDestination;
        destinationOnly.canonicalDestination.clear();
        return Matches(permissions, request, sourceOnly, resource) || Matches(permissions, request, destinationOnly, resource);
    }
    return Matches(permissions, request, decision, resource);
}

bool ValidGrant(const AuthorityGrantRequest& request)
{
    const auto& permissions = request.scope.permissions;
    const bool ownedImageOnly = permissions.operations.size() == 1 &&
        permissions.operations.front() == actions::ActionType::GenerateImage;
    return !permissions.withinMachineCeiling && !permissions.operations.empty() &&
           std::find(permissions.operations.begin(), permissions.operations.end(), actions::ActionType::Unknown) ==
               permissions.operations.end() &&
           (ownedImageOnly || !permissions.roots.empty() || !permissions.applications.empty() || !permissions.internetHosts.empty()) &&
           std::all_of(permissions.roots.begin(), permissions.roots.end(), [](const auto& root) { return root.is_absolute(); }) &&
           request.lifetime > std::chrono::seconds::zero() && request.lifetime <= std::chrono::hours(24);
}

} // namespace

AuthorityPermissions AuthorityPermissions::WithinMachineCeiling()
{
    AuthorityPermissions permissions;
    permissions.withinMachineCeiling = true;
    return permissions;
}

CompanionAuthority::CompanionAuthority(OwnerVerifier inputVerifier, Clock inputClock)
    : verifier(std::move(inputVerifier)), clock(std::move(inputClock))
{
    if (!clock)
        clock = [] { return std::chrono::steady_clock::now(); };
}

std::string CompanionAuthority::SessionKey(const runtime::RuntimeStamp& stamp)
{
    return std::to_string(stamp.companionId.size()) + ":" + stamp.companionId + std::to_string(stamp.sessionId.size()) + ":" +
           stamp.sessionId + ":" + std::to_string(stamp.generation);
}

bool CompanionAuthority::SubjectExists(const runtime::RuntimeStamp& stamp) const
{
    const auto session = sessions.find(SessionKey(stamp));
    return session != sessions.end() && (stamp.taskId.empty() || session->second.tasks.contains(stamp.taskId));
}

void CompanionAuthority::SetCompanionDefaults(const std::string& companionId, AuthorityPermissions permissions)
{
    if (companionId.empty())
        return;
    std::lock_guard lock(mutex);
    defaults[companionId] = std::move(permissions);
    ++revision;
}

bool CompanionAuthority::RegisterSession(const runtime::RuntimeStamp& stamp)
{
    if (stamp.companionId.empty() || stamp.sessionId.empty() || stamp.generation == 0 || !stamp.taskId.empty())
        return false;
    std::lock_guard lock(mutex);
    const bool inserted = sessions.emplace(SessionKey(stamp), Session{stamp, {}}).second;
    if (inserted)
        ++revision;
    return inserted;
}

void CompanionAuthority::EndSession(const runtime::RuntimeStamp& stamp)
{
    std::lock_guard lock(mutex);
    if (sessions.erase(SessionKey(stamp)))
    {
        std::erase_if(records, [&](const auto& entry) { return entry.second.scope.subject.SameSession(stamp); });
        ++revision;
    }
}

bool CompanionAuthority::RegisterTask(const runtime::RuntimeStamp& stamp, const std::string& parentTaskId, AuthorityPermissions restriction)
{
    std::lock_guard lock(mutex);
    auto session = sessions.find(SessionKey(stamp));
    if (session == sessions.end() || stamp.taskId.empty() || (!parentTaskId.empty() && !session->second.tasks.contains(parentTaskId)))
        return false;
    const bool inserted = session->second.tasks.emplace(stamp.taskId, Task{parentTaskId, std::move(restriction)}).second;
    if (inserted)
        ++revision;
    return inserted;
}

void CompanionAuthority::EndTask(const runtime::RuntimeStamp& stamp)
{
    std::lock_guard lock(mutex);
    const auto session = sessions.find(SessionKey(stamp));
    if (session != sessions.end() && session->second.tasks.contains(stamp.taskId))
    {
        std::set<std::string> ended{stamp.taskId};
        bool added = true;
        while (added)
        {
            added = false;
            for (const auto& [id, task] : session->second.tasks)
                if (ended.contains(task.parent))
                    added = ended.insert(id).second || added;
        }
        std::erase_if(session->second.tasks, [&](const auto& entry) { return ended.contains(entry.first); });
        std::erase_if(records, [&](const auto& entry)
            { return entry.second.scope.subject.SameSession(stamp) && ended.contains(entry.second.scope.subject.taskId); });
        ++revision;
    }
}

bool CompanionAuthority::GrantAuthenticated(const AuthorityGrantRequest& request, std::string& outId, std::string& outError)
{
    outId.clear();
    const auto proposal = request;
    if (!ValidGrant(proposal) || !verifier)
    {
        outError = "An exact bounded scope and trusted owner verifier are required.";
        return false;
    }
    std::uint64_t observedRevision;
    {
        std::lock_guard lock(mutex);
        if (!SubjectExists(proposal.scope.subject))
        {
            outError = "The authority subject is not active.";
            return false;
        }
        observedRevision = revision;
    }
    // Authentication may block. Never hold the ledger lock while waiting for the owner.
    bool verified = false;
    try
    {
        verified = verifier(proposal);
    }
    catch (...)
    {
        verified = false;
    }
    std::lock_guard lock(mutex);
    if (!verified || revision != observedRevision || !SubjectExists(proposal.scope.subject))
    {
        outError = "Owner authentication failed or the authority changed during authentication.";
        return false;
    }
    outId = actions::NewActionId();
    records.emplace(outId, Record{proposal.scope, false, clock() + proposal.lifetime});
    ++revision;
    outError.clear();
    return true;
}

std::string CompanionAuthority::Deny(const AuthorityScope& scope)
{
    std::lock_guard lock(mutex);
    const auto& subject = scope.subject;
    const bool companionScope = subject.sessionId.empty() && subject.taskId.empty() && subject.attemptId.empty() &&
                                subject.generation == 0 && defaults.contains(subject.companionId);
    if (!companionScope && !SubjectExists(subject))
        return {};
    const std::string id = actions::NewActionId();
    records.emplace(id, Record{scope, true, std::chrono::steady_clock::time_point::max()});
    ++revision;
    return id;
}

bool CompanionAuthority::Revoke(const std::string& id)
{
    std::lock_guard lock(mutex);
    const bool removed = records.erase(id) != 0;
    if (removed)
        ++revision;
    return removed;
}

void CompanionAuthority::SetEmergencyStopped(const std::string& companionId, const bool stopped)
{
    std::lock_guard lock(mutex);
    emergencyStops[companionId] = stopped;
    ++revision;
}

std::uint64_t CompanionAuthority::Revision() const
{
    std::lock_guard lock(mutex);
    return revision;
}

bool CompanionAuthority::IsActive(const runtime::RuntimeStamp& stamp) const
{
    const std::lock_guard lock(mutex);
    const auto stopped = emergencyStops.find(stamp.companionId);
    return SubjectExists(stamp) && (stopped == emergencyStops.end() || !stopped->second);
}

std::string CompanionAuthority::Evaluate(const runtime::RuntimeStamp& stamp, const actions::ActionRequest& request,
    const actions::PolicyDecision& decision, const std::string& effectResource) const
{
    std::lock_guard lock(mutex);
    if (decision.verdict == actions::PolicyVerdict::Blocked)
        return "The machine policy ceiling blocks this operation.";
    if (!SubjectExists(stamp))
        return "The companion session or task authority is no longer active.";
    const auto stopped = emergencyStops.find(stamp.companionId);
    if (stopped != emergencyStops.end() && stopped->second)
        return "Companion authority is emergency stopped.";
    const auto companionDefaults = defaults.find(stamp.companionId);
    const auto& session = sessions.at(SessionKey(stamp));
    runtime::RuntimeStamp current = stamp;
    const auto now = clock();
    for (const auto& [id, record] : records)
        if (record.denial && record.scope.subject.companionId == stamp.companionId && record.scope.subject.sessionId.empty() &&
            Denies(record.scope.permissions, request, decision, effectResource))
            return "An explicit companion denial blocks this operation.";
    for (;;)
    {
        bool allowed = companionDefaults != defaults.end() && Matches(companionDefaults->second, request, decision, effectResource);
        for (const auto& [id, record] : records)
        {
            if (!MatchesSubject(record.scope.subject, current))
                continue;
            if (record.denial && Denies(record.scope.permissions, request, decision, effectResource))
                return "An explicit owner denial blocks this operation.";
            if (!record.denial && record.expires > now && Matches(record.scope.permissions, request, decision, effectResource))
                allowed = true;
        }
        // Session denials also cover every task; grants remain scoped to their task.
        if (!current.taskId.empty())
        {
            auto sessionSubject = current;
            sessionSubject.taskId.clear();
            for (const auto& [id, record] : records)
                if (record.denial && MatchesSubject(record.scope.subject, sessionSubject) &&
                    Denies(record.scope.permissions, request, decision, effectResource))
                    return "An explicit owner denial blocks this operation.";
        }
        if (!allowed)
            return "The current companion or task scope does not authorize this operation.";
        if (current.taskId.empty())
            break;
        const auto task = session.tasks.find(current.taskId);
        if (task == session.tasks.end())
            return "An ancestor task authority is no longer active.";
        if (!Matches(task->second.restriction, request, decision, effectResource))
            return "A task restriction blocks this operation.";
        if (task->second.parent.empty())
            break;
        current.taskId = task->second.parent;
    }
    return {};
}

} // namespace revia::policy
