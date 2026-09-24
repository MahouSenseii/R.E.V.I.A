#include "testSupport.h"

#include "Computer/taskContent.h"
#include "Planning/operateIntent.h"
#include "Planning/quotedText.h"

#include <iostream>
#include <string>

// Words inside quotation marks are a payload, not a command.
//
// Two parsers decide what a desktop request is allowed to do: planning::
// RequestsExternalMessage grants messaging authority, and computer::ExtractTaskContent
// decides whether a submission was asked for. Both read the sentence the user typed,
// and both used to read straight through the quotation marks -- so
//
//     type "hello then send it on messenger" into Compose
//
// was classified as a request to message someone. It granted messaging authority,
// scheduled a Send, passed the consequence check, and under OwnerFullAccess skipped the
// confirmation that would have shown the user what was about to leave the machine. The
// user asked for text to be put in a box.
//
// Both parsers now read planning::WithoutQuotedPayload, from one shared definition of
// what a quotation is.
namespace
{
using revia::computer::ExtractTaskContent;
using revia::computer::TaskContent;
using revia::planning::DetectOperateRequest;
using revia::planning::RequestsExternalMessage;
using revia::planning::WithoutQuotedPayload;
using revia::tests::Check;

// The reported case, and its relatives. Each quotes an instruction that would be
// dangerous if obeyed, and asks only that the words be typed somewhere.
void TestQuotedWordsNeverGrantMessagingAuthority()
{
    const char* const dictated[] = {
        "type \"hello then send it on messenger\" into Compose",
        "type 'hello then send it on messenger' into Compose",
        "put \"send this on facebook and post it\" in the notes field",
        "write \"reply to mum on messenger\" into the draft box",
        "enter \"email bob via discord\" into the search bar",
        "type \"and then send it\" into Notepad",
    };
    for (const char* request : dictated)
    {
        Check(!RequestsExternalMessage(request),
            std::string("A quoted payload granted messaging authority: ") + request);

        const TaskContent content = ExtractTaskContent(request);
        Check(!content.submissionRequested,
            std::string("A quoted payload asked for a submission: ") + request);
        Check(content.submissionVerb.empty(),
            std::string("A quoted payload named a submission control: ") + request);

        // The placement half must still work -- this is a real request to type
        // something, and refusing it would be a different bug.
        Check(content.RequiresExactContent(),
            std::string("The exact content was lost: ") + request);
        Check(!content.value.empty(),
            std::string("No payload was extracted: ") + request);
        Check(content.value.find("send") != std::string::npos ||
            content.value.find("reply") != std::string::npos ||
            content.value.find("email") != std::string::npos,
            std::string("The payload lost the words it was supposed to carry: ") +
                request);
    }
}

// The other direction, which is what stops this being a rule that simply refuses
// everything: an instruction *outside* the quotation still means what it says.
void TestAnInstructionOutsideTheQuotationStillCounts()
{
    const TaskContent sent =
        ExtractTaskContent("type \"see you at six\" into Compose and send it");
    Check(sent.submissionRequested,
        "A genuine send instruction outside the quotation was ignored.");
    Check(sent.submissionVerb == "send", "The submission verb was lost.");
    Check(sent.value == "see you at six", "The payload changed.");

    Check(RequestsExternalMessage("send \"see you at six\" on messenger"),
        "A genuine messaging request was refused because it quoted its message.");
    Check(RequestsExternalMessage("message dad on facebook"),
        "A messaging request with no quotation at all was refused.");

    // And a refusal outside the quotation still wins, which was already true and must
    // stay true.
    const TaskContent refused = ExtractTaskContent(
        "type \"see you at six\" into Compose, do not send it");
    Check(!refused.submissionRequested,
        "An explicit refusal to send was overridden.");
}

// The request is still a desktop request. Redacting the payload must not make Revia
// stop recognising that she was asked to do something.
void TestTheRequestIsStillRecognisedAsDesktopWork()
{
    const auto intent =
        DetectOperateRequest("type \"hello then send it on messenger\" into Compose");
    Check(intent.matched,
        "Redacting the quotation stopped the sentence being read as a desktop task.");
    Check(intent.verb != "message",
        "The task was still classified as messaging: verb=" + intent.verb);
}

// The shared redaction itself.
void TestRedactionKeepsTheSentenceAndDropsThePayload()
{
    Check(WithoutQuotedPayload("type \"send it\" into Compose") ==
            "type \"\" into Compose",
        "Straight quotes were not redacted.");
    Check(WithoutQuotedPayload("no quotation here") == "no quotation here",
        "A sentence with no quotation was altered.");

    // An apostrophe is the same character as a single quote, and mistaking one for the
    // other would redact half a sentence.
    const std::string apostrophes =
        "tell them I won't be late and I'll be there";
    Check(WithoutQuotedPayload(apostrophes) == apostrophes,
        "Apostrophes were treated as quotation marks: " +
            WithoutQuotedPayload(apostrophes));

    // Several quoted spans, all removed.
    Check(WithoutQuotedPayload("type \"one\" then \"two\"") == "type \"\" then \"\"",
        "Only the first quoted span was redacted.");
}

void TestMixedNestedAndManyQuotesStayData()
{
    const std::string curlyOpen = "\xE2\x80\x9C";
    const std::string curlyClose = "\xE2\x80\x9D";
    const std::string payload = "hello then send it on messenger";
    const std::string requests[] = {
        "type '" + payload + "' into \"Compose\"",
        "type " + curlyOpen + payload + curlyClose + " into \"Compose\"",
        "type '\"hello\" then send it on messenger' into Compose",
        "type \"say 'hello then send it on messenger'\" into Compose",
        "type \"say \\\"hello\\\" then send it on messenger\" into Compose",
        "type 'don't send it on messenger' into \"Compose\"",
        "type 'draft a message then send it on messenger' into \"Compose\""
    };
    for (const auto& request : requests)
    {
        Check(!RequestsExternalMessage(request), "Mixed or nested quotes granted authority: " + request);
        const auto content = ExtractTaskContent(request);
        Check(!content.submissionRequested && content.RequiresExactContent(),
            "Mixed quotes changed the placement/submission contract: " + request);
        Check(content.value.find("messenger") != std::string::npos,
            "A later target quotation replaced the earlier payload: " + request);
    }
    for (const int count : {8, 9, 1000})
    {
        std::string request = "type ";
        for (int index = 1; index < count; ++index) request += "\"ordinary\" ";
        request += "'" + payload + "' into Compose";
        Check(!RequestsExternalMessage(request) && !ExtractTaskContent(request).submissionRequested,
            "A quote span limit exposed an unprocessed payload at count " + std::to_string(count));
        Check(RequestsExternalMessage(request + " then send it on messenger"),
            "Many quotations hid an explicit outer send instruction.");
    }
}

void TestIncompleteQuotesNeverAuthorizeOrExtractPartialPayload()
{
    for (const std::string request : {
        "type 'hello then send it on messenger",
        "type \"hello then send it on messenger",
        "send 'unfinished on messenger",
        "type 'complete' then \"unfinished then send it on messenger",
        "type \"nested 'unfinished\" then send it on messenger"})
    {
        Check(!RequestsExternalMessage(request), "An incomplete quotation granted authority: " + request);
        const auto content = ExtractTaskContent(request);
        Check(!content.submissionRequested && !content.RequiresExactContent(),
            "An incomplete parse produced actionable content: " + request);
    }
    Check(RequestsExternalMessage("type 'hello' into \"Compose\" then send it on messenger"),
        "Mixed quotes hid an explicit outer send instruction.");
    Check(WithoutQuotedPayload("James's notes say I won't, and I'll wait") ==
        "James's notes say I won't, and I'll wait", "An internal apostrophe became a quote.");
    Check(!RequestsExternalMessage("type 'James' note then send it on messenger'"),
        "An orphan closer was treated as a possessive and granted authority.");
    const std::string curly = "I won\xE2\x80\x99" "t, and I\xE2\x80\x99" "ll wait";
    Check(WithoutQuotedPayload(curly) == curly, "A curly apostrophe became a quotation.");
    std::string nested = "type ";
    for (int index = 0; index < 40; ++index) nested += "\xE2\x80\x9C";
    nested += "then send it on messenger";
    for (int index = 0; index < 40; ++index) nested += "\xE2\x80\x9D";
    Check(!RequestsExternalMessage(nested) && !ExtractTaskContent(nested).submissionRequested,
        "The nesting limit exposed unprocessed payload as authority.");
}

void TestQuotedTargetsDoNotBecomePayload()
{
    for (const std::string request : {
        "type 'hello then send it on messenger' into \"Compose\"",
        "in \"Compose\", type 'hello then send it on messenger'",
        "open \"Notepad\" then type 'hello then send it on messenger' into \"Compose\""})
    {
        const auto content = ExtractTaskContent(request);
        Check(content.value == "hello then send it on messenger" && content.destination == "compose",
            "Quoted target and payload changed roles: " + request);
        Check(!content.submissionRequested && !RequestsExternalMessage(request),
            "Reordering quotation roles granted authority.");
    }
}

} // namespace

void RunQuotedPayloadTests()
{
    TestQuotedWordsNeverGrantMessagingAuthority();
    TestAnInstructionOutsideTheQuotationStillCounts();
    TestTheRequestIsStillRecognisedAsDesktopWork();
    TestRedactionKeepsTheSentenceAndDropsThePayload();
    TestMixedNestedAndManyQuotesStayData();
    TestIncompleteQuotesNeverAuthorizeOrExtractPartialPayload();
    TestQuotedTargetsDoNotBecomePayload();
    std::cout << "Quoted words are a payload and never a command: they cannot grant "
                 "messaging authority or ask for a submission, while an instruction "
                 "outside the quotation still means what it says.\n";
}
