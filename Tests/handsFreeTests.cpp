#include "Initiative/initiativeSettings.h"
#include "reviaSessionTestAccess.h"
#include "Initiative/attentionPolicy.h"
#include "Perception/microphoneUse.h"
#include "Runtime/companion.h"
#include "Speech/addresseeGate.h"

#include <algorithm>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

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

// Her own speech keeps the follow-up window open for the person she is answering. It
// used to stamp the window with a placeholder speaker, so the window closed for the real
// one as soon as her first phrase ended.
void TestHerSpeechKeepsTheFollowUpWindowOpen()
{
    AddresseeGate gate;
    const auto start = AddresseeGate::Clock::now();
    Check(gate.Accept("Revia, read me the build log", start, false, "owner", "private", 3, true), "Speech naming her was ignored.");
    gate.NoteExchange(start + 2s, "owner", "private", 3);
    gate.RefreshExchange(start + 6s);
    Check(gate.Accept("and the warnings too", start + 7s, false, "owner", "private", 3, true),
        "A follow-up after her first spoken phrase was ignored.");
    // A long answer keeps the window open phrase by phrase.
    gate.RefreshExchange(start + 20s);
    gate.RefreshExchange(start + 35s);
    Check(gate.Accept("thanks, and the errors", start + 50s, false, "owner", "private", 3, true),
        "A follow-up shortly after a long spoken answer was ignored.");
    // It still belongs to that person and that audience only.
    gate.RefreshExchange(start + 52s);
    Check(!gate.Accept("what about me", start + 53s, false, "guest", "private", 3, true) &&
              !gate.Accept("and now", start + 53s, false, "owner", "private", 4, true),
        "Her speech handed the follow-up window to someone else.");
    // And speech long after the window closed does not reopen it.
    gate.RefreshExchange(start + 120s);
    Check(!gate.Accept("one more thing", start + 121s, false, "owner", "private", 3, true),
        "Her speech reopened a follow-up window that had already closed.");
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
    ReviaSession session(revia::runtime::CompanionPaths(directory.root,
        {"hands-free-fixture", "Hands-free fixture", "assistant", false}));
    const std::stop_token operation = Access::OperationToken(session);

    Access::Hear(session, Heard("Transcript", "Revia, how is it going?"));
    Check(Access::TakeOfferedInput(session).empty() && !operation.stop_requested(),
        "A session that was not started admitted speech or cancelled work.");
    Access::MarkStudioStarted(session, true);
    Access::Hear(session, Heard("SpeechDetected"));
    Check(!operation.stop_requested(), "Detected speech cancelled the operation in progress.");
    Access::Hear(session, Heard("Transcript", "no I said the blue one"));
    Check(!operation.stop_requested() && Access::TakeOfferedInput(session).empty(),
        "Speech not addressed to her cancelled work or became a message.");

    Access::Hear(session, Heard("Transcript", "Revia, how is it going?"));
    Check(Access::TakeOfferedInput(session) == "Revia, how is it going?" && operation.stop_requested(),
        "Speech addressed to her did not reach her or supersede the reply in progress.");

    // Turning the microphone off applies at once: what it still hears is dropped, and Listen is
    // refused until it is turned back on and Revia restarts.
    Access::UsePreferences(session, directory.root);
    const auto off = session.SetPreference("speechRecognition.enabled", "off");
    Check(off.succeeded && off.message.find("stops listening now") != std::string::npos, "Turning the microphone off was not live.");
    Access::Hear(session, Heard("Transcript", "Revia, are you still listening?"));
    Check(Access::TakeOfferedInput(session).empty() && !session.BeginListening(),
        "Speech heard after the microphone was turned off still reached her.");
    // Hands-free cannot start behind the master switch, from /set or from Settings.
    std::vector<std::string> microphonePhases;
    std::mutex phasesMutex;
    const auto subscription = session.Events().Subscribe(
        [&](const revia::runtime::RuntimeEvent& event)
        {
            if (event.component == "Microphone")
            {
                std::lock_guard lock(phasesMutex);
                microphonePhases.push_back(event.phase);
            }
        });
    const auto handsFree = session.SetPreference("speechRecognition.handsFree", "on");
    session.Events().Unsubscribe(subscription);
    Check(handsFree.succeeded && !session.IsHandsFreeEnabled() &&
              std::find(microphonePhases.begin(), microphonePhases.end(), "HandsFree") == microphonePhases.end() &&
              handsFree.message.find("next time") != std::string::npos,
        "Hands-free started listening while the microphone was off.");
    const auto on = session.SetPreference("speechRecognition.enabled", "on");
    Access::Hear(session, Heard("Transcript", "Revia, can you hear me now?"));
    Check(on.succeeded && on.message.find("next time") != std::string::npos && Access::TakeOfferedInput(session).empty(),
        "Turning the microphone back on claimed to apply before a restart.");
    Access::MarkStudioStarted(session, false);
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
    TestHerSpeechKeepsTheFollowUpWindowOpen();
    TestVoicesInTheRoomNeitherInterruptNorReachHer();
    TestACallKeepsHerQuiet();
    std::cout << "Hands-free answers only speech meant for Revia; other voices neither reach "
        "nor interrupt her.\n";
}
