#include "testSupport.h"

#include "Computer/taskAcceptance.h"
#include "Computer/taskProgression.h"

void RunTaskAcceptanceTests()
{
    using namespace revia;
    computer::TaskContent task;
    task.requirement = computer::ContentRequirement::ExactUserContent;
    task.value = "user-owned exact value";
    task.destination = "Compose";
    computer::ComputerObservation observation;
    observation.screen.succeeded = true;
    observation.screen.foregroundApplication = "fixture.exe";
    computer::ObservedCandidate candidate;
    candidate.id = "compose-edit";
    candidate.name = "Compose";
    candidate.role = "edit";
    candidate.maySetText = true;
    observation.candidates.push_back(candidate);
    goals::Goal goal;
    goals::GoalStep step;
    step.action.type = actions::ActionType::SetControlText;
    step.action.application = "fixture.exe";
    step.action.control = candidate.id;
    step.action.value = task.value;
    step.status = goals::StepStatus::Succeeded;
    goals::StepAttempt attempt;
    attempt.executed = true;
    attempt.verified = true;
    attempt.outcome = goals::VerificationOutcome::Verified;
    attempt.checkedBy = goals::PostconditionKind::ControlValueIs;
    step.attempts.push_back(attempt);
    goal.steps.push_back(step);
    auto acceptance = computer::DeriveTaskAcceptance("put \"user-owned exact value\" into Compose");
    const auto check = [&](bool readback) { return computer::VerifyExactContentPlacement(task, goal, observation, readback, acceptance); };
    tests::Check(check(true).accepted, "Fresh exact content in the original unique field did not satisfy placement.");
    tests::Check(!check(false).accepted, "Historical verified steps bypassed a failed fresh readback.");
    goal.steps[0].action.control = "different-field";
    tests::Check(!check(true).accepted, "Content placed in the wrong field satisfied the original request.");
    goal.steps[0] = step;
    observation.screen.foregroundApplication = "different.exe";
    tests::Check(!check(true).accepted, "An identically named field in another application satisfied placement.");
    observation.screen.foregroundApplication = "FIXTURE.EXE";
    tests::Check(check(true).accepted, "Application identity comparison changed its case-insensitive contract.");
    observation.candidates.push_back(candidate);
    tests::Check(!check(true).accepted, "An ambiguous original destination satisfied placement.");
    observation.candidates.pop_back();
    observation.withheld = true;
    tests::Check(!check(true).accepted, "A withheld observation satisfied placement.");
    observation.withheld = false;
    goal.steps[0].attempts[0].checkedBy = goals::PostconditionKind::TextObserved;
    tests::Check(!check(true).accepted, "A descriptive substring check became exact placement evidence.");
    goal.steps[0] = step;
    task.submissionRequested = true;
    tests::Check(!check(true).accepted, "Draft placement claimed to prove external submission.");
    task.submissionRequested = false;
    task.requirement = computer::ContentRequirement::Drafted;
    tests::Check(!check(true).accepted, "Draft placement claimed to prove the requested composition quality.");
    task.requirement = computer::ContentRequirement::None;
    tests::Check(!check(true).accepted && !check(true).detail.empty(),
        "An unsupported whole-task criterion received success or no useful explanation.");

    task.requirement = computer::ContentRequirement::ExactUserContent;
    tests::Check(!computer::VerifyExactContentPlacement(task, goal, observation, true).accepted,
        "Missing whole-request contract inherited exact placement success.");
    tests::Check(!computer::VerifyExactContentPlacement(task, goal, observation, true,
        computer::DeriveTaskAcceptance("put \"user-owned exact value\" into Compose in the Other window")).accepted,
        "ACT-1: placement in the current app certified a different requested application context.");
    const std::string browserRequest = "put \"user-owned exact value\" into Compose at https://example.com/form";
    const auto browserContract = computer::DeriveTaskAcceptance(browserRequest);
    tests::Check(!computer::VerifyExactContentPlacement(task, goal, observation, true, browserContract).accepted,
        "ACT-1: native placement certified an original URL request without browser or origin evidence.");
    const auto browserTask = computer::ExtractTaskContent(browserRequest);
    computer::ComputerTaskContext nativeContext;
    nativeContext.observation = observation;
    computer::TaskProgressInputs browserInputs;
    browserInputs.content = &browserTask;
    browserInputs.acceptance = &browserContract;
    browserInputs.context = &nativeContext;
    browserInputs.application = "fixture.exe";
    browserInputs.contentPlaced = true;
    tests::Check(computer::DeriveTaskProgress(browserInputs).phase != computer::TaskPhase::Complete,
        "ACT-1: native progression completed an original URL request without browser evidence.");
    const auto matchingApp = computer::DeriveTaskAcceptance("put \"user-owned exact value\" into Compose in the FIXTURE window");
    tests::Check(computer::VerifyExactContentPlacement(task, goal, observation, true, matchingApp).accepted,
        "A bounded literal executable name could not prove the explicitly named application context.");
    const auto quotedApp = computer::DeriveTaskAcceptance("put \"hello\" into Compose in \"Notes and then drafts.exe\" window");
    tests::Check(quotedApp.placementOnly && quotedApp.MatchesApplication("NOTES AND THEN DRAFTS.EXE") &&
            !quotedApp.MatchesApplication("other.exe"), "Quoted application data or exact executable matching was lost.");
    for (const std::string& request : {std::string("put \"bread and butter then tea\" into Compose, please."),
             std::string("Could you please type \"and then\" into \"Terms and conditions\"? Thank you."),
             std::string("put \"hello\" in the Compose box. Do not send anything."),
             std::string("put \"hello\" in Compose in \"Notes and then drafts\" window")})
    {
        tests::Check(computer::DeriveTaskAcceptance(request).placementOnly,
            "Quoted data or a harmless courtesy/negative-send suffix became extra work: " + request);
    }
    for (const std::string& request : {std::string("put \"hello\" into Compose then open the project folder"),
             std::string("open the project folder and put \"hello\" into Compose"),
             std::string("put \"hello\" into Compose also open the project folder"),
             std::string("put \"hello\" into Compose; open the project folder"),
             std::string("put \"hello\" into Compose. Open the project folder"),
             std::string("put \"hello\" into Compose save changes"),
             std::string("put \"hello\" into Compose using voice"),
             std::string("put \"hello\" into Compose in \"\" window"),
             std::string("put \"hello\" into Compose using the app Notepad to save output"),
             std::string("put \"hello\" into Compose in the Notepad window to check the project"),
             std::string("put \"hello\" into Compose in \"Notes and then drafts\" window save output"),
             std::string("put \"hello\" into Compose in reverse order"),
             std::string("put \"hello\" into Compose, please check it"),
             std::string("put \"hello\" into Compose and send it"),
             std::string("put \"hello\" into Compose do not send it and open the project folder"),
             std::string("put \"hello into Compose")})
        tests::Check(!computer::DeriveTaskAcceptance(request).placementOnly,
            "Unknown or additional requested work received placement-only authority: " + request);

    for (const std::string& request : {std::string("put \"user-owned exact value\" into Compose and open the project folder"),
             std::string("put \"user-owned exact value\" into Compose then open the project folder"),
             std::string("put \"user-owned exact value\" into Compose; open the project folder")})
    {
        task = computer::ExtractTaskContent(request);
        acceptance = computer::DeriveTaskAcceptance(request);
        tests::Check(!check(true).accepted,
            "ACT-1: exact native placement falsely certified unrelated remaining project-folder work: " + request);
        computer::ComputerTaskContext context;
        context.observation = observation;
        computer::TaskProgressInputs inputs;
        inputs.content = &task;
        inputs.acceptance = &acceptance;
        inputs.contentPlaced = true;
        inputs.context = &context;
        inputs.application = "fixture.exe";
        tests::Check(computer::DeriveTaskProgress(inputs).phase != computer::TaskPhase::Complete,
            "ACT-1: deterministic progression completed unrelated remaining project-folder work.");
    }

    task.requirement = computer::ContentRequirement::ExactUserContent;
    task.redacted = "Enter '<the prepared content>' into Compose at https://example.com/form";
    browser::BrowserReceipt receipt;
    receipt.session = "owned-session";
    receipt.generation = 4;
    receipt.url = "https://example.com/form";
    receipt.elements.push_back({"node-1", "Compose", "textarea", false, true, task.value, true});
    auto& browserStep = goal.steps[0];
    browserStep.action.type = actions::ActionType::BrowserFill;
    browserStep.action.browser = {receipt.url, receipt.session, 2, "node-1", task.value};
    browserStep.check.type = actions::ActionType::BrowserObserve;
    browserStep.check.browser.url = receipt.url;
    browserStep.attempts[0].checkedBy = goals::PostconditionKind::BrowserControlValueIs;
    const auto browserAcceptance = computer::DeriveTaskAcceptance(
        "put \"user-owned exact value\" into Compose at https://example.com/form");
    const auto browserCheck = [&] { return computer::VerifyExactBrowserPlacement(task, goal, receipt, browserAcceptance); };
    tests::Check(browserCheck().accepted, "Exact fresh browser placement did not satisfy original origin and named field.");
    receipt.elements[0].value += "extra";
    tests::Check(!browserCheck().accepted, "An altered browser field value satisfied exact placement.");
    receipt.elements[0].value = task.value;
    receipt.elements[0].valueAvailable = false;
    tests::Check(!browserCheck().accepted, "An unavailable or truncated browser value satisfied exact placement.");
    receipt.elements[0].valueAvailable = true;
    receipt.url = "https://different.example/form";
    tests::Check(!browserCheck().accepted, "An identical field on another origin satisfied the original request.");
    receipt.url = browserStep.action.browser.url;
    receipt.elements.push_back(receipt.elements[0]);
    tests::Check(!browserCheck().accepted, "Duplicate browser field names satisfied the original request.");
    receipt.elements.pop_back();
    receipt.generation = 2;
    tests::Check(!browserCheck().accepted, "A pre-effect browser observation satisfied completion.");
    receipt.generation = 4;
    receipt.session = "replacement-session";
    tests::Check(!browserCheck().accepted, "A replacement browser session inherited prior placement evidence.");
    receipt.session = browserStep.action.browser.session;
    task.redacted = "Enter '<the prepared content>' into Compose";
    tests::Check(!browserCheck().accepted, "An unspecified original browser origin was inferred from model actions.");
    task.redacted += " at https://example.com/form";
    task.submissionRequested = true;
    tests::Check(!browserCheck().accepted, "A browser draft readback claimed external submission.");

    const auto condition = goals::DerivePostcondition(browserStep);
    tests::Check(
        condition.kind == goals::PostconditionKind::BrowserControlValueIs, "A browser fill derived only a weak page substring check.");
    actions::ActionResult result;
    result.succeeded = true;
    result.browser = receipt;
    tests::Check(goals::EvaluatePostcondition(condition, result) == goals::VerificationOutcome::Verified,
        "A typed browser field check rejected exact fresh readback.");
    result.browser->session = "other";
    tests::Check(goals::EvaluatePostcondition(condition, result) == goals::VerificationOutcome::Unknown,
        "Typed browser verification accepted a different session.");
}
