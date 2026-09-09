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

} // namespace revia::actions::windows

#endif
