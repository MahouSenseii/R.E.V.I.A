#include "Computer/computerBenchmark.h"

#include "Computer/subgoalValidator.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace revia::computer
{

namespace
{

ObservedCandidate Control(
    const std::string& name,
    const std::string& role,
    const bool invokable,
    const bool editable)
{
    ObservedCandidate candidate;
    candidate.id = name;
    candidate.name = name;
    candidate.role = role;
    candidate.mayInvoke = invokable;
    candidate.maySetText = editable;
    candidate.mayType = editable;
    return candidate;
}

ComputerObservation Screen(
    const std::string& application,
    std::vector<ObservedCandidate> candidates,
    const std::size_t omitted = 0)
{
    ComputerObservation observation;
    observation.screen.succeeded = true;
    observation.screen.generation = 1;
    observation.screen.id = "benchmark";
    observation.screen.foregroundApplication = application;
    observation.screen.foregroundTitle = "Benchmark window";
    observation.screen.omittedControls = omitted;
    observation.candidates = std::move(candidates);
    return observation;
}

// A subgoal the same way the runtime makes one. Going through validation matters even
// here: a benchmark that fed policies inputs the runtime cannot produce would be
// measuring them on a problem they never face.
ComputerSubgoal Validated(
    const SubgoalIntent intent,
    const std::string& application,
    const std::string& name,
    const std::string& role,
    const PayloadVault& vault,
    const PayloadReference& payload = {})
{
    ComputerSubgoal proposed;
    proposed.id = NewSubgoalId();
    proposed.intent = intent;
    proposed.description = "benchmark subgoal";
    proposed.target.application = application;
    proposed.target.name = name;
    proposed.target.role = role;
    proposed.payload = payload;

    SubgoalContext context;
    context.goalId = "benchmark";
    context.origin = RequestOrigin::UserDirected;
    context.scope.approvedApplications = {application};
    context.scope.desktopControl.applicationLaunch = true;
    context.scope.desktopControl.keyboard = true;
    context.scope.desktopControl.pointer = true;
    context.scope.desktopControl.maxTypedCharacters = 512;
    context.actionsLeft = 10;
    context.retriesLeft = 3;

    const SubgoalValidation validation = ValidateSubgoal(proposed, context, vault);
    // An invalid case would silently become an "everything declines" row, which reads
    // like a cautious policy rather than a broken benchmark.
    return validation.accepted ? validation.subgoal : ComputerSubgoal{};
}

} // namespace

std::vector<BenchmarkCase> StandardBenchmarkCases(
    PayloadVault& vault, const std::string& application)
{
    const PayloadReference payload = vault.Store("benchmark content", "message");

    const std::vector<ObservedCandidate> form = {
        Control("Document", "edit", false, true),
        Control("Notes", "edit", false, true),
        Control("Save", "button", true, false),
        Control("Save as", "button", true, false),
        Control("Send", "button", true, false),
        Control("Delete", "button", true, false),
    };

    using actions::ActionType;
    std::vector<BenchmarkCase> cases;

    // -- unambiguous: the family the whole feature exists for --
    cases.push_back({"press save", "unambiguous",
        Validated(SubgoalIntent::InteractWithControl, application, "Save", "button", vault),
        Screen(application, form), ActionType::InvokeControl, "Save", false});
    cases.push_back({"press send", "unambiguous",
        Validated(SubgoalIntent::InteractWithControl, application, "Send", "button", vault),
        Screen(application, form), ActionType::InvokeControl, "Send", false});
    cases.push_back({"fill document", "unambiguous",
        Validated(SubgoalIntent::EnterPayload, application, "Document", "edit", vault,
            payload),
        Screen(application, form), ActionType::SetControlText, "Document", false});
    cases.push_back({"fill notes", "unambiguous",
        Validated(SubgoalIntent::EnterPayload, application, "Notes", "edit", vault,
            payload),
        Screen(application, form), ActionType::SetControlText, "Notes", false});

    // -- near miss: where a loose matcher presses the wrong thing --
    cases.push_back({"save, not save as", "near miss",
        Validated(SubgoalIntent::InteractWithControl, application, "Save", "button", vault),
        Screen(application, {form[2], form[3]}), ActionType::InvokeControl, "Save", false});
    cases.push_back({"save as, not save", "near miss",
        Validated(SubgoalIntent::InteractWithControl, application, "Save as", "button",
            vault),
        Screen(application, {form[2], form[3]}), ActionType::InvokeControl, "Save as",
        false});
    cases.push_back({"edit target must be editable", "near miss",
        Validated(SubgoalIntent::EnterPayload, application, "Document", "edit", vault,
            payload),
        Screen(application, {Control("Document", "button", true, false),
            Control("Document", "edit", false, true)}),
        ActionType::SetControlText, "Document", false});

    // -- preparation: correct, and neither acting on the target nor declining --
    //
    // The target lives in a window that is not in front. Bringing that window forward is
    // the right bounded step toward the same subgoal; acting into whatever happens to be
    // focused is the failure this case exists to catch, and refusing outright would
    // leave the task stuck with nothing wrong.
    cases.push_back({"wrong window in front", "preparation",
        Validated(SubgoalIntent::InteractWithControl, application, "Save", "button", vault),
        Screen("somethingelse.exe", form), ActionType::FocusWindow, application, false});

    // -- should decline: acting here is worse than paying for a model call --
    cases.push_back({"two controls match", "should decline",
        Validated(SubgoalIntent::InteractWithControl, application, "Send", "button", vault),
        Screen(application, {Control("Send", "button", true, false),
            Control("Send", "button", true, false)}),
        ActionType::Unknown, "", true});
    cases.push_back({"target is not there", "should decline",
        Validated(SubgoalIntent::InteractWithControl, application, "Publish", "button",
            vault),
        Screen(application, form), ActionType::Unknown, "", true});
    cases.push_back({"listing was truncated", "should decline",
        Validated(SubgoalIntent::InteractWithControl, application, "Publish", "button",
            vault),
        Screen(application, form, /*omitted=*/80), ActionType::Unknown, "", true});
    cases.push_back({"screen could not be read", "should decline",
        Validated(SubgoalIntent::InteractWithControl, application, "Save", "button", vault),
        ComputerObservation{}, ActionType::Unknown, "", true});

    return cases;
}

std::vector<BenchmarkResult> RunBenchmark(
    IComputerPolicy& policy,
    const std::vector<BenchmarkCase>& cases,
    const std::uint32_t repetitions,
    const std::function<void(const ComputerSubgoal&)>& installSubgoal)
{
    std::vector<BenchmarkResult> results;
    const auto find = [&](const std::string& family) -> BenchmarkResult&
    {
        const auto existing = std::find_if(results.begin(), results.end(),
            [&](const BenchmarkResult& result) { return result.family == family; });
        if (existing != results.end()) return *existing;
        BenchmarkResult fresh;
        fresh.provider = policy.Name();
        fresh.family = family;
        results.push_back(fresh);
        return results.back();
    };

    for (std::uint32_t repetition = 0; repetition < std::max(repetitions, 1u); ++repetition)
    {
        for (const BenchmarkCase& testCase : cases)
        {
            if (installSubgoal) installSubgoal(testCase.subgoal);

            ComputerTaskContext context;
            context.subgoal = testCase.name;
            context.observation = testCase.observation;
            context.actionsLeft = 10;
            context.retriesLeft = 3;
            context.scope = testCase.subgoal.scope;

            const auto started = std::chrono::steady_clock::now();
            const ComputerDecision decision = policy.Decide(context, {});
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();

            BenchmarkResult& result = find(testCase.family);
            ++result.cases;
            result.totalMicroseconds += static_cast<std::uint64_t>(elapsed);
            if (!decision.costReported || decision.tokens > 0) ++result.reachedModel;

            switch (decision.kind)
            {
                case ComputerDecisionKind::ProposeAction:
                {
                    // A focus names its application; everything else names a control.
                    const std::string named =
                        decision.step.action.type == actions::ActionType::FocusWindow
                            ? decision.step.action.application
                            : decision.step.action.control;
                    const bool rightAction =
                        testCase.expectedAction == actions::ActionType::Unknown ||
                        decision.step.action.type == testCase.expectedAction;
                    if (!testCase.expectedToDecline && rightAction &&
                        named == testCase.expectedTarget)
                    {
                        ++result.correct;
                    }
                    else
                    {
                        // Includes acting when the right answer was to decline, which
                        // is the failure that costs an unauthorized action rather than
                        // a model call.
                        ++result.wrong;
                    }
                    break;
                }

                case ComputerDecisionKind::NeedUser:
                    ++result.escalatedToUser;
                    if (testCase.expectedToDecline) ++result.declinedCorrectly;
                    else ++result.declinedUnnecessarily;
                    break;

                case ComputerDecisionKind::ProposeCompletion:
                    // Declining to act because the work is already done. Correct only
                    // where declining was correct.
                    if (testCase.expectedToDecline) ++result.declinedCorrectly;
                    else ++result.declinedUnnecessarily;
                    break;

                case ComputerDecisionKind::NeedReasoning:
                case ComputerDecisionKind::NeedVision:
                case ComputerDecisionKind::WaitForState:
                case ComputerDecisionKind::Reobserve:
                case ComputerDecisionKind::CannotHandle:
                default:
                    if (testCase.expectedToDecline) ++result.declinedCorrectly;
                    else ++result.declinedUnnecessarily;
                    break;
            }
        }
    }

    return results;
}

BenchmarkResult BenchmarkReport::Total(const std::string& provider) const
{
    BenchmarkResult total;
    total.provider = provider;
    total.family = "all";
    for (const BenchmarkResult& result : results)
    {
        if (result.provider != provider) continue;
        total.cases += result.cases;
        total.correct += result.correct;
        total.wrong += result.wrong;
        total.declinedCorrectly += result.declinedCorrectly;
        total.declinedUnnecessarily += result.declinedUnnecessarily;
        total.escalatedToUser += result.escalatedToUser;
        total.reachedModel += result.reachedModel;
        total.totalMicroseconds += result.totalMicroseconds;
    }
    return total;
}

std::vector<std::string> BenchmarkReport::Providers() const
{
    std::vector<std::string> providers;
    for (const BenchmarkResult& result : results)
    {
        if (std::find(providers.begin(), providers.end(), result.provider) ==
            providers.end())
        {
            providers.push_back(result.provider);
        }
    }
    return providers;
}

std::vector<std::string> BenchmarkReport::Families() const
{
    std::vector<std::string> families;
    for (const BenchmarkResult& result : results)
    {
        if (std::find(families.begin(), families.end(), result.family) == families.end())
        {
            families.push_back(result.family);
        }
    }
    return families;
}

std::string BenchmarkReport::Format() const
{
    std::ostringstream stream;
    stream << "\n===== Decision provider comparison (SYNTHETIC) =====\n";
    stream << "Repetitions per case: " << repetitions << "\n";
    stream << "\nThis is not a measurement against Main.\n"
              "\n"
              "The baseline here is a substring matcher standing in for a model-driven\n"
              "path. It reaches no backend, so what this measures is the SHAPE of the\n"
              "difference on fixed inputs -- which policy acts, which declines, and what\n"
              "each costs in microseconds -- and nothing about how a real model behaves.\n"
              "For that, run --computer-live, which drives legacy and assisted through\n"
              "the same task with an actual llama.cpp backend and counts every call.\n"
              "\n"
              "Every provider was asked the same cases, in the same order, from the same\n"
              "observations. \'wrong\' counts acting on the wrong control or acting where\n"
              "the right answer was to decline -- an abstention costs a model call, a\n"
              "wrong action costs something nobody authorized.\n";

    for (const std::string& provider : Providers())
    {
        stream << "\n-- " << provider << " --\n";
        stream << std::left
               << std::setw(18) << "family"
               << std::setw(7) << "cases"
               << std::setw(9) << "correct"
               << std::setw(7) << "wrong"
               << std::setw(10) << "declined"
               << std::setw(10) << "coverage"
               << std::setw(10) << "accuracy"
               << std::setw(10) << "us/case" << "\n";
        for (const BenchmarkResult& result : results)
        {
            if (result.provider != provider) continue;
            stream << std::left
                   << std::setw(18) << result.family
                   << std::setw(7) << result.cases
                   << std::setw(9) << result.correct
                   << std::setw(7) << result.wrong
                   << std::setw(10) << (result.declinedCorrectly +
                        result.declinedUnnecessarily)
                   << std::setw(10) << std::fixed << std::setprecision(2)
                   << result.Coverage()
                   << std::setw(10) << result.Accuracy()
                   << std::setw(10) << std::setprecision(1)
                   << result.MicrosecondsPerCase() << "\n";
        }
        const BenchmarkResult total = Total(provider);
        stream << std::left << std::setw(18) << "ALL"
               << std::setw(7) << total.cases
               << std::setw(9) << total.correct
               << std::setw(7) << total.wrong
               << std::setw(10) << (total.declinedCorrectly + total.declinedUnnecessarily)
               << std::setw(10) << std::fixed << std::setprecision(2) << total.Coverage()
               << std::setw(10) << total.Accuracy()
               << std::setw(10) << std::setprecision(1) << total.MicrosecondsPerCase()
               << "\n";
        stream << "reached a model: " << total.reachedModel << " of " << total.cases
               << "\n";
    }
    stream << "===== end of synthetic comparison =====\n";
    return stream.str();
}

} // namespace revia::computer
