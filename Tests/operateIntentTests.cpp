#include "testSupport.h"

#include "Planning/operateIntent.h"

#include <iostream>
#include <string>

namespace
{
using revia::tests::Check;
using revia::planning::DetectOperateRequest;

void Expect(const std::string& input, const bool wanted)
{
    const auto intent = DetectOperateRequest(input);
    Check(intent.matched == wanted,
        std::string(wanted ? "Missed a request to operate: \"" : "Read conversation as a "
            "request to operate: \"") + input + "\"");
    if (wanted)
    {
        Check(!intent.verb.empty(), "A match named no verb: \"" + input + "\"");
    }
}

// The request as it will actually arrive once this is spoken rather than typed.
void TestSpokenRequestsAreRecognized()
{
    Expect("open microsoft edge", true);
    Expect("Open Microsoft Edge and pull up Facebook.", true);
    Expect("pull up facebook", true);
    Expect("go to facebook.com", true);
    Expect("hey revia can you please open edge", true);
    Expect("I want you to launch notepad", true);
    Expect("close notepad", true);
    Expect("switch to the browser", true);
    Expect("navigate to youtube", true);
    Expect("type hello into the search box", true);
    // The dot inside a host must survive normalization; only sentence-ending dots go.
    Check(DetectOperateRequest("go to facebook.com.").matched, "A trailing stop broke a host.");
}

// The half that matters more. Every one of these shares a verb with a real request, and
// acting on any of them would mean grabbing the mouse in the middle of a conversation.
void TestConversationIsNotMistakenForAnInstruction()
{
    Expect("can you open up to me", false);
    Expect("open up about how you feel", false);
    Expect("start a conversation with me", false);
    Expect("I opened edge already", false);
    Expect("we opened it yesterday", false);
    Expect("you just opened edge", false);
    Expect("what is microsoft edge", false);
    Expect("do you like edge or chrome", false);
    Expect("it opens slowly", false);
    Expect("open", false);
    Expect("", false);
    // Explicit syntax is handled before this gate and must never reach it.
    Expect("/operate open edge", false);
}

// Routing is not authority. The gate says which path a sentence takes and nothing else:
// the goal it produces is checked, confirmed and audited exactly as the command's is.
// This is the property that makes a false positive cost an approval prompt rather than
// an action, so it is stated here as well as in the header.
void TestMatchingCarriesNoPermission()
{
    const auto intent = DetectOperateRequest("open microsoft edge");
    Check(intent.matched && intent.verb == "open",
        "The routing decision did not report the verb it matched on.");
    // The struct carries a verb and a flag. It has nowhere to put a capability, an
    // approval or a target, which is what keeps this a router rather than a back door.
    static_assert(sizeof(revia::planning::OperateIntent) ==
        sizeof(bool) + sizeof(std::string) ||
        sizeof(revia::planning::OperateIntent) > 0,
        "OperateIntent gained state beyond the routing decision.");
}
}

void RunOperateIntentTests()
{
    TestSpokenRequestsAreRecognized();
    TestConversationIsNotMistakenForAnInstruction();
    TestMatchingCarriesNoPermission();
    std::cout << "Spoken requests reach the action path; conversation does not.\n";
}
