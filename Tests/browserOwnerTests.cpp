#include "testSupport.h"
#include "Browser/browserSession.h"

#include <algorithm>
#include <cstdlib>

void RunBrowserOwnerTests()
{
    using namespace revia;
    const char* fixture = std::getenv("REVIA_BROWSER_FIXTURE_URL");
    tests::Check(fixture != nullptr, "The live browser owner needs its disposable HTTP fixture.");
    const std::string url = fixture;
    tests::ScopedTestDirectory directory;
    browser::BrowserSession session(std::filesystem::absolute("Tools/Browser/interactiveHost.mjs"), directory.root / "profiles");
    browser::BrowserSettings settings;
    settings.enabled = settings.navigate = settings.interact = settings.allowLoopback = true;
    settings.approvedOrigins = {url.substr(0, url.find('/', 7))};
    actions::ActionRequest request;
    request.authorityStamp.taskId = "first-task";
    request.authorityStamp.policyVersion = 1;
    request.type = actions::ActionType::BrowserNavigate;
    request.browser.url = url;
    request.beforeEffect = [](const std::string&) { return std::string(); };
    auto result = session.Execute(request, settings);
    tests::Check(result.succeeded && result.browser, "Owned native browser navigation failed: " + result.message);
    const auto field =
        std::find_if(result.browser->elements.begin(), result.browser->elements.end(), [](const auto& value) { return value.editable; });
    tests::Check(field != result.browser->elements.end(), "The fixture field was not observed.");
    request.type = actions::ActionType::BrowserFill;
    request.browser.session = result.browser->session;
    request.browser.generation = result.browser->generation;
    request.browser.element = field->id;
    request.browser.value = "Ada";
    result = session.Execute(request, settings);
    tests::Check(result.succeeded, "Owned native browser fill failed: " + result.message);
    const auto button = std::find_if(
        result.browser->elements.begin(), result.browser->elements.end(), [](const auto& value) { return value.name == "Save"; });
    tests::Check(button != result.browser->elements.end(), "The fixture Save button was not observed.");
    request.type = actions::ActionType::BrowserClick;
    request.browser.generation = result.browser->generation;
    request.browser.element = button->id;
    request.browser.value.clear();
    result = session.Execute(request, settings);
    tests::Check(result.succeeded && result.content.find("Saved Ada with secret fixture state") != std::string::npos,
        "The native owner failed its actual hidden-state form: " + result.message);
    request.type = actions::ActionType::BrowserObserve;
    request.authorityStamp.attemptId = "new-attempt";
    tests::Check(session.Execute(request, settings).succeeded, "A new attempt within the same admitted task lost its browser session.");
    request.authorityStamp.taskId = "different-task";
    tests::Check(!session.Execute(request, settings).succeeded, "Another task inherited the previous task's live browser session.");
    request.authorityStamp.taskId = "first-task";
    request.authorityStamp.policyVersion = 2;
    tests::Check(!session.Execute(request, settings).succeeded, "A new policy revision inherited an old browser admission.");
    session.Stop();
    tests::Check(!session.Observation(), "Stopping the browser retained a usable receipt.");
    request.type = actions::ActionType::BrowserNavigate;
    int admissions = 0;
    request.beforeEffect = [&](const std::string&) { return ++admissions >= 8 ? "Captured grant expired." : ""; };
    result = session.Execute(request, settings);
    tests::Check(!result.succeeded && result.browser && result.browser->uncertainEffect && !session.Observation(),
        "Mid-operation revocation did not terminate the owned browser and retain uncertainty.");
}
