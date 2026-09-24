#pragma once

#ifdef _WIN32

#include "Actions/actionTypes.h"

#include <string>

struct IUIAutomation;
struct IUIAutomationElement;

namespace revia::actions::windows
{

// One owner for "which window, and which element, does this typed request mean".
//
// UI Automation lookup used to live inside WindowsAutomationExecutor. Desktop operation
// needs exactly the same lookup -- a click has to re-find the element it was authorized
// against before it is allowed to aim at anything -- and a second copy of a security
// check is a second place for it to drift.
//
// Every returned element is an owning reference: the caller releases it. The functions
// expect the caller to have already created the IUIAutomation instance.

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
[[nodiscard]] IUIAutomationElement* FindApplicationWindow(
    IUIAutomation* automation,
    const ActionRequest& request);

// The exact element a vision resolution referred to. Deliberately has no fallback: if
// the runtime id, name, automation id, or control type no longer agree, the element the
// user confirmed is gone and refusing is the safe answer.
[[nodiscard]] IUIAutomationElement* FindResolvedControl(
    IUIAutomation* automation,
    IUIAutomationElement* window,
    const ActionRequest& request);

// A resolved element when the request carries one, otherwise a control matched by
// accessible name or automation id.
[[nodiscard]] IUIAutomationElement* FindControl(
    IUIAutomation* automation,
    IUIAutomationElement* window,
    const ActionRequest& request);

// What is under a screen point: the executable that owns it, and the accessible name
// and control type of the element there.
//
// This is how a pointer skill becomes something other than clicking in the dark. It is
// read-only and grants nothing -- describing a control is not permission to press it,
// and the text it returns is treated as untrusted the same way screen text is.
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
