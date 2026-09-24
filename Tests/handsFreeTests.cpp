#include "reviaSessionTestAccess.h"
#include "Initiative/attentionPolicy.h"
#include "Perception/microphoneUse.h"
#include "Speech/addresseeGate.h"

#include <iostream>

namespace
{
using namespace std::chrono_literals;
using revia::runtime::ReviaSession;
using revia::speech::AddresseeGate;
using revia::speech::AddresseeSettings;
using revia::tests::Check;
using Access = revia::runtime::ReviaSessionTestAccess;

void TestHerNameIsMatchedAsAWholeWord()
{
    const AddresseeSettings defaults;
    for (const char* named : {"Revia, what time is it?", "is that revia's idea", "OK REVIA",
             "hey rivia can you check"})
    {
        Check(AddresseeGate::MentionsWakeWord(named, defaults.wakeWords),
            std::string("Her name was not recognised in: ") + named);
    }
    for (const char* other : {"reviewing the code now", "turn it up", "", "revival"})
    {
        Check(!AddresseeGate::MentionsWakeWord(other, defaults.wakeWords),
            std::string("Speech without her name was taken as addressed: ") + other);
    }
}

void TestFollowUpsNeedNoNameButOthersAreIgnored()
{
    AddresseeGate gate;
    const auto start = AddresseeGate::Clock::now();
    Check(!gate.Accept("turn up the music", start, false),
        "Speech that never named her was answered.");
    Check(gate.Accept("Revia, how long until the build finishes?", start, false),
        "Speech naming her was ignored.");
    Check(gate.Accept("and the tests too", start + 15s, false),
        "A follow-up inside the conversation window was ignored.");
    Check(!gate.Accept("okay see you later", start + 40s, false),
        "Speech long after the conversation was taken as a follow-up.");

    gate.NoteExchange(start + 60s);
    Check(!gate.Accept("sounds good", start + 65s, true),
        "During a call, speech without her name was answered.");
    Check(gate.Accept("Revia, mute for a bit", start + 66s, true),
        "During a call, speech naming her was ignored.");

    AddresseeSettings open;
    open.requireWakeWord = false;
    AddresseeGate everything(open);
    Check(everything.Accept("turn up the music", start, false),
        "requireWakeWord=false no longer answers everything it hears.");
}

revia::speech::RecognitionEvent Heard(const std::string& phase, const std::string& transcript = {})
{
    revia::speech::RecognitionEvent event{phase, "", transcript};
    event.automatic = true;
    return event;
}

void TestVoicesInTheRoomNeitherInterruptNorReachHer()
{
    revia::tests::ScopedTestDirectory directory;
    ReviaSession session;
    const std::stop_token operation = Access::OperationToken(session);

    Access::Hear(session, Heard("SpeechDetected"));
    Check(!operation.stop_requested(), "Detected speech cancelled the operation in progress.");
    Access::Hear(session, Heard("Transcript", "no I said the blue one"));
    Check(!operation.stop_requested() && Access::TakeOfferedInput(session).empty(),
        "Speech not addressed to her cancelled work or became a message.");

    Access::Hear(session, Heard("Transcript", "Revia, how is it going?"));
    Check(Access::TakeOfferedInput(session) == "Revia, how is it going?" &&
        operation.stop_requested(),
        "Speech addressed to her did not reach her or supersede the reply in progress.");
}

void TestACallKeepsHerQuiet()
{
    using revia::perception::MicrophoneUse;
    using revia::perception::OtherAppUsingMicrophone;
    const std::vector<std::string> own = {"ReviaDesktop.exe", "R_E_V_I_A.exe"};
    Check(OtherAppUsingMicrophone({{"Zoom.exe", true}, {"reviadesktop.exe", true}}, own),
        "A call app holding the microphone was not recognised as a call.");
    Check(!OtherAppUsingMicrophone({{"ReviaDesktop.exe", true}, {"Zoom.exe", false}}, own),
        "Revia's own hands-free microphone, or a finished call, was taken as a call.");

    initiativeSettings talkative;
    talkative.bEnabled = true;
    revia::initiative::AttentionPolicy policy{talkative};
    revia::initiative::AttentionContext context;
    context.sinceLastInput = std::chrono::minutes{10};
    context.inCall = true;
    Check(policy.Evaluate(0.99f, context) == revia::initiative::AttentionVerdict::InCall,
        "She would speak up unprompted during a call.");
}

} // namespace

void RunHandsFreeTests()
{
    TestHerNameIsMatchedAsAWholeWord();
    TestFollowUpsNeedNoNameButOthersAreIgnored();
    TestVoicesInTheRoomNeitherInterruptNorReachHer();
    TestACallKeepsHerQuiet();
    std::cout << "Hands-free answers only speech meant for Revia; other voices neither reach "
        "nor interrupt her.\n";
}
