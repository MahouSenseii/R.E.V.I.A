#include "testSupport.h"

#include "Browser/browserTypes.h"
#include "Planning/structuredActionParser.h"
#include "Policy/capabilityPolicy.h"

void RunBrowserContractTests()
{
    using namespace revia;
    const planning::StructuredActionParser parser;
    const auto parsed = parser.ParseJson(R"({"action":"browser_navigate","url":"https://example.com/form"})");
    tests::Check(parsed.succeeded, "Interactive browser actions have no typed parser.");
    actions::CapabilitySettings settings;
    settings.mode = actions::ExecutionMode::OwnerFullAccess;
    tests::Check(policy::CapabilityPolicy(settings).Evaluate(parsed.request).verdict == actions::PolicyVerdict::Blocked,
        "Research permissions implicitly admitted interactive browser navigation.");
    browser::BrowserSettings grant;
    grant.enabled = true;
    grant.navigate = true;
    grant.interact = true;
    grant.approvedOrigins = {"https://example.com", "http://127.0.0.1:8080"};
    std::string error;
    tests::Check(!browser::ValidateSettings(grant, error), "Loopback origin silently enabled private network access.");
    grant.allowLoopback = true;
    tests::Check(browser::ValidateSettings(grant, error), error);
    tests::Check(browser::IsApprovedUrl("https://example.com/form", grant), "The exact granted origin was refused.");
    for (const auto* url : {"https://example.com.evil.test/", "http://example.com", "file:///secret", "https://owner:pw@example.com"})
        tests::Check(!browser::IsApprovedUrl(url, grant), "An unapproved URL passed origin admission.");
    auto smaller = grant;
    smaller.approvedOrigins = {"https://example.com"};
    smaller.interact = false;
    smaller.maxTextBytes = 512;
    const auto intersection = browser::IntersectSettings(grant, smaller);
    tests::Check(!intersection.interact && intersection.maxTextBytes == 512 && intersection.approvedOrigins.size() == 1,
        "Browser scope intersection widened permissions.");
    tests::Check(
        !parser
            .ParseJson(
                R"({"action":"browser_click","url":"https://example.com","session":"x","generation":1,"element":"a","script":"evil"})")
            .succeeded,
        "A raw script reached the typed browser surface.");
}
