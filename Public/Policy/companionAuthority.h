#pragma once

#include "Actions/actionTypes.h"
#include "Runtime/runtimeStamp.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace revia::policy
{

struct AuthorityPermissions
{
    std::vector<actions::ActionType> operations;
    std::vector<std::filesystem::path> roots;
    std::vector<std::string> applications;
    std::vector<std::string> internetHosts;
    bool withinMachineCeiling = false;

    // An adapter for an existing companion, never an owner grant.
    [[nodiscard]] static AuthorityPermissions WithinMachineCeiling();
};

struct AuthorityScope
{
    runtime::RuntimeStamp subject;
    AuthorityPermissions permissions;
};

struct AuthorityGrantRequest
{
    AuthorityScope scope;
    std::chrono::seconds lifetime{0};
};

class CompanionAuthority
{
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    // Installed by trusted runtime construction. Request text and UI clicks are not proof.
    using OwnerVerifier = std::function<bool(const AuthorityGrantRequest&)>;
    explicit CompanionAuthority(OwnerVerifier verifier = {}, Clock clock = {});

    void SetCompanionDefaults(const std::string& companionId, AuthorityPermissions permissions);
    [[nodiscard]] bool RegisterSession(const runtime::RuntimeStamp& stamp);
    void EndSession(const runtime::RuntimeStamp& stamp);
    [[nodiscard]] bool RegisterTask(const runtime::RuntimeStamp& stamp, const std::string& parentTaskId,
        AuthorityPermissions restriction = AuthorityPermissions::WithinMachineCeiling());
    void EndTask(const runtime::RuntimeStamp& stamp);
    [[nodiscard]] bool GrantAuthenticated(const AuthorityGrantRequest& request, std::string& outId, std::string& outError);
    // A companion-only subject denies across sessions; grants always require a live session.
    [[nodiscard]] std::string Deny(const AuthorityScope& scope);
    [[nodiscard]] bool Revoke(const std::string& id);
    void SetEmergencyStopped(const std::string& companionId, bool stopped);
    [[nodiscard]] std::uint64_t Revision() const;
    [[nodiscard]] std::string Evaluate(const runtime::RuntimeStamp& stamp, const actions::ActionRequest& request,
        const actions::PolicyDecision& canonicalDecision, const std::string& effectResource = {}) const;

  private:
    struct Task
    {
        std::string parent;
        AuthorityPermissions restriction;
    };
    struct Session
    {
        runtime::RuntimeStamp stamp;
        std::map<std::string, Task> tasks;
    };
    struct Record
    {
        AuthorityScope scope;
        bool denial = false;
        std::chrono::steady_clock::time_point expires;
    };
    [[nodiscard]] bool SubjectExists(const runtime::RuntimeStamp& stamp) const;
    [[nodiscard]] static std::string SessionKey(const runtime::RuntimeStamp& stamp);

    mutable std::mutex mutex;
    OwnerVerifier verifier;
    Clock clock;
    std::uint64_t revision = 1;
    std::map<std::string, AuthorityPermissions> defaults;
    std::map<std::string, Session> sessions;
    std::map<std::string, Record> records;
    std::map<std::string, bool> emergencyStops;
};

} // namespace revia::policy
