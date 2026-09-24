#pragma once

#include "Policy/desktopAuthorization.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace revia::policy
{

// "Allow this one thing, once" instead of "raise every permission until it works".
//
// Without this, the only way past a refusal is a standing policy change: turn up
// maxUnconfirmedConsequence, or switch on owner_full_access, and leave it that way long
// after the one message that needed it. A permission raised to get through a moment
// tends to stay raised, which is how a careful system becomes a permissive one without
// anyone deciding to.
//
// Everything an approval is bound to is a parameter of the consequence, so an approval
// to send *this* message to *this* recipient cannot be spent on a different one.

// What an approval is for. Every field participates in matching: if any of them differs
// at the moment of use, the approval does not apply.
struct ApprovalScope
{
    std::string taskId;
    DesktopOperation operation = DesktopOperation::Observe;
    // The runtime-minted binding this was approved against. Ties the approval to a
    // specific observed control rather than to a description of one.
    std::string bindingId;
    DesktopEffects effects = 0u;
    // Recipient, amount, destination, resource -- whatever the consequence actually
    // depends on, rendered by the caller. Bounded and never the message body itself.
    std::string parameterFingerprint;
    std::string policyVersion;

    [[nodiscard]] bool Matches(const ApprovalScope& other) const;
};

struct ActionApproval
{
    std::string id;
    ApprovalScope scope;
    std::chrono::steady_clock::time_point createdAt{};
    std::chrono::steady_clock::time_point expiresAt{};
    bool consumed = false;
};

// Short. An approval is answered by a person who is looking at the screen right now, and
// the thing they looked at is what stops being true first. Long enough to click through
// a dialog and act; short enough that walking away closes the window.
inline constexpr std::chrono::milliseconds DefaultApprovalLifetime{45000};

// Runtime-owned, and deliberately not reachable from parsed model output.
//
// The trust boundary is the call site of Grant: only a path that has actually asked a
// person may call it. Nothing in ActionRequest carries an approval id, and the parser
// has no field that would populate one, so a plan cannot arrive carrying its own
// permission slip.
class ApprovalRegistry
{
public:
    // Called only by a trusted user-approval path.
    [[nodiscard]] std::string Grant(
        ApprovalScope scope,
        std::chrono::steady_clock::time_point now,
        std::chrono::milliseconds lifetime = DefaultApprovalLifetime);

    // Spends the approval if -- and only if -- it exists, is unspent, is unexpired, and
    // every bound parameter still matches. Success marks it consumed; a second attempt
    // finds nothing to spend.
    [[nodiscard]] bool Consume(
        const std::string& approvalId,
        const ApprovalScope& against,
        std::chrono::steady_clock::time_point now,
        std::string& outReason);

    // Cancellation, emergency stop and task completion all end anything outstanding for
    // that task: an approval given for work that is no longer happening is a loose end.
    void InvalidateTask(const std::string& taskId, const std::string& reason);
    void InvalidateAll(const std::string& reason);
    // Drops spent and expired entries so the registry does not grow for the life of the
    // process.
    void Prune(std::chrono::steady_clock::time_point now);

    [[nodiscard]] std::size_t OutstandingCount() const;

private:
    mutable std::mutex mutex;
    std::unordered_map<std::string, ActionApproval> approvals;
};

} // namespace revia::policy
