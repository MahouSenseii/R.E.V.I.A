#include "testSupport.h"

#include "Actions/actionRuntime.h"
#include "Browser/browserSession.h"

#include <cstdlib>
#include <future>
#include <nlohmann/json.hpp>

void RunBrowserExecutionTests()
{
    using namespace revia;
    const char* supplied = std::getenv("REVIA_BROWSER_FIXTURE_URL");
    tests::Check(supplied != nullptr, "Run the native browser fixture through its local HTTP fixture harness.");
    const std::string url = supplied;
    tests::Check(url.starts_with("http://127.0.0.1:"), "The native browser fixture requires its explicitly admitted loopback origin.");
    tests::ScopedTestDirectory directory;
    auto session = std::make_shared<browser::BrowserSession>(
        std::filesystem::absolute("Tools/Browser/interactiveHost.mjs"), directory.root / "profiles");
    actions::ActionRuntime runtime;
    runtime.SetBrowserSession(session);
    const auto origin = url.substr(0, url.find('/', 7));
    const nlohmann::json config = {{"mode", "supervised"}, {"approvedRoots", {actions::PathToUtf8(directory.root)}},
        {"createMissingApprovedRoots", false},
        {"browser", {{"enabled", true}, {"navigate", true}, {"interact", true}, {"allowLoopback", true}, {"approvedOrigins", {origin}},
                        {"timeoutMs", 10000}, {"maxTextBytes", 4096}, {"maxElements", 40}, {"maxValueBytes", 4096}}}};
    {
        std::ofstream file(directory.root / "capabilities.json");
        file << config.dump();
    }
    std::string error;
    tests::Check(runtime.Initialize(directory.root / "capabilities.json", directory.root / "audit.jsonl", error), error);
    actions::ActionRequest request;
    request.id = actions::NewActionId();
    request.type = actions::ActionType::BrowserNavigate;
    request.browser.url = url;
    const auto navigated = runtime.Execute(request, true);
    tests::Check(navigated.Succeeded() && navigated.result.browser, "Native browser navigation failed: " + navigated.Message());
    auto receipt = *navigated.result.browser;
    const auto field = std::find_if(receipt.elements.begin(), receipt.elements.end(), [](const auto& value) { return value.editable; });
    tests::Check(field != receipt.elements.end(), "The native browser did not observe its input.");
    request.type = actions::ActionType::BrowserFill;
    request.browser.session = receipt.session;
    request.browser.generation = receipt.generation;
    request.browser.element = field->id;
    request.browser.value = "Ada";
    const auto filled = runtime.Execute(request, true);
    tests::Check(filled.Succeeded(), "Native fill failed: " + filled.Message());
    const auto stale = runtime.Execute(request, true);
    tests::Check(!stale.Succeeded() && !stale.result.attempted, "A stale browser target was dispatched twice.");
    receipt = *filled.result.browser;
    const auto button =
        std::find_if(receipt.elements.begin(), receipt.elements.end(), [](const auto& value) { return value.name == "Save"; });
    tests::Check(button != receipt.elements.end(), "The Save button was not observed.");
    request.type = actions::ActionType::BrowserClick;
    request.browser.element = button->id;
    request.browser.generation = receipt.generation;
    request.browser.value.clear();
    const auto clicked = runtime.Execute(request, true);
    tests::Check(clicked.Succeeded() && clicked.result.content.find("Saved Ada with secret fixture state") != std::string::npos,
        "The native browser did not verify the hidden-state form outcome: " + clicked.Message());
    auto narrowed = runtime.Settings();
    narrowed.browser.interact = false;
    tests::Check(runtime.ExecuteScoped(request, policy::CapabilityPolicy(narrowed), true).policy.verdict == actions::PolicyVerdict::Blocked,
        "A scoped browser request recovered broader interaction permission.");
    request.type = actions::ActionType::BrowserObserve;
    request.beforeEffect = [](const std::string&) { return std::string("The captured browser grant expired."); };
    tests::Check(!runtime.Execute(request, true).Succeeded(), "A revoked browser admission returned a usable observation.");
    session->Stop();
    tests::Check(!session->Observation(), "Stopped browser receipts remained available for planning.");
}
