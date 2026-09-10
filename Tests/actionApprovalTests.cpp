#include "testSupport.h"

#include "Policy/actionApproval.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace revia::policy;
using revia::tests::Check;

// Approvals, tested for what they refuse.
//
// The value of a single-use approval is entirely in the cases where it does not apply,
// so almost everything here is a negative case. The one positive case matters just as
// much: an approval that is too fragile to spend once would push the user straight back
// to raising a standing permission, which is the thing it exists to avoid.

ApprovalScope SendScope()
{
    ApprovalScope scope;
    scope.taskId = "task-1";
    scope.operation = DesktopOperation::PointerActivate;
    scope.bindingId = "binding-7";
    scope.effects = static_cast<DesktopEffects>(DesktopEffect::ExternalMessage);
    scope.parameterFingerprint = "to=alex|window=Compose|len=214";
    scope.policyVersion = "dc1:pk----:routine";
    return scope;
}

void TestOneApprovalSendsOneMessage()
{
    ApprovalRegistry registry;
    const auto now = std::chrono::steady_clock::now();
    const std::string id = registry.Grant(SendScope(), now);
    Check(!id.empty() && registry.OutstandingCount() == 1,
        "Granting an approval did not produce one outstanding approval.");

    std::string reason;
    Check(registry.Consume(id, SendScope(), now, reason),
        "A valid approval could not be spent: " + reason);
    Check(reason.empty(), "A successful use still reported a reason.");

    // The whole point. Sending one message does not authorize sending the next.
    Check(!registry.Consume(id, SendScope(), now, reason) &&
        reason.find("already been used") != std::string::npos,
        "The same approval sent a second message: " + reason);
    Check(registry.OutstandingCount() == 0,
        "A spent approval was still counted as outstanding.");
}

void TestAnApprovalDoesNotOutliveItsParameters()
{
    const auto now = std::chrono::steady_clock::now();
    std::string reason;

    // Each of these is the same operation with one thing changed, and each has to fail.
    // Together they are the definition of "narrow".
    struct Variation
    {
        const char* what;
        ApprovalScope scope;
    };
    std::vector<Variation> variations;
    {
        ApprovalScope recipient = SendScope();
        recipient.parameterFingerprint = "to=someone-else|window=Compose|len=214";
        variations.push_back({"a different recipient", recipient});

        ApprovalScope task = SendScope();
        task.taskId = "task-2";
        variations.push_back({"a different task", task});

        ApprovalScope binding = SendScope();
        binding.bindingId = "binding-9";
        variations.push_back({"a different control", binding});

        ApprovalScope effect = SendScope();
        effect.effects = static_cast<DesktopEffects>(DesktopEffect::Financial);
        variations.push_back({"a different effect", effect});

        ApprovalScope operation = SendScope();
        operation.operation = DesktopOperation::Invoke;
        variations.push_back({"a different route", operation});

        ApprovalScope policy = SendScope();
        policy.policyVersion = "dc1:pk---a:external_message";
        variations.push_back({"a changed policy", policy});
    }

    for (const Variation& variation : variations)
    {
        ApprovalRegistry registry;
        const std::string id = registry.Grant(SendScope(), now);
        Check(!registry.Consume(id, variation.scope, now, reason),
            std::string("An approval was spent on ") + variation.what + ".");
        Check(reason.find("not what was approved") != std::string::npos,
            std::string("The refusal for ") + variation.what + " was unclear: " + reason);
    }
}

void TestApprovalsExpireAndCannotBeForged()
{
    ApprovalRegistry registry;
    const auto now = std::chrono::steady_clock::now();
    std::string reason;

    const std::string id = registry.Grant(SendScope(), now);
    Check(!registry.Consume(id, SendScope(), now + std::chrono::seconds(60), reason) &&
        reason.find("expired") != std::string::npos,
        "An approval outlived its lifetime: " + reason);

    // An id that was never granted. This is the shape a model-authored approval would
    // take, and it has to find nothing.
    Check(!registry.Consume("approval-i-made-this-up", SendScope(), now, reason),
        "A forged approval id was accepted.");
    Check(!registry.Consume("", SendScope(), now, reason),
        "An empty approval id was accepted.");

    // A forged id and a revoked one report the same thing on purpose: a caller that
    // could tell them apart could map the registry.
    ApprovalRegistry second;
    const std::string revoked = second.Grant(SendScope(), now);
    second.InvalidateAll("emergency stop");
    std::string revokedReason;
    static_cast<void>(second.Consume(revoked, SendScope(), now, revokedReason));
    std::string forgedReason;
    static_cast<void>(second.Consume("approval-nope", SendScope(), now, forgedReason));
    Check(!revokedReason.empty() && !forgedReason.empty(),
        "A refusal came back without a reason.");
}

void TestCancellationAndStopEndOutstandingApprovals()
{
    const auto now = std::chrono::steady_clock::now();
    std::string reason;

    ApprovalRegistry byTask;
    const std::string mine = byTask.Grant(SendScope(), now);
    ApprovalScope otherTask = SendScope();
    otherTask.taskId = "task-2";
    const std::string theirs = byTask.Grant(otherTask, now);

    byTask.InvalidateTask("task-1", "the task was cancelled");
    Check(!byTask.Consume(mine, SendScope(), now, reason),
        "Cancelling a task left its approval spendable.");
    // A cancelled task must not take another task's approval with it.
    Check(byTask.Consume(theirs, otherTask, now, reason),
        "Cancelling one task invalidated an unrelated approval: " + reason);

    ApprovalRegistry byStop;
    const std::string pending = byStop.Grant(SendScope(), now);
    byStop.InvalidateAll("emergency stop");
    Check(!byStop.Consume(pending, SendScope(), now, reason),
        "An emergency stop left an approval spendable.");
    Check(byStop.OutstandingCount() == 0,
        "An emergency stop left approvals outstanding.");
}

void TestTheRegistryDoesNotGrowForever()
{
    ApprovalRegistry registry;
    const auto now = std::chrono::steady_clock::now();
    for (int index = 0; index < 20; ++index)
    {
        ApprovalScope scope = SendScope();
        scope.taskId = "task-" + std::to_string(index);
        static_cast<void>(registry.Grant(scope, now));
    }
    Check(registry.OutstandingCount() == 20, "Not every approval was recorded.");

    registry.Prune(now + std::chrono::seconds(60));
    Check(registry.OutstandingCount() == 0,
        "Pruning left expired approvals outstanding.");
}

} // namespace

void RunActionApprovalTests()
{
    TestOneApprovalSendsOneMessage();
    TestAnApprovalDoesNotOutliveItsParameters();
    TestApprovalsExpireAndCannotBeForged();
    TestCancellationAndStopEndOutstandingApprovals();
    TestTheRegistryDoesNotGrowForever();
    std::cout << "Action approval tests passed: one thing, once.\n";
}
