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
    // Verbatim from a live session, and the reason this list grew. The verb has to sit
    // at the front, so a bare "lets" left the whole sentence unmatched: it never reached
    // the action path, and the conversation model answered as though it had.
    Expect("Lets pull up facebook", true);
    Expect("let's pull up facebook", true);
    Expect("let us open edge", true);
    Expect("how about you open notepad", true);
    Expect("send a message to Joe on Facebook saying hello", true);
    Expect("message Joe on Messenger", true);
    Expect("please reply to Joe on Facebook", true);
    Expect("post hello on Facebook", true);
    // How a request starts when it is said out loud. None of these openers names an
    // action; each only stood between the sentence and the verb that was already there.
    Expect("okay open notepad", true);
    Expect("Ok, Revia, open Edge.", true);
    Expect("alright so pull up youtube", true);
    Expect("hey open notepad", true);
    Expect("and then close notepad", true);
    Expect("also open the calculator", true);
    Expect("I need you to open edge", true);
    Expect("would you please launch notepad", true);
    Expect("okay so hey revia can you please go ahead and open edge", true);
    // The same actions under the names people also use for them.
    Expect("right click the desktop", true);
    Expect("right-click the file", true);
    Expect("double click the icon", true);
    Expect("refresh the page", true);
    Expect("reload the tab", true);
    Expect("browse to github.com", true);
    Expect("minimise the window", true);
    // A real target after a verb that also has figurative uses must still get through.
    Expect("go to the next page", true);
    Expect("start notepad", true);
    Expect("run setup.exe", true);
    Expect("close the settings window", true);
    Expect("open my documents folder", true);
    Expect("bring up my email", true);
    Expect("switch to dark mode", true);
    Expect("press enter", true);
    Expect("press the save button", true);
    Expect("quit notepad", true);
    Expect("select all the text", true);
    Expect("type hello world", true);
    Expect("focus on the notepad window", true);
    Expect("focus the browser", true);
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
    Expect("write a draft message to Joe", false);
    Expect("how do I send a message on Facebook", false);
    Expect("send me a joke", false);
    Expect("send me a joke about Facebook", false);
    Expect("reply to my question about Facebook", false);
    Expect("I sent Joe a message on Facebook", false);
    // Ordinary things to say to someone, each beginning with a verb that can also name a
    // desktop action. With pointer and keyboard control off, every one of these used to
    // be answered with the line about turning control on in Permissions.
    for (const char* said : {"go to sleep", "Go to bed, Revia.", "go to hell", "focus on me for a second", "focus please",
             "focus on what I said", "focus on your breathing",
             "start telling me about your day", "start with the basics", "start by saying hello", "start from the beginning",
             "quit joking around", "quit it", "quit being so dramatic", "run that by me again", "run me through it",
             "run with it", "run through the plan once more", "close your eyes", "open your eyes", "open your mind",
             "open with a joke", "open up", "press on", "press on with the story", "refresh my memory", "switch to another topic",
             "switch to spanish", "switch to something else", "pull up a chair", "bring up the past", "bring up that topic again",
             "launch into a rant", "minimize the risk", "maximise your potential", "select a topic", "scroll through your memories",
             "type of thing I like", "close the topic", "close enough", "select whatever you like", "start a new conversation"})
        Expect(said, false);
    // An opener in front of one of them changes nothing, and an opener alone is not a request.
    Expect("okay so start over", false);
    Expect("so start from the beginning", false);
    Expect("and then I opened it", false);
    Expect("ok", false);
    Expect("okay so", false);
    Expect("alright", false);
    Expect("so it opens slowly", false);
    Expect("hey open up to me", false);
    // "let's" is a suggestion to act, but only when something to act on follows it.
    Expect("lets talk about edge", false);
    Expect("let's start a conversation", false);
    Expect("lets", false);
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
