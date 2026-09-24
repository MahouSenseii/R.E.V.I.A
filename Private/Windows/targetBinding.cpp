#include "Windows/targetBinding.h"

#include "Actions/actionTypes.h"
#include "Policy/desktopAuthorization.h"

#include <algorithm>
#include <vector>

#ifdef _WIN32
#include "Windows/uiaElementLocator.h"

#include <windows.h>
#include <uiautomation.h>
#endif

namespace revia::actions::windows
{

bool TargetBinding::Describes(const TargetBinding& other) const
{
    // Window first: the cheap check, and the one that catches a switch to another
    // program or another document of the same program.
    if (window != other.window || processId != other.processId)
    {
        return false;
    }
    // Then the control. A runtime id is the strongest short-lived signal, so when both
    // sides have one it decides.
    if (!runtimeId.empty() && !other.runtimeId.empty())
    {
        return runtimeId == other.runtimeId;
    }
    // Without one, agreement has to come from the rest of the evidence together --
    // including where the control is.
    //
    // Position matters more than it looks. Two unlabelled text fields in one window have
    // no name, no automation id and the same control type, so every other field here
    // compares equal and "all empty" reads as "the same control". That is how focus
    // moving between two fields went unnoticed until a native test put two of them in
    // one window. Where a control sits is evidence, and it is the only evidence left
    // when the rest is blank.
    return automationId == other.automationId && controlType == other.controlType &&
        controlName == other.controlName && isPassword == other.isPassword &&
        left == other.left && top == other.top &&
        right == other.right && bottom == other.bottom;
}

bool SameControl(const TargetBinding& intended, const TargetBinding& current)
{
    if (!intended.valid || !current.valid)
    {
        return false;
    }
    if (intended.window != current.window || intended.processId != current.processId)
    {
        return false;
    }
    if (!intended.runtimeId.empty() && intended.runtimeId == current.runtimeId)
    {
        return true;
    }
    // Deliberately demands all of it, including the rectangle. With the id gone this is
    // the whole of the evidence, and any one of these differing means a different
    // control.
    return intended.automationId == current.automationId &&
        intended.controlType == current.controlType &&
        intended.controlName == current.controlName &&
        intended.isPassword == current.isPassword &&
        intended.left == current.left && intended.top == current.top &&
        intended.right == current.right && intended.bottom == current.bottom;
}

bool IsFresh(
    const TargetBinding& binding,
    const std::chrono::steady_clock::time_point now,
    const std::chrono::milliseconds limit)
{
    if (!binding.valid)
    {
        return false;
    }
    // Monotonic. Wall-clock would let a clock adjustment extend a binding's life.
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - binding.observedAt);
    return age >= std::chrono::milliseconds::zero() && age <= limit;
}

std::string CompareBindings(const TargetBinding& authorized, const TargetBinding& current)
{
    if (!authorized.valid)
    {
        return "the authorized target was never observed";
    }
    if (!current.valid)
    {
        return "the target can no longer be observed";
    }
    if (authorized.window != current.window)
    {
        return "a different window now has the target";
    }
    if (authorized.processId != current.processId)
    {
        return "the window now belongs to a different process";
    }
    if (authorized.taskOrigin != current.taskOrigin ||
        authorized.policyVersion != current.policyVersion)
    {
        return "the task or policy that authorized the target changed";
    }
    if (authorized.automationId != current.automationId ||
        authorized.controlName != current.controlName ||
        authorized.controlType != current.controlType)
    {
        return "a different control identity or meaning is now at the target";
    }
    if (authorized.contextFingerprint != current.contextFingerprint)
    {
        return "the window or dialog context changed";
    }
    if (!authorized.Describes(current))
    {
        return "a different control is now in that position";
    }
    if (authorized.isPassword != current.isPassword)
    {
        return "the field changed to or from a password field";
    }
    return {};
}

std::string CompareDraftSnapshots(const DraftSnapshot& authorized,
    const DraftSnapshot& current, const std::string& expectedValue)
{
    if (!authorized.valid || !current.valid)
        return "the draft or its surrounding fields could not be read";
    const std::string drift = CompareBindings(authorized.control, current.control);
    if (!drift.empty()) return drift;
    if (authorized.context != current.context)
        return "the draft recipient or surrounding context changed";
    if (current.value != expectedValue)
        return "the draft no longer contains the exact prepared content";
    return {};
}

std::optional<std::string> BuildDraftContext(const std::vector<DraftContextNode>& nodes)
{
    std::vector<std::string> rows;
    for (const auto& node : nodes)
    {
        if (node.insideDraft || node.isPassword || node.isStatus) continue;
        if (!node.ancestorOfDraft && node.hasValue && !node.valueKnown) return std::nullopt;
        std::string row;
        const auto append = [&](const std::string& value) {
            row += std::to_string(value.size()) + ":" + value;
        };
        append(node.runtimeId);
        append(node.automationId);
        append(std::to_string(node.controlType));
        append(node.name);
        if (!node.ancestorOfDraft && node.hasValue) append(node.value);
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end());
    std::string context;
    for (const auto& row : rows) context += std::to_string(row.size()) + ":" + row;
    return context;
}

std::string ValidateActionTarget(const ActionRequest& request, const TargetBinding& current)
{
    if (request.requiresRuntimeGuard && !request.beforeCommit && !request.navigationConstraint.enabled)
        return "the saved action needs fresh runtime validation before it can resume";
    const auto& navigation = request.navigationConstraint;
    if (!navigation.enabled) return {};
    if (!current.valid || current.isPassword || current.controlName != navigation.controlName ||
        (!navigation.windowTitle.empty() && current.windowTitle != navigation.windowTitle))
        return "the navigation target changed after it was observed";
    // A field-focus click remains a field-focus click only while its actual role is
    // still Edit or Document. Labels such as Send a message do not make it a button.
    if (navigation.editable)
        return current.controlType == 50004 || current.controlType == 50030
            ? std::string{} : "the field-focus target is no longer an editable control";
    policy::AuthorizationRequest effect;
    effect.operation = policy::DesktopOperation::Invoke;
    effect.evidence.resolved = true;
    effect.evidence.controlName = current.controlName;
    effect.evidence.windowTitle = current.windowTitle;
    effect.evidence.isPassword = current.isPassword;
    if (policy::AssessEffects(effect) != 0u ||
        policy::AssessEvidence(effect) != policy::EvidenceQuality::Verified)
        return "the navigation target now has a committing or unresolved consequence";
    return {};
}

#ifdef _WIN32

namespace
{

std::string Fingerprint(const TargetBinding& binding, const std::string& windowTitle)
{
    // Deliberately includes the control identity: a dialog opening over the target
    // changes what "here" means even when the handle underneath is the same.
    return windowTitle + '|' + binding.runtimeId + '|' + binding.automationId + '|' +
        std::to_string(binding.controlType) + '|' + binding.controlName;
}

TargetBinding FromElement(
    IUIAutomationElement* element,
    const HWND window,
    const std::string& windowTitle,
    const std::string& taskOrigin,
    const std::string& policyVersion,
    const bool focused)
{
    TargetBinding binding;
    binding.id = NewActionId();
    binding.observedAt = std::chrono::steady_clock::now();
    binding.taskOrigin = taskOrigin;
    binding.policyVersion = policyVersion;
    binding.wasFocused = focused;
    binding.windowTitle = windowTitle;

    if (window != nullptr)
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        binding.window = window;
        binding.processId = static_cast<std::uint32_t>(processId);
        binding.executable = WideToUtf8(ProcessFileName(static_cast<int>(processId)));
    }

    if (element != nullptr)
    {
        CONTROLTYPEID controlType = 0;
        BOOL isPassword = FALSE;
        element->get_CurrentControlType(&controlType);
        element->get_CurrentIsPassword(&isPassword);
        binding.runtimeId = ElementRuntimeId(element);
        binding.automationId = WideToUtf8(ElementAutomationId(element));
        binding.controlName = WideToUtf8(ElementName(element));
        binding.controlType = static_cast<int>(controlType);
        binding.isPassword = isPassword != FALSE;
        const ElementBounds bounds = ElementBoundingRectangle(element);
        binding.left = bounds.left;
        binding.top = bounds.top;
        binding.right = bounds.right;
        binding.bottom = bounds.bottom;
    }

    binding.contextFingerprint = Fingerprint(binding, windowTitle);
    // A binding needs a window to be about. Without one there is nothing to compare
    // against later, which is not a weak binding but an absent one.
    binding.valid = binding.window != nullptr && element != nullptr &&
        binding.controlType != 0;
    return binding;
}

std::string TitleOf(const HWND window)
{
    if (window == nullptr)
    {
        return {};
    }
    wchar_t title[512]{};
    const int length = GetWindowTextW(window, title, 511);
    return length > 0 ? WideToUtf8(std::wstring(title, static_cast<std::size_t>(length)))
                      : std::string{};
}

} // namespace

TargetBinding BindElement(IUIAutomationElement* element, IUIAutomationElement* window,
    const std::string& taskOrigin, const std::string& policyVersion)
{
    UIA_HWND handle = nullptr;
    if (window != nullptr) window->get_CurrentNativeWindowHandle(&handle);
    return FromElement(element, reinterpret_cast<HWND>(handle),
        WideToUtf8(ElementName(window)), taskOrigin, policyVersion, false);
}

namespace
{

bool ReadDraftValue(IUIAutomationElement* element, std::string& output,
    bool* writable = nullptr)
{
    constexpr UINT MaximumCharacters = 65536;
    BOOL password = TRUE;
    if (FAILED(element->get_CurrentIsPassword(&password)) || password) return false;
    IUIAutomationValuePattern* value = nullptr;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId,
            IID_IUIAutomationValuePattern, reinterpret_cast<void**>(&value))) && value)
    {
        BOOL readOnly = TRUE;
        BSTR text = nullptr;
        const bool read = SUCCEEDED(value->get_CurrentIsReadOnly(&readOnly)) &&
            SUCCEEDED(value->get_CurrentValue(&text)) && SysStringLen(text) <= MaximumCharacters;
        if (read) output = WideToUtf8(std::wstring(text ? text : L"", SysStringLen(text)));
        if (writable != nullptr) *writable = read && !readOnly;
        SysFreeString(text);
        value->Release();
        return read;
    }
    IUIAutomationTextPattern* pattern = nullptr;
    IUIAutomationTextRange* range = nullptr;
    BSTR text = nullptr;
    bool read = SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPatternId,
        IID_IUIAutomationTextPattern, reinterpret_cast<void**>(&pattern))) && pattern &&
        SUCCEEDED(pattern->get_DocumentRange(&range)) && range &&
        SUCCEEDED(range->GetText(MaximumCharacters + 1, &text)) &&
        SysStringLen(text) <= MaximumCharacters;
    if (read) output = WideToUtf8(std::wstring(text ? text : L"", SysStringLen(text)));
    if (writable != nullptr) *writable = read;
    SysFreeString(text);
    if (range) range->Release();
    if (pattern) pattern->Release();
    return read;
}

DraftSnapshot ReadDraft(IUIAutomation* automation, IUIAutomationElement* window,
    IUIAutomationElement* field, const ActionRequest& placement)
{
    DraftSnapshot snapshot;
    snapshot.control = BindElement(field, window, placement.requestedBy, "draft-v1");
    if (!snapshot.control.valid || !ReadDraftValue(field, snapshot.value)) return snapshot;
    IUIAutomationCondition* condition = nullptr;
    IUIAutomationElementArray* elements = nullptr;
    if (FAILED(automation->CreateTrueCondition(&condition)) || !condition) return snapshot;
    const HRESULT found = window->FindAll(TreeScope_Descendants, condition, &elements);
    condition->Release();
    if (FAILED(found) || !elements) return snapshot;
    int count = 0;
    bool complete = SUCCEEDED(elements->get_Length(&count)) && count <= 512;
    IUIAutomationTreeWalker* walker = nullptr;
    complete = complete && SUCCEEDED(automation->get_ControlViewWalker(&walker)) && walker;
    std::vector<IUIAutomationElement*> ancestors;
    IUIAutomationElement* ancestor = field;
    ancestor->AddRef();
    bool reachedWindow = false;
    for (int depth = 0; complete && depth < 32; ++depth)
    {
        IUIAutomationElement* parent = nullptr;
        const HRESULT read = walker->GetParentElement(ancestor, &parent);
        ancestor->Release();
        ancestor = parent;
        if (FAILED(read) || !parent) { complete = false; break; }
        BOOL root = FALSE;
        complete = SUCCEEDED(automation->CompareElements(parent, window, &root));
        if (root) { reachedWindow = true; break; }
        parent->AddRef();
        ancestors.push_back(parent);
    }
    if (ancestor) ancestor->Release();
    complete = complete && reachedWindow;
    std::vector<DraftContextNode> nodes;
    for (int index = 0; complete && index < count; ++index)
    {
        IUIAutomationElement* element = nullptr;
        if (FAILED(elements->GetElement(index, &element)) || !element)
        {
            complete = false;
            break;
        }
        DraftContextNode node;
        BOOL password = TRUE;
        CONTROLTYPEID type = 0;
        complete = SUCCEEDED(element->get_CurrentIsPassword(&password)) &&
            SUCCEEDED(element->get_CurrentControlType(&type));
        node.controlType = type;
        node.isPassword = password != FALSE;
        node.isStatus = type == UIA_StatusBarControlTypeId;
        for (auto* parent : ancestors)
        {
            BOOL same = FALSE;
            complete = complete && SUCCEEDED(automation->CompareElements(parent, element, &same));
            if (same) { node.ancestorOfDraft = true; break; }
        }
        // Exclude the complete draft subtree, not only its editable root. Rich editors
        // publish body text through descendants and aggregate Document ancestors.
        IUIAutomationElement* cursor = element;
        cursor->AddRef();
        bool reachedBoundary = false;
        for (int depth = 0; complete && depth < 32; ++depth)
        {
            BOOL body = FALSE;
            BOOL root = FALSE;
            CONTROLTYPEID ancestorType = 0;
            complete = SUCCEEDED(automation->CompareElements(cursor, field, &body)) &&
                SUCCEEDED(automation->CompareElements(cursor, window, &root)) &&
                SUCCEEDED(cursor->get_CurrentControlType(&ancestorType));
            if (ancestorType == UIA_StatusBarControlTypeId) node.isStatus = true;
            if (body || root)
            {
                node.insideDraft = body != FALSE;
                reachedBoundary = true;
                break;
            }
            IUIAutomationElement* parent = nullptr;
            const HRESULT read = walker->GetParentElement(cursor, &parent);
            cursor->Release();
            cursor = parent;
            if (FAILED(read) || !parent) { complete = false; break; }
        }
        if (cursor) cursor->Release();
        complete = complete && reachedBoundary;
        if (complete && !node.insideDraft && !node.isPassword && !node.isStatus)
        {
            node.runtimeId = ElementRuntimeId(element);
            node.automationId = WideToUtf8(ElementAutomationId(element));
            node.name = WideToUtf8(ElementName(element));
            // Ancestors contribute identity and label, never aggregate body text.
            // Read-only siblings still matter: recipients are often read-only.
            node.hasValue = !node.ancestorOfDraft && (type == UIA_EditControlTypeId ||
                type == UIA_DocumentControlTypeId || type == UIA_ComboBoxControlTypeId);
            if (node.hasValue) node.valueKnown = ReadDraftValue(element, node.value);
            if (type == UIA_ListItemControlTypeId)
            {
                IUIAutomationSelectionItemPattern* selection = nullptr;
                if (SUCCEEDED(element->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                    IID_IUIAutomationSelectionItemPattern, reinterpret_cast<void**>(&selection))) && selection)
                {
                    BOOL selected = FALSE;
                    node.hasValue = true;
                    node.valueKnown = SUCCEEDED(selection->get_CurrentIsSelected(&selected));
                    node.value = selected ? "selected" : "not selected";
                    selection->Release();
                }
            }
            nodes.push_back(std::move(node));
        }
        element->Release();
    }
    for (auto* parent : ancestors) parent->Release();
    if (walker) walker->Release();
    elements->Release();
    if (!complete) return snapshot;
    const auto context = BuildDraftContext(nodes);
    if (!context) return snapshot;
    snapshot.context = *context;
    snapshot.valid = true;
    return snapshot;
}
} // namespace

DraftSnapshot ObserveDraft(const ActionRequest& placement)
{
    DraftSnapshot snapshot;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = initialized == S_OK || initialized == S_FALSE;
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return snapshot;
    IUIAutomation* automation = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_IUIAutomation, reinterpret_cast<void**>(&automation))) && automation)
    {
        IUIAutomationElement* window = FindApplicationWindow(automation, placement);
        IUIAutomationElement* field = window ? FindControl(automation, window, placement) : nullptr;
        if (field) snapshot = ReadDraft(automation, window, field, placement);
        if (field) field->Release();
        if (window) window->Release();
        automation->Release();
    }
    if (uninitialize) CoUninitialize();
    return snapshot;
}

TargetBinding BindFocusedControl(
    IUIAutomation* automation,
    const std::string& taskOrigin,
    const std::string& policyVersion)
{
    const HWND foreground = GetForegroundWindow();
    IUIAutomationElement* element = nullptr;
    if (automation != nullptr)
    {
        automation->GetFocusedElement(&element);
    }
    TargetBinding binding = FromElement(
        element, foreground, TitleOf(foreground), taskOrigin, policyVersion, true);
    if (element != nullptr) element->Release();
    return binding;
}

TargetBinding BindPoint(
    IUIAutomation* automation,
    const int x,
    const int y,
    const std::string& taskOrigin,
    const std::string& policyVersion)
{
    const POINT point{static_cast<LONG>(x), static_cast<LONG>(y)};
    const HWND hit = WindowFromPoint(point);
    const HWND window = hit == nullptr ? nullptr : GetAncestor(hit, GA_ROOT);
    IUIAutomationElement* element = nullptr;
    if (automation != nullptr)
    {
        automation->ElementFromPoint(point, &element);
    }
    TargetBinding binding = FromElement(
        element, window, TitleOf(window), taskOrigin, policyVersion, false);
    if (element != nullptr) element->Release();
    return binding;
}

std::string RevalidateFocusBinding(
    IUIAutomation* automation,
    const TargetBinding& authorized,
    const std::chrono::steady_clock::time_point now)
{
    if (!IsFresh(authorized, now))
    {
        return "the observation this was authorized against has expired";
    }
    const TargetBinding current =
        BindFocusedControl(automation, authorized.taskOrigin, authorized.policyVersion);
    return CompareBindings(authorized, current);
}

std::string RevalidatePointBinding(
    IUIAutomation* automation,
    const TargetBinding& authorized,
    const int x,
    const int y,
    const std::chrono::steady_clock::time_point now)
{
    if (!IsFresh(authorized, now))
    {
        return "the observation this was authorized against has expired";
    }
    const TargetBinding current = BindPoint(
        automation, x, y, authorized.taskOrigin, authorized.policyVersion);
    return CompareBindings(authorized, current);
}

#else
DraftSnapshot ObserveDraft(const ActionRequest&) { return {}; }
#endif // _WIN32

} // namespace revia::actions::windows
