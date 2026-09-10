#include "testSupport.h"

#include "Windows/desktopControlExecutor.h"
#include "Windows/targetBinding.h"

#include <iostream>
#include <string>

namespace
{

using namespace revia::actions::windows;
using revia::tests::Check;

// The binding logic, tested without a desktop.
//
// Capturing a binding needs a real window; deciding whether one still describes reality
// does not, and that decision is the part that has to be right. These are evidence for
// the comparison rules only -- whether the capture reads a real control correctly is a
// native question and is not answered here.

TargetBinding Binding(void* window, const std::uint32_t pid, const std::string& runtimeId)
{
    TargetBinding binding;
    binding.valid = true;
    binding.observedAt = std::chrono::steady_clock::now();
    binding.window = window;
    binding.processId = pid;
    binding.runtimeId = runtimeId;
    binding.automationId = "editField";
    binding.controlName = "Document body";
    binding.controlType = 50004;
    binding.contextFingerprint = "Untitled|" + runtimeId + "|editField|50004|Document body";
    return binding;
}

void TestSurrogatePairsSurviveSplitting()
{
    // Two code units, one character. Delivered separately they are two broken halves.
    const std::wstring emoji = L"\xD83D\xDE00";
    const auto scalars = SplitScalars(emoji);
    Check(scalars.size() == 1 && scalars.front().size() == 2,
        "A surrogate pair was split into two scalars.");

    const std::wstring mixed = std::wstring(L"a") + emoji + L"b";
    const auto mixedScalars = SplitScalars(mixed);
    Check(mixedScalars.size() == 3 && mixedScalars[0] == L"a" &&
        mixedScalars[1].size() == 2 && mixedScalars[2] == L"b",
        "Mixed BMP and non-BMP text did not split into three scalars.");

    // A lone high surrogate is malformed input, not a pair. It must not swallow the
    // character after it.
    const std::wstring lone = std::wstring(1, static_cast<wchar_t>(0xD83D)) + L"x";
    const auto loneScalars = SplitScalars(lone);
    Check(loneScalars.size() == 2 && loneScalars[1] == L"x",
        "A lone high surrogate consumed the following character.");

    Check(SplitScalars(L"").empty(), "Empty text produced scalars.");
    Check(SplitScalars(L"plain").size() == 5, "Plain text miscounted.");

    // Control characters stay their own scalars so the typing loop can treat them as
    // operations rather than as text.
    const auto withControls = SplitScalars(L"a\tb\nc");
    Check(withControls.size() == 5 && withControls[1] == L"\t" && withControls[3] == L"\n",
        "Tab and newline were not isolated as their own scalars.");
}

void TestFreshnessIsMonotonicAndBounded()
{
    const auto now = std::chrono::steady_clock::now();
    TargetBinding binding = Binding(reinterpret_cast<void*>(0x1), 100, "7.1");
    binding.observedAt = now;

    Check(IsFresh(binding, now), "A just-made binding was not fresh.");
    Check(IsFresh(binding, now + std::chrono::milliseconds(1500)),
        "A binding expired inside its own limit.");
    Check(!IsFresh(binding, now + std::chrono::milliseconds(2500)),
        "A binding outlived the freshness limit.");

    // An observation timestamped in the future is not a fresh observation, it is a
    // broken one.
    Check(!IsFresh(binding, now - std::chrono::milliseconds(50)),
        "A binding observed in the future was accepted.");

    TargetBinding never;
    Check(!IsFresh(never, now), "An invalid binding was reported fresh.");
}

void TestADifferentControlIsNoticedInsideOneWindow()
{
    void* window = reinterpret_cast<void*>(0x1);
    const TargetBinding document = Binding(window, 100, "7.1");

    Check(CompareBindings(document, document).empty(),
        "A binding did not match itself.");

    // The case the window binding alone cannot see: same handle, same process, focus
    // moved from a document to a button.
    TargetBinding button = Binding(window, 100, "7.2");
    button.controlName = "Send";
    button.automationId = "sendButton";
    button.contextFingerprint = "Untitled|7.2|sendButton|50004|Send";
    const std::string drift = CompareBindings(document, button);
    Check(!drift.empty(), "Focus moving to another control in the same window was missed.");
    Check(drift.find("different control") != std::string::npos ||
        drift.find("context changed") != std::string::npos,
        "The drift reason did not describe a control change: " + drift);

    // A second window of the same program.
    const TargetBinding otherWindow = Binding(reinterpret_cast<void*>(0x2), 100, "7.1");
    Check(CompareBindings(document, otherWindow).find("different window") !=
        std::string::npos,
        "A second window of the same process was not noticed.");

    // A reused handle now owned by something else.
    const TargetBinding reused = Binding(window, 999, "7.1");
    Check(!CompareBindings(document, reused).empty(),
        "A handle belonging to a different process was accepted.");

    // A field becoming a password field is a change of consequence.
    TargetBinding secret = document;
    secret.isPassword = true;
    Check(!CompareBindings(document, secret).empty(),
        "A field turning into a password field was accepted as the same target.");

    TargetBinding gone;
    Check(CompareBindings(document, gone).find("no longer be observed") != std::string::npos,
        "A target that disappeared was not reported as unobservable.");
}

void TestRuntimeIdDecidesOnlyWhenBothSidesHaveOne()
{
    void* window = reinterpret_cast<void*>(0x1);
    const TargetBinding a = Binding(window, 100, "7.1");

    TargetBinding differentRuntimeId = a;
    differentRuntimeId.runtimeId = "7.9";
    Check(!a.Describes(differentRuntimeId),
        "Two different runtime ids were treated as the same control.");

    // Without runtime ids on both sides the rest of the evidence has to agree. Any one
    // field alone is weak, which is why it is the combination that decides.
    TargetBinding noIds = a;
    noIds.runtimeId.clear();
    TargetBinding otherNoIds = noIds;
    Check(noIds.Describes(otherNoIds),
        "Identical evidence without runtime ids was rejected.");

    otherNoIds.controlName = "Send";
    Check(!noIds.Describes(otherNoIds),
        "A changed control name was ignored when no runtime id was available.");

    otherNoIds = noIds;
    otherNoIds.controlType = 50000;
    Check(!noIds.Describes(otherNoIds),
        "A changed control type was ignored when no runtime id was available.");
}

void TestTwoUnlabelledFieldsAreToldApart()
{
    // Regression, and it was found the hard way: the native fixture put two unlabelled
    // EDIT controls in one window, focus moved between them mid-typing, and the
    // comparison said they were the same control. They had no name, no automation id and
    // the same control type, so every field the comparison looked at was equal --
    // "all empty" read as "unchanged".
    //
    // Position is the evidence that remains when the rest is blank.
    void* window = reinterpret_cast<void*>(0x1);
    TargetBinding first;
    first.valid = true;
    first.window = window;
    first.processId = 100;
    first.controlType = 50004;
    first.left = 20;
    first.top = 20;
    first.right = 340;
    first.bottom = 46;

    TargetBinding second = first;
    second.top = 56;
    second.bottom = 82;

    Check(!first.Describes(second),
        "Two unlabelled fields at different positions compared as the same control.");
    Check(first.Describes(first),
        "A binding stopped matching itself once position was compared.");
    Check(!CompareBindings(first, second).empty(),
        "Moving focus between two unlabelled fields produced no drift reason.");
}

} // namespace

void RunTargetBindingTests()
{
    TestSurrogatePairsSurviveSplitting();
    TestFreshnessIsMonotonicAndBounded();
    TestADifferentControlIsNoticedInsideOneWindow();
    TestRuntimeIdDecidesOnlyWhenBothSidesHaveOne();
    TestTwoUnlabelledFieldsAreToldApart();
    std::cout << "Target binding tests passed: the control, not just the window.\n";
}
