#pragma once

#ifdef _WIN32

#include "Actions/actionTypes.h"

#include <string>

struct IUIAutomation;
struct IUIAutomationElement;

namespace revia::actions::windows
{

// Shared UIA lookup requires a caller-created IUIAutomation instance.
// Returned element references are owned and must be released by the caller.

struct ElementBounds
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool valid = false;
};

[[nodiscard]] std::wstring Utf8ToWide(const std::string& value);
[[nodiscard]] std::string WideToUtf8(const std::wstring& value);

[[nodiscard]] std::wstring ElementName(IUIAutomationElement* element);
[[nodiscard]] std::wstring ElementAutomationId(IUIAutomationElement* element);
[[nodiscard]] std::string ElementRuntimeId(IUIAutomationElement* element);
[[nodiscard]] ElementBounds ElementBoundingRectangle(IUIAutomationElement* element);
[[nodiscard]] std::wstring ProcessFileName(int processId);

// The top-level window of the requested executable, matched by process image name and,
// when one is given, by a case-insensitive window-title substring.
[[nodiscard]] IUIAutomationElement* FindApplicationWindow(IUIAutomation* automation, const ActionRequest& request);

// The exact element a vision resolution referred to. Deliberately has no fallback: if
// the runtime id, name, automation id, or control type no longer agree, the element the
// user confirmed is gone and refusing is the safe answer.
[[nodiscard]] IUIAutomationElement* FindResolvedControl(IUIAutomation* automation,
    IUIAutomationElement* window, const ActionRequest& request);

// A resolved element when the request carries one, otherwise a control matched by
// accessible name or automation id.
[[nodiscard]] IUIAutomationElement* FindControl(IUIAutomation* automation, IUIAutomationElement* window, const ActionRequest& request);

// Read-only point description grants no permission; returned application text is untrusted.
struct PointDescription
{
    std::string executable;
    std::string elementName;
    int controlType = 0;
    // From UI Automation rather than from the name. It is the one thing about a control
    // that says "this is a secret" without anyone having to guess from a label.
    bool isPassword = false;
    bool found = false;

    // A short line for an action result: "Save button in notepad.exe".
    [[nodiscard]] std::string Summary() const;
};

[[nodiscard]] PointDescription DescribePoint(IUIAutomation* automation, int x, int y);

// The control that will receive typing. Same read, different question: a click asks
// "what is under the pointer", a keystroke asks "what has the caret".
[[nodiscard]] PointDescription DescribeFocusedElement(IUIAutomation* automation);

} // namespace revia::actions::windows

#endif
