#include "testSupport.h"
#include "Core/speechAttribution.h"
#include "Identity/relationshipEvidence.h"
#include "Agents/conversationStylePolicy.h"

#include <iostream>

namespace
{
using namespace revia::conversation;
using namespace revia::identity;
using revia::tests::Check;

void TestReportedSpeechDoesNotBecomeUserEvidence()
{
    for (const std::string text : {
        "Someone else said you're useless. I just said you need therapy. Stop saying chuckles.",
        "My friend said \"you're useless, I already said that. My name is Bob.\"",
        "Sam said: 'You are stupid. Call me Sam.'",
        "She told you: \"Thanks, you're perfect. Let's work together.\"",
        "You said I hate you. My name is Alice.",
        "Someone said 'you're useless' to me.",
        "The example string is \"you're useless\"."})
    {
        const auto signals = ReadConversationSignals(text, {}, true);
        Check(!signals.hostileTowardRevia && !signals.repeatedCorrection &&
            !signals.expressedAppreciation && !signals.collaborative,
            "Reported words became the messenger's relationship evidence: " + text);
        Check(ReadStatedName(text).empty(), "A quoted introduction renamed the current speaker: " + text);
        const auto event = BuildRelationshipEvent("messenger", signals);
        Check(event.disrespectEvidence == 0 && event.conflict == 0 && event.trustEvidence == 0,
            "Someone else's words changed the messenger's trust or grievance.");
    }
    const std::string mixed = "Someone said \"you're useless\". I disagree. Thanks for helping. My name is Davis.";
    Check(ReadConversationSignals(mixed, {}, true).expressedAppreciation && ReadStatedName(mixed) == "Davis",
        "The messenger's own praise or introduction outside a quote was erased.");
    Check(ReadConversationSignals("She said \"nice work\". You are useless.", {}, true).hostileTowardRevia,
        "A quotation hid the current user's own insult after it.");
    Check(ReadConversationSignals("You're useless. Someone said \"good job\".", {}, true).hostileTowardRevia,
        "A quotation hid the current user's own insult before it.");
    Check(ReadConversationSignals("No, I already said that. They said \"hi\".", {}, true).repeatedCorrection,
        "A real user correction outside the reported speech was lost.");
    Check(!ReadConversationSignals("You're useless. They said \"just kidding\".", {}, true).explicitlyPlayful,
        "Someone else's joking qualifier softened the user's insult.");
}

void TestSpeakerAndRecipientBoundaries()
{
    const auto explicitTarget = ReadSpeechAttribution("She said to you: \"You're useless.\"");
    Check(explicitTarget.quotes.size() == 1 && explicitTarget.quotes[0].speaker == QuotedSpeaker::OtherPerson &&
        explicitTarget.quotes[0].recipient == QuoteRecipient::Revia, "The explicit Revia addressee was lost.");
    const auto userTarget = ReadSpeechAttribution("Someone said 'you're useless' to me.");
    Check(userTarget.quotes.size() == 1 && userTarget.quotes[0].recipient == QuoteRecipient::User,
        "A third party's insult to the user became an insult to Revia.");
    const auto unspecified = ReadSpeechAttribution("My friend said \"I need help.\"");
    Check(unspecified.quotes[0].recipient == QuoteRecipient::Unknown, "An unspecified quote recipient was invented.");
    const std::string correction = "But that's what they said to you after you said You know, if they really thought I needed therapy, "
        "maybe they should have asked. I don't have feelings. chuckles";
    const auto corrected = ReadSpeechAttribution(correction);
    Check(corrected.correctsAttribution && corrected.refersToOtherPerson && corrected.quotes.size() == 1 &&
        corrected.quotes[0].speaker == QuotedSpeaker::Revia, "The nested correction reversed Revia's earlier words.");
    const auto annotated = AnnotateReportedSpeech(correction);
    Check(annotated.find("speaker: Revia, in an earlier turn") != std::string::npos &&
        annotated.find("I don't have feelings. chuckles") != std::string::npos,
        "Prompt labels lost or rewrote the actual quoted words.");
    const std::string smart = "She said \xE2\x80\x9Cyou're useless\xE2\x80\x9D. Thanks.";
    Check(!ReadConversationSignals(smart, {}, true).hostileTowardRevia &&
        ReadConversationSignals(smart, {}, true).expressedAppreciation, "Smart quotation boundaries were lost.");
    const std::string own = "I said \"I need therapy\".";
    Check(ReadSpeechAttribution(own).quotes[0].speaker == QuotedSpeaker::User,
        "The user's quotation of their own words acquired a third-party speaker.");
    const std::string code = "Print \"hello\" and return \"world\".";
    Check(AnnotateReportedSpeech(code) == code && BuildSpeechAttributionGuidance(code, {}).empty(),
        "Ordinary string literals received conversation-role markup.");
}

void TestReportedRebuttalAndFollowup()
{
    const std::string report = "Someone else said if you don't have feelings why are you getting defensive. "
        "BTW YOU are the ones who bought up feelings. I just said you need therapy. "
        "That could be compiler update therapy. So stop making silly assumptions - like a human being with feelings, "
        "and stop saying chuckles. It's weird AF. It makes me think you need an exorcism as well as therapy\"";
    const auto parsed = ReadSpeechAttribution(report);
    Check(parsed.quotes.size() == 1 && parsed.quotes[0].speaker == QuotedSpeaker::OtherPerson &&
        parsed.userAuthoredText.find("I just said") == std::string::npos,
        "The unbalanced, unquoted report from the regression transcript lost its speaker.");
    const std::vector<conversationMessage> context = {{"user", report},
        {"assistant", "You need therapy. Stop making that sound."},
        {"user", "What would you say to them directly ?"}};
    const auto guidance = BuildSpeechAttributionGuidance(context.back().content, context);
    Check(guidance.find("I=Revia, you=that person") != std::string::npos &&
        guidance.find("messenger") != std::string::npos,
        "A direct-reply follow-up did not resolve to the previous user report.");
    Check(BuildSpeechAttributionGuidance("I need therapy.", context).empty(),
        "An unrelated new user turn inherited an old third-party target.");
    Check(BuildSpeechAttributionGuidance("What would you say to them directly?",
        {{"assistant", report}}).empty(), "An assistant's invented report established speaker evidence.");
    const auto roast = ReadSpeechAttribution("Roast the mfor saying if you don't have feelings why are you getting defensive. "
        "Also tell here \"stop saying chuckles\"\n\nand by her they are talking about you");
    Check(roast.requestsDirectReply && roast.correctsAttribution && roast.quotes.size() == 1 &&
        roast.quotes[0].speaker == QuotedSpeaker::OtherPerson && roast.quotes[0].recipient == QuoteRecipient::Revia,
        "The explicit her=Revia correction did not bind the pasted roast target.");

    revia::agents::ConversationStylePolicy policy;
    Check(!policy.CanStreamReply(context.back().content, context) && policy.CanStreamReply("You are useless."),
        "A relayed reply could be spoken before attribution repair, or direct banter lost streaming.");
    const auto repaired = policy.RefineReply(context.back().content, context,
        "That was a cheap shot. And stop saying chuckles. It's weird AF. It makes me think you need an exorcism as well as therapy.");
    Check(repaired.find("That was a cheap shot.") != std::string::npos && repaired.find("stop saying chuckles") == std::string::npos,
        "A copied complaint about Revia's sounds survived as a complaint about the critic.");
    Check(policy.RefineReply(context.back().content, context, "That was a cheap shot. Stop making that sound.") == "That was a cheap shot.",
        "A paraphrased command about Revia's sounds reversed the recipient.");
    const std::string echo = "You need therapy.";
    Check(policy.RefineReply("Reply to them with exactly what they said, verbatim.", context, echo) == echo,
        "An explicit request to quote the source was treated as a mistaken rebuttal.");
    const std::string fix = "But that's what they said to you after you said I don't have feelings. I'd rather get a compiler update.";
    Check(policy.RefineReply(fix, context, "You got that wrong. Those were your words.") ==
        "Those were my words. They were responding to me; you were passing their message along.",
        "A correction could blame the messenger or reverse ownership of Revia's quoted words again.");
    Check(policy.RefineReply(fix, context, "You're twisting my own words. Stop pretending you're the messenger.") ==
        "Those were my words. They were responding to me; you were passing their message along.",
        "An attribution correction kept arguing with the messenger.");
    Check(policy.RefineReply("My friend said to me: \"You are useless.\" What would you say to them directly?", {},
        "Useless? I can run locally and handle tasks without any internet access.") ==
        "Calling them that was out of line. Make your point without the personal digs.",
        "A reply to an insult aimed at the user defended Revia's own abilities instead.");
    const auto unchanged = "Those were my words. They were replying to me.";
    Check(policy.RefineReply(fix, context, unchanged) == unchanged,
        "An already correct attribution response was replaced unnecessarily.");
}
}

void RunSpeechAttributionTests()
{
    TestReportedSpeechDoesNotBecomeUserEvidence();
    TestSpeakerAndRecipientBoundaries();
    TestReportedRebuttalAndFollowup();
    std::cout << "Reported speech, messenger evidence, correction and follow-up attribution tests passed.\n";
}
