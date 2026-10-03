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
    TestReferenceCheckDoesNotInventRepeatedCorrection();
    TestRepeatSignalsRequirePhraseBoundaries();
    TestActualHostilityAndQuotedSignalsRemainDistinct();
    std::cout << "Relationship evidence: actual reference checks, complete repeat signals, hostility and quotations passed.\n";
}
