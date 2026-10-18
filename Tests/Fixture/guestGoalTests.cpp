#include "Goals/goalStore.h"
#include "Policy/permissionStore.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace revia;
namespace
{
void Check(const bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() / actions::NewActionId();
    std::filesystem::create_directories(root);
    try
    {
        const auto config = root / "capabilities.json";
        {
            std::ofstream file(config);
            file << R"({"approvedRoots":[],"guest":{"enabled":true,"read":true,"write":true,"execute":true,"allowTaskExecution":true,"approvedExecutables":["/usr/bin/python3"],"maxReadBytes":128}})";
        }
        actions::CapabilitySettings settings;
        std::string error;
        Check(policy::PermissionStore{}.Load(config, settings, error) && settings.guest.enabled && settings.guest.read &&
            settings.guest.write && settings.guest.execute && settings.guest.allowTaskExecution &&
            settings.guest.limits.maxReadBytes == 128 && settings.guest.limits.approvedExecutables == std::vector<std::string>{"/usr/bin/python3"} &&
            !settings.process.enabled, "Guest permissions did not load independently from the existing capability owner.");
        goals::Goal goal;
        goal.id = goals::NewGoalId();
        goal.title = "Guest coding task";
        goal.scope = settings;
        goal.scope.guest.expectedIdentity = {"environment-a", "boot-a"};
        goals::GoalStep step;
        step.id = goals::NewStepId();
        step.action.type = actions::ActionType::VmWriteTextFile;
        step.action.guest.operation = workspace::GuestOperation::WriteText;
        step.action.guest.path = "src/Main.cpp";
        step.action.guest.content = "int main() {}";
        step.action.guest.expectedDigest = "missing";
        step.action.guest.expectedIdentity = goal.scope.guest.expectedIdentity;
        step.check.type = actions::ActionType::VmReadTextFile;
        step.check.guest.operation = workspace::GuestOperation::ReadText;
        step.check.guest.path = step.action.guest.path;
        step.check.guest.expectedIdentity = goal.scope.guest.expectedIdentity;
        step.expected = "Source file contains the requested program";
        goal.steps.push_back(step);
        goals::GoalStore store(actions::PathToUtf8(root / "goals.db"));
        Check(store.Save(goal), "Guest goal fixture could not save.");
        const auto loaded = store.Load(goal.id);
        Check(loaded && loaded->scope.guest.expectedIdentity.environmentId == "environment-a" &&
            loaded->scope.guest.expectedIdentity.bootId == "boot-a" && loaded->scope.guest.allowTaskExecution &&
            loaded->steps.size() == 1 && loaded->steps[0].action.type == actions::ActionType::VmWriteTextFile &&
            loaded->steps[0].action.guest.path == "src/Main.cpp" && loaded->steps[0].action.guest.content == "int main() {}" &&
            loaded->steps[0].action.guest.expectedDigest == "missing" && loaded->steps[0].check.guest.path == "src/Main.cpp",
            "A stored guest task lost its frozen identity, permissions, or typed action payload.");
        auto condition = goals::DerivePostcondition(step);
        Check(condition.kind == goals::PostconditionKind::GuestFileContentIs, "Guest writes lack exact independent readback verification.");
        actions::ActionResult receipt;
        receipt.succeeded = receipt.attempted = true;
        receipt.content = step.action.guest.content;
        receipt.guest.emplace();
        receipt.guest->succeeded = true;
        receipt.guest->receiptVerified = true;
        receipt.guest->identity = goal.scope.guest.expectedIdentity;
        Check(goals::JudgeStep(goals::CurrentVerificationSchema, condition, step.expected, receipt).outcome == goals::VerificationOutcome::Verified,
            "Exact guest readback failed to verify the written content.");
        receipt.content += "unexpected";
        Check(goals::EvaluatePostcondition(condition, receipt) == goals::VerificationOutcome::Failed,
            "Partial or expanded guest content verified as an exact write.");
        receipt.content = step.action.guest.content;
        receipt.guest->receiptVerified = false;
        Check(goals::EvaluatePostcondition(condition, receipt) == goals::VerificationOutcome::Unknown,
            "Unverified guest transport supplied accepted task evidence.");
        receipt.guest->receiptVerified = true;
        receipt.guest->identity.bootId = "boot-b";
        Check(goals::EvaluatePostcondition(condition, receipt) == goals::VerificationOutcome::Unknown,
            "A different guest boot supplied accepted task evidence.");
        {
            std::ofstream file(config);
            file << R"({"approvedRoots":[],"guest":{"enabled":true,"environmentId":"forged"}})";
        }
        Check(!policy::PermissionStore{}.Load(config, settings, error), "The capability file supplied a runtime guest identity.");
        std::filesystem::remove_all(root);
        std::cout << "Guest goal: permissions, frozen identity, payload persistence and exact readback passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::filesystem::remove_all(root);
        std::cerr << "Guest goal: " << error.what() << '\n';
        return 1;
    }
}
