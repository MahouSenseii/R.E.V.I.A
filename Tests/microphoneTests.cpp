#include "testSupport.h"

#include "Speech/speechRecognitionService.h"
#include "Speech/transcriptRouting.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace revia::speech
{

// Declared a friend by SpeechRecognitionService. These are the states a live session
// reaches through hardware and worker threads; setting them directly is what makes the
// gate testable without a sound card or a running whisper.cpp.
struct MicrophoneTestAccess
{
    static void SetAvailable(SpeechRecognitionService& service, const bool value)
    {
        service.available.store(value);
    }
    static void SetRecording(SpeechRecognitionService& service, const bool value)
    {
        service.recording.store(value);
    }
    static void SetTranscribing(SpeechRecognitionService& service, const bool value)
    {
        service.transcribing.store(value);
    }
    static void SetHandsFree(SpeechRecognitionService& service, const bool value)
    {
        service.handsFreeEnabled.store(value);
    }
    static void SetDevice(SpeechRecognitionService& service, const std::string& name)
    {
        service.configuration.microphoneDevice = name;
    }
};

} // namespace revia::speech

namespace
{
using revia::tests::Check;
using revia::speech::MicrophoneAttempt;
using revia::speech::MicrophoneDevice;
using revia::speech::MicrophoneTestAccess;
using revia::speech::SelectMicrophone;
using revia::speech::SpeechRecognitionService;
using revia::speech::TranscriptRouting;
using revia::speech::DecideTranscriptRouting;

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// A service that believes it can record, without having started anything.
void MakeReady(SpeechRecognitionService& service)
{
    MicrophoneTestAccess::SetAvailable(service, true);
    MicrophoneTestAccess::SetRecording(service, false);
    MicrophoneTestAccess::SetTranscribing(service, false);
    MicrophoneTestAccess::SetHandsFree(service, false);
}

void TestAnUnavailableRecognizerSaysSoInsteadOfFailingSilently()
{
    SpeechRecognitionService service;
    MicrophoneTestAccess::SetAvailable(service, false);

    const MicrophoneAttempt attempt = service.BeginRecordingDiagnosed();
    Check(!attempt.started, "Recording started without a recognizer.");
    Check(!attempt.recognizerAvailable,
        "The attempt did not record that the recognizer was unavailable.");
    Check(!attempt.reason.empty(),
        "A refusal carried no reason, which is what made a dead button look idle.");
    Check(Contains(attempt.reason, "not available"),
        "The reason did not say the recognizer is unavailable: " + attempt.reason);
    Check(Contains(attempt.Summary(), "recognizer_available=no"),
        "The diagnostic summary omitted recognizer availability.");
}

void TestHandsFreeOwnsTheMicrophone()
{
    SpeechRecognitionService service;
    MakeReady(service);
    MicrophoneTestAccess::SetHandsFree(service, true);

    const MicrophoneAttempt attempt = service.BeginRecordingDiagnosed();
    Check(!attempt.started, "Manual recording started while hands-free was on.");
    Check(attempt.handsFree, "The attempt did not record that hands-free was on.");
    Check(Contains(attempt.reason, "Hands-free"),
        "The refusal did not name hands-free as the reason: " + attempt.reason);
    // The gate must not have claimed the device on its way to refusing.
    Check(!service.IsRecording(),
        "A hands-free refusal left the service believing it was recording.");
}

void TestASecondPressWhileRecordingIsRefusedWithoutDisturbingTheFirst()
{
    SpeechRecognitionService service;
    MakeReady(service);
    MicrophoneTestAccess::SetRecording(service, true);

    const MicrophoneAttempt attempt = service.BeginRecordingDiagnosed();
    Check(!attempt.started, "A second recording started on top of the first.");
    Check(attempt.alreadyRecording,
        "The attempt did not record that a recording was already running.");
    Check(service.IsRecording(),
        "Refusing a second press cleared the first recording's state.");
}

// The regression. This is the defect behind "the Listen button does not reliably work".
//
// The gate used to read:
//   !available || handsFree || recording.exchange(true) || transcribing
// so a press arriving while whisper.cpp was still working -- exactly when a person
// presses again -- ran the exchange, latched `recording` to true, and then returned
// false on the transcribing test without unwinding it. Nothing cleared the flag
// afterwards, so every later press failed at the exchange and the microphone was dead
// for the rest of the session.
void TestARefusalWhileTranscribingDoesNotLatchTheMicrophoneOff()
{
    SpeechRecognitionService service;
    MakeReady(service);
    MicrophoneTestAccess::SetTranscribing(service, true);

    const MicrophoneAttempt attempt = service.BeginRecordingDiagnosed();
    Check(!attempt.started, "Recording started while a transcription was running.");
    Check(attempt.transcribing,
        "The attempt did not record that a transcription was running.");
    Check(!service.IsRecording(),
        "Refusing because of transcription latched the recording flag on. This is the "
        "bug that killed the microphone for the rest of the session.");

    // And once transcription finishes, the gate is open again rather than stuck.
    MicrophoneTestAccess::SetTranscribing(service, false);
    const MicrophoneAttempt second = service.BeginRecordingDiagnosed();
    Check(!second.alreadyRecording,
        "After transcription finished, the gate still believed a recording was in "
        "progress, so the button could never work again.");
    // Whether it truly starts depends on hardware this test does not have; what must
    // hold is that nothing refuses it before the device is ever consulted.
    Check(second.started || !Contains(second.reason, "already running"),
        "A retry after transcription was refused as a duplicate: " + second.reason);
    if (second.started)
    {
        service.EndRecording();
    }
}

void TestEveryRefusalIsRetryable()
{
    // A refusal must leave the service exactly as it found it. Anything else means the
    // first failure of a session is also the last.
    const std::vector<std::string> cases{"unavailable", "handsfree", "transcribing"};
    for (const std::string& which : cases)
    {
        SpeechRecognitionService service;
        MakeReady(service);
        if (which == "unavailable") MicrophoneTestAccess::SetAvailable(service, false);
        if (which == "handsfree") MicrophoneTestAccess::SetHandsFree(service, true);
        if (which == "transcribing") MicrophoneTestAccess::SetTranscribing(service, true);

        const MicrophoneAttempt attempt = service.BeginRecordingDiagnosed();
        Check(!attempt.started, "The " + which + " case did not refuse.");
        Check(!service.IsRecording(),
            "The " + which + " refusal left the recording flag set, so every later "
            "press would be refused as a duplicate.");
        Check(!attempt.Summary().empty(),
            "The " + which + " refusal produced no diagnostic line.");
    }
}

void TestTheDiagnosticSummaryAnswersWhyAPressDidNothing()
{
    SpeechRecognitionService service;
    MakeReady(service);
    MicrophoneTestAccess::SetTranscribing(service, true);
    const std::string summary = service.BeginRecordingDiagnosed().Summary();
    for (const std::string_view field : {"requested=yes", "started=no",
        "recognizer_available=", "hands_free=", "already_recording=", "transcribing=",
        "device="})
    {
        Check(Contains(summary, std::string(field)),
            "The diagnostic line is missing " + std::string(field) + ": " + summary);
    }
}

// Device selection. Pure, so the fallback behaviour is covered without hardware.
void TestAnEmptyDeviceMeansTheWindowsDefault()
{
    const std::vector<MicrophoneDevice> devices{{0, "Headset Microphone"}};
    for (const std::string_view raw : {"", "Default", "default", "  DEFAULT  "})
    {
        const std::string configured(raw);
        const auto selection = SelectMicrophone(devices, configured);
        Check(selection.deviceId == -1,
            "\"" + configured + "\" did not resolve to the default device.");
        Check(!selection.fellBackToDefault,
            "Choosing the default was reported as a fallback.");
        Check(!selection.report.empty(), "The default selection carried no report.");
    }
}

void TestASelectedDeviceIsMatchedByName()
{
    const std::vector<MicrophoneDevice> devices{
        {0, "Realtek Audio Input"}, {1, "Blue Yeti"}, {2, "Webcam Mic"}};

    const auto selection = SelectMicrophone(devices, "Blue Yeti");
    Check(selection.deviceId == 1, "The named microphone was not selected.");
    Check(selection.name == "Blue Yeti", "The resolved name was wrong.");
    Check(!selection.fellBackToDefault, "A present device reported a fallback.");

    // Matching is by name rather than by ordinal precisely so that this holds: the same
    // setting finds the same microphone after the list is reordered by a device being
    // unplugged elsewhere.
    const std::vector<MicrophoneDevice> reordered{
        {0, "Blue Yeti"}, {1, "Webcam Mic"}};
    const auto afterReorder = SelectMicrophone(reordered, "Blue Yeti");
    Check(afterReorder.deviceId == 0,
        "Reordering the device list changed which microphone the setting means.");
    Check(afterReorder.name == "Blue Yeti", "The name did not survive reordering.");
}

void TestAMissingDeviceFallsBackButNeverQuietly()
{
    const std::vector<MicrophoneDevice> devices{{0, "Realtek Audio Input"}};
    const auto selection = SelectMicrophone(devices, "Blue Yeti");

    Check(selection.deviceId == -1, "A missing device did not fall back to default.");
    Check(selection.fellBackToDefault,
        "A missing device fell back without marking it as a fallback, which is the "
        "silent device switch the fallback exists to avoid.");
    Check(Contains(selection.report, "Blue Yeti"),
        "The report did not name the microphone that went missing: " + selection.report);
    Check(Contains(selection.report, "not connected"),
        "The report did not say the device is not connected: " + selection.report);
}

void TestNoDevicesAtAllStillResolves()
{
    const auto selection = SelectMicrophone({}, "");
    Check(selection.deviceId == -1, "An empty machine did not resolve to default.");
    Check(!selection.fellBackToDefault,
        "Having no devices was reported as a fallback from a named one.");

    const auto named = SelectMicrophone({}, "Blue Yeti");
    Check(named.fellBackToDefault,
        "A named device on a machine with no capture hardware was not reported.");
}

void TestTheSelectedDeviceSurvivesOnTheService()
{
    SpeechRecognitionService service;
    MicrophoneTestAccess::SetDevice(service, "Blue Yeti");
    Check(service.MicrophoneDeviceSetting() == "Blue Yeti",
        "The configured microphone was not readable back from the service.");
    const MicrophoneAttempt attempt = service.BeginRecordingDiagnosed();
    Check(!attempt.device.empty(),
        "An attempt did not report which device it would have used.");
}

// Where a finished transcript goes. This is the last step of the voice path and the
// one the user experiences as "I spoke and nothing happened".
void TestATranscriptReachesTheMessageBox()
{
    Check(DecideTranscriptRouting(false, false, false, false) ==
            TranscriptRouting::FillAndHold,
        "A transcript did not reach the message box with auto-send off.");
    Check(DecideTranscriptRouting(false, false, true, false) ==
            TranscriptRouting::FillAndSend,
        "A transcript did not reach the message box with auto-send on.");
}

void TestAutoSendSendsOnlyWhenEnabledAndFree()
{
    Check(DecideTranscriptRouting(false, false, true, false) ==
            TranscriptRouting::FillAndSend,
        "Auto-send was on and idle but the transcript was not sent.");
    Check(DecideTranscriptRouting(false, false, false, false) ==
            TranscriptRouting::FillAndHold,
        "Auto-send was off but the transcript was sent anyway.");
    // Busy holds rather than drops. What the user said is not recoverable once
    // discarded, so it waits in the box instead of being thrown away.
    Check(DecideTranscriptRouting(false, false, true, true) ==
            TranscriptRouting::FillAndHold,
        "A transcript that arrived mid-reply was sent on top of it.");
}

void TestHandsFreeDoesNotAlsoFillTheBox()
{
    Check(DecideTranscriptRouting(true, false, true, false) ==
            TranscriptRouting::HandsFreeAlreadySubmitted,
        "A hands-free transcript was routed to the message box as well, which would "
        "submit it twice.");
}

void TestAnEmptyTranscriptNeverClearsWhatWasTyped()
{
    for (const bool handsFree : {false, true})
    {
        for (const bool autoSend : {false, true})
        {
            Check(DecideTranscriptRouting(handsFree, true, autoSend, false) ==
                    TranscriptRouting::Ignore,
                "An empty transcript reached the message box and would have erased "
                "whatever the user had already typed.");
        }
    }
}

} // namespace

void RunMicrophoneTests()
{
    TestAnUnavailableRecognizerSaysSoInsteadOfFailingSilently();
    TestHandsFreeOwnsTheMicrophone();
    TestASecondPressWhileRecordingIsRefusedWithoutDisturbingTheFirst();
    TestARefusalWhileTranscribingDoesNotLatchTheMicrophoneOff();
    TestEveryRefusalIsRetryable();
    TestTheDiagnosticSummaryAnswersWhyAPressDidNothing();
    TestAnEmptyDeviceMeansTheWindowsDefault();
    TestASelectedDeviceIsMatchedByName();
    TestAMissingDeviceFallsBackButNeverQuietly();
    TestNoDevicesAtAllStillResolves();
    TestTheSelectedDeviceSurvivesOnTheService();
    TestATranscriptReachesTheMessageBox();
    TestAutoSendSendsOnlyWhenEnabledAndFree();
    TestHandsFreeDoesNotAlsoFillTheBox();
    TestAnEmptyTranscriptNeverClearsWhatWasTyped();
    std::cout << "Listening refuses with a reason, never latches the microphone off "
                 "after a refusal, and resolves a saved\nmicrophone by name -- falling "
                 "back to the default only out loud.\n";
}
