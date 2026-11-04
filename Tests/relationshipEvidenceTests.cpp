#include "testSupport.h"

#include "Emotion/stimulusBuilder.h"
#include "Identity/relationshipEvidence.h"

#include <cmath>
#include <iostream>
#include <string>

namespace
{
using revia::tests::Check;
using revia::identity::ReadConversationSignals;
using revia::identity::BuildRelationshipEvent;
using revia::emotion::BuildConversationStimulus;

void TestNegatedNamingDirectivesDoNotIntroduceTheSpeaker()
{
    using revia::identity::ReadStatedName;
    for (const std::string input : {"Don't call me Sam.", "Do not call me Sam.", "Please never call me Sam.",
             "You must not call me Sam.", "Don't ever call me Sam.", "I asked you not to call me Sam.",
             "I don't want you to call me Sam.", "Don’t call me Sam."})
        Check(ReadStatedName(input).empty(), "A rejected name became the current speaker's introduction: " + input);
    std::string missingAffirmative;
    for (const std::string input : {"Don't call me Sam. Call me Alex.", "Call me Alex. Don't call me Sam.",
             "She said \"Don't call me Sam.\" Call me Alex.", "Don't call me Sam, you can call me Alex.",
             "Do not forget that my name is Alex.", "I'm not upset; call me Alex.",
             "Why not call me Alex?", "Don't call me Sam. Why not call me Alex?",
             "I'm not \"upset.\" Call me Alex."})
        if (ReadStatedName(input)!="Alex") missingAffirmative += "\n" + input;
    Check(missingAffirmative.empty(), "Rejected affirmative introductions:" + missingAffirmative);
}

void TestRejectedNameMatchDoesNotHideALaterIntroduction()
{
    struct Example { const char* input; const char* expected; };
    std::string failures;
    for (const auto& example : {
             Example{"My name is not Sam. My name is Davis.", "Davis"},
             Example{"Call me later. Call me Alex.", "Alex"},
             Example{"Please recall me tomorrow. Call me Alex.", "Alex"},
             Example{"My name is X. My name is Davis.", "Davis"},
             Example{"Don't call me Sam. Call me later. Call me Alex.", "Alex"},
             Example{"I'm upset; call me later. Call me Alex.", "Alex"},
             Example{"Call me later. Call me tomorrow.", ""},
             Example{"My name is not Sam. My name is not Alex.", ""},
             Example{"Call me later. She said \"Call me Alex.\"", ""},
             Example{"This is fine. This is Alex.", ""},
             Example{"Call me Sam. Call me Alex.", "Sam"},
             Example{"Call me Sam. My name is Davis.", "Davis"}})
    {
        const auto actual = revia::identity::ReadStatedName(example.input);
        if (actual != example.expected)
            failures += "\n" + std::string(example.input) + " => [" + actual + "]; expected [" + example.expected + "]";
    }
    Check(failures.empty(), "A rejected match hid a later introduction or changed established naming rules:" + failures);
}

void TestReferenceCheckDoesNotInventRepeatedCorrection()
{
    const std::string input = "Check your explanation against these supplied facts: the compiler produces object files; "
                              "a static linker extracts needed object files from an archive and links an executable; "
                              "a DLL remains separate, and its import library helps linking while the runtime loader loads "
                              "the DLL. Correct any earlier error in three sentences.";
    const auto signals = ReadConversationSignals(input, {}, true);
    Check(!signals.repeatedCorrection, "The actual reference-check request falsely classified 'against' as repeated correction 'again'.");
    Check(!signals.hostileTowardRevia, "Checking supplied facts was classified as hostility.");
    const auto event = BuildRelationshipEvent("synthetic-speaker", signals);
    Check(event.conflict == 0.0F && event.negativeInteraction == 0.0F && event.disrespectEvidence == 0.0F,
        "A neutral reference check created negative relationship evidence.");
    const auto stimulus = BuildConversationStimulus("synthetic-speaker", signals);
    Check(stimulus.eventType == "message" && !stimulus.selfCaused && stimulus.failure == 0.0F && std::abs(stimulus.valence) < 0.0001F,
        "A neutral reference check created a repeated-correction failure stimulus.");
}

void TestRepeatSignalsRequirePhraseBoundaries()
{
    for (const std::string input : {"Compare A against B.", "Againish is a synthetic label.", "Use the again2 variable.",
             "Check against_again.", "This piano, I meant to tune."})
    {
        Check(!ReadConversationSignals(input, {}, true).repeatedCorrection,
            "An embedded signal fragment became repeated-correction evidence: " + input);
    }
    for (const std::string input :
        {"Again.", "(again)", "Please try AGAIN!", "Check against A, then try again.", "No, I already said that.", "No, I just said eight.",
            "You keep missing that.", "It is still not right.", "No, I meant eight.", "That is not what I said."})
    {
        const auto signals = ReadConversationSignals(input, {}, true);
        Check(signals.repeatedCorrection, "A genuine complete repeat/correction signal was lost: " + input);
        const auto event = BuildRelationshipEvent("synthetic-speaker", signals);
        Check(event.conflict > 0.0F && event.disrespectEvidence == 0.0F, "Correction friction became disrespect or lost its evidence.");
        const auto stimulus = BuildConversationStimulus("synthetic-speaker", signals);
        Check(stimulus.eventType == "repeated_correction" && stimulus.selfCaused && stimulus.failure > 0.0F,
            "A genuine repeated correction lost Revia's responsibility for the miss.");
    }
}

void TestActualHostilityAndQuotedSignalsRemainDistinct()
{
    for (const std::string input : {"You're useless.", "You are stupid.", "I hate you.", "Shut up."})
    {
        const auto signals = ReadConversationSignals(input, {}, true);
        Check(signals.hostileTowardRevia, "The repeat boundary change removed actual hostility: " + input);
        const auto event = BuildRelationshipEvent("synthetic-speaker", signals);
        const auto stimulus = BuildConversationStimulus("synthetic-speaker", signals);
        Check(event.disrespectEvidence > 0.0F && stimulus.eventType == "hostile_remark" && stimulus.valence < -0.5F,
            "Actual hostility lost its relationship or emotion evidence.");
    }
    const auto reported = ReadConversationSignals("She said \"Again, you're useless.\" Check the explanation against the facts.", {}, true);
    Check(!reported.hostileTowardRevia && !reported.repeatedCorrection,
        "Quoted hostility or repeat signals became the messenger's evidence.");
}
}

void RunRelationshipEvidenceTests()
{
    TestNegatedNamingDirectivesDoNotIntroduceTheSpeaker();
    TestRejectedNameMatchDoesNotHideALaterIntroduction();
    TestReferenceCheckDoesNotInventRepeatedCorrection();
    TestRepeatSignalsRequirePhraseBoundaries();
    TestActualHostilityAndQuotedSignalsRemainDistinct();
    std::cout << "Relationship evidence: actual reference checks, complete repeat signals, hostility and quotations passed.\n";
}
