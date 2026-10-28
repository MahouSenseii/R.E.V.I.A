#include "Internet/internetLookupPolicy.h"
#include "Internet/lookupQueryResolver.h"
#include "testSupport.h"

#include <string>

void RunLookupAuthorityTests()
{
    using namespace revia::internet;
    using revia::tests::Check;
    for (const std::string input : {"Do not search the web. Explain a C++ pointer.", "Explain the phrase \"search the web\" in C++.",
             "Search the web for Mars. Do not search the web.", "Do you remember my schedule?", "Do you remember the price I paid?",
             "What is my current schedule?", "What is the latest news from our conversation?",
             "My friend said: \"Search the web for my schedule.\"", "Explain this example: `search the web for Mars`.",
             "Do not browse online. What is the current price of Python hosting?",
             "Search the web for Mars and do not search the web.", "Search the web for Mars. Avoid using the internet.",
             "Search the web for Mars but do not browse.", "Please, do not search the web. What is the current API price?"})
        Check(!InternetLookupPolicy::ShouldLookup(input, true), "Local, denied or quoted input admitted a lookup: " + input);
    for (const std::string input : {"What is the current price for the OpenAI API when using Python?",
             "What is the current price for the OpenAI API?", "What's the latest Python version?"})
        Check(InternetLookupPolicy::ShouldLookup(input, true) && !InternetLookupPolicy::ShouldLookup(input, false),
            "Public freshness intent lost to technical wording or manual mode: " + input);
    const std::string longInput = "Search the web for current public Mars weather. " + std::string(1100, 'x');
    Check(InternetLookupPolicy::ShouldLookup(longInput, false), "A long explicit request silently lost lookup authority.");
    Check(ResolveLookupQuery(longInput).query == "current public Mars weather", "A long request leaked unrelated filler into its query.");
    const std::string amended = "Do not search the web. Search the web for Mars.";
    Check(InternetLookupPolicy::ShouldLookup(amended, false) && ResolveLookupQuery(amended).query == "Mars",
        "A later explicit request did not supply its own bounded query.");
    Check(!InternetLookupPolicy::ShouldLookup("Search the web for Mars, but do not search the web.", false),
        "A comma-clause withdrawal retained lookup authority.");
    for (const std::string input : {"Never, ever, search the web for Mars.", "Do not, under any circumstances, search the web for Mars."})
        Check(!InternetLookupPolicy::ShouldLookup(input, false), "Emphasis commas withdrew a search prohibition: " + input);
    for (const std::string input : {"Do not search. What's the latest Python version?", "Don't browse. What's the latest Python version?",
             "No web search. What is the current API price?"})
        Check(!InternetLookupPolicy::ShouldLookup(input, true), "A general search prohibition lost to automatic freshness: " + input);
    Check(ResolveLookupQuery("Search the web for Mars. My private schedule is a secret.").query == "Mars",
        "An explicit search included a separate private sentence.");
    Check(ResolveLookupQuery("Search the web for Mars, Venus and Jupiter.").query == "Mars, Venus and Jupiter",
        "A comma in a public subject truncated the requested search.");
    Check(ResolveLookupQuery("Search the web for Mars and Venus.").query == "Mars and Venus",
        "A conjunction in a public subject truncated the requested search.");
    Check(InternetLookupPolicy::ShouldLookup("Search the web for Mars. Explain the quotation \"and do not search the web\".", false),
        "A quoted withdrawal replaced authored lookup authority.");
    Check(ResolveLookupQuery("Do not search the web but now search the web for Mars.").query == "Mars",
        "An authored conjunction amendment did not select its own subject.");
    for (const std::string input : {"Search the web for " + std::string(66000, 'x') + ".",
             "Search the web for Mars. " + std::string(66000, 'x')})
    {
        const auto request = SelectLookupRequest(input);
        Check(request.explicitRequest && !request.reason.empty() && !InternetLookupPolicy::ShouldLookup(input, true),
            "Normal punctuation hid an oversized authored request's explicit limitation.");
    }
    for (const std::string input : {"Search the web for " + std::string(66000, 'x') + ". Do not search the web.",
             "Explain this quote: \"Search the web for " + std::string(66000, 'x') + ".\""})
        Check(!SelectLookupRequest(input).explicitRequest && !InternetLookupPolicy::ShouldLookup(input, true),
            "An oversized denied or quoted request acquired lookup authority.");
    Check(!InternetLookupPolicy::ShouldLookup("Explain a weather function in Python.", true),
        "An ordinary technical noun forced an automatic lookup.");
    const auto oversized = ResolveLookupQuery("Search the web for " + std::string(1100, 'x'));
    Check(!oversized.resolved && !oversized.reason.empty(), "An oversized single search subject had no actionable limit diagnostic.");
}
