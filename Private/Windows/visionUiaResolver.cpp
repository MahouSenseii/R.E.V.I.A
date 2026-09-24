#include "Windows/visionUiaResolver.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <set>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <uiautomation.h>
#endif

namespace revia::actions::windows
{

namespace
{
std::string Normalize(const std::string& value)
{
    std::string normalized;
    bool lastWasSpace = true;
    for (const unsigned char character : value)
    {
        if (std::isalnum(character))
        {
            normalized.push_back(static_cast<char>(std::tolower(character)));
            lastWasSpace = false;
        }
        else if (!lastWasSpace)
        {
            normalized.push_back(' ');
            lastWasSpace = true;
        }
    }
    if (!normalized.empty() && normalized.back() == ' ')
    {
        normalized.pop_back();
    }
    return normalized;
}

std::set<std::string> Tokens(const std::string& value)
{
    std::set<std::string> tokens;
    std::istringstream stream(Normalize(value));
    for (std::string token; stream >> token;)
    {
        tokens.insert(std::move(token));
    }
    return tokens;
}

double NameAgreement(const std::string& wanted, const std::string& candidate)
{
    const std::string normalizedWanted = Normalize(wanted);
    const std::string normalizedCandidate = Normalize(candidate);
    if (normalizedWanted.empty() || normalizedCandidate.empty())
    {
        return 0.0;
    }
    if (normalizedWanted == normalizedCandidate)
    {
        return 1.0;
    }
    if (normalizedWanted.find(normalizedCandidate) != std::string::npos ||
        normalizedCandidate.find(normalizedWanted) != std::string::npos)
    {
        return 0.88;
    }

    const std::set<std::string> wantedTokens = Tokens(normalizedWanted);
    const std::set<std::string> candidateTokens = Tokens(normalizedCandidate);
    std::size_t intersection = 0;
    for (const std::string& token : wantedTokens)
    {
        intersection += candidateTokens.contains(token) ? 1U : 0U;
    }
    const std::size_t unionSize = wantedTokens.size() + candidateTokens.size() - intersection;
    return unionSize == 0 ? 0.0 : static_cast<double>(intersection) / unionSize;
}

double Area(const vision::ScreenRegion& region)
{
    return region.IsValid()
        ? static_cast<double>(region.right - region.left) *
            static_cast<double>(region.bottom - region.top)
        : 0.0;
}

double SpatialAgreement(
    const vision::ScreenRegion& wanted,
    const vision::ScreenRegion& candidate)
{
    const vision::ScreenRegion overlap{
        std::max(wanted.left, candidate.left),
        std::max(wanted.top, candidate.top),
        std::min(wanted.right, candidate.right),
        std::min(wanted.bottom, candidate.bottom)};
    const double intersection = Area(overlap);
    const double wantedArea = Area(wanted);
    const double candidateArea = Area(candidate);
    if (intersection <= 0.0 || wantedArea <= 0.0 || candidateArea <= 0.0)
    {
        return 0.0;
    }
    const double overlapOfSmaller = intersection / std::min(wantedArea, candidateArea);
    const double unionArea = wantedArea + candidateArea - intersection;
    const double intersectionOverUnion = unionArea > 0.0 ? intersection / unionArea : 0.0;
    return std::clamp(0.7 * overlapOfSmaller + 0.3 * intersectionOverUnion, 0.0, 1.0);
}

#ifdef _WIN32
template <typename T>
void Release(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}

std::wstring Lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character)
    {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0)
    {
        return {};
    }
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), count, nullptr, nullptr);
    return output;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0)
    {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), count);
    return output;
}

std::wstring BstrProperty(
    IUIAutomationElement* element,
    HRESULT (STDMETHODCALLTYPE IUIAutomationElement::*getter)(BSTR*))
{
    BSTR value = nullptr;
    if (FAILED((element->*getter)(&value)) || value == nullptr)
    {
        return {};
    }
    std::wstring output(value, SysStringLen(value));
    SysFreeString(value);
    return output;
}

std::wstring ProcessFileName(const int processId)
{
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        static_cast<DWORD>(processId));
    if (process == nullptr)
    {
        return {};
    }
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const bool queried = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (!queried)
    {
        return {};
    }
    path.resize(length);
    return std::filesystem::path(path).filename().wstring();
}

std::string RuntimeId(IUIAutomationElement* element)
{
    SAFEARRAY* values = nullptr;
    if (FAILED(element->GetRuntimeId(&values)) || values == nullptr)
    {
        return {};
    }
    LONG lower = 0;
    LONG upper = -1;
    if (FAILED(SafeArrayGetLBound(values, 1, &lower)) ||
        FAILED(SafeArrayGetUBound(values, 1, &upper)))
    {
        SafeArrayDestroy(values);
        return {};
    }
    std::ostringstream stream;
    for (LONG index = lower; index <= upper; ++index)
    {
        int value = 0;
        if (FAILED(SafeArrayGetElement(values, &index, &value)))
        {
            SafeArrayDestroy(values);
            return {};
        }
        if (index > lower)
        {
            stream << '.';
        }
        stream << value;
    }
    SafeArrayDestroy(values);
    return stream.str();
}

bool SupportsPattern(IUIAutomationElement* element, const PATTERNID patternId)
{
    IUnknown* pattern = nullptr;
    const bool supported = SUCCEEDED(element->GetCurrentPattern(patternId, &pattern)) &&
        pattern != nullptr;
    Release(pattern);
    return supported;
}

IUIAutomationElement* FindApplicationWindow(
    IUIAutomation* automation,
    const std::string& application,
    const std::string& windowTitle)
{
    IUIAutomationElement* root = nullptr;
    IUIAutomationCondition* condition = nullptr;
    IUIAutomationElementArray* windows = nullptr;
    if (FAILED(automation->GetRootElement(&root)) || root == nullptr ||
        FAILED(automation->CreateTrueCondition(&condition)) || condition == nullptr ||
        FAILED(root->FindAll(TreeScope_Children, condition, &windows)) || windows == nullptr)
    {
        Release(windows);
        Release(condition);
        Release(root);
        return nullptr;
    }

    const std::wstring wantedApplication = Lower(Utf8ToWide(application));
    const std::wstring wantedTitle = Lower(Utf8ToWide(windowTitle));
    IUIAutomationElement* match = nullptr;
    int count = 0;
    windows->get_Length(&count);
    for (int index = 0; index < count; ++index)
    {
        IUIAutomationElement* candidate = nullptr;
        if (FAILED(windows->GetElement(index, &candidate)) || candidate == nullptr)
        {
            continue;
        }
        int processId = 0;
        candidate->get_CurrentProcessId(&processId);
        const std::wstring title = Lower(BstrProperty(
            candidate,
            &IUIAutomationElement::get_CurrentName));
        if (Lower(ProcessFileName(processId)) == wantedApplication &&
            (wantedTitle.empty() || title.find(wantedTitle) != std::wstring::npos))
        {
            match = candidate;
            break;
        }
        candidate->Release();
    }
    Release(windows);
    Release(condition);
    Release(root);
    return match;
}
#endif
}

vision::CandidateScore VisionUiaResolver::ScoreCandidate(
    const vision::VisionActionIntent& intent,
    const vision::UiaCandidate& candidate)
{
    vision::CandidateScore score;
    score.spatial = SpatialAgreement(intent.region, candidate.bounds);
    score.nameAgreement = std::max(
        NameAgreement(intent.targetName, candidate.name),
        NameAgreement(intent.targetName, candidate.automationId));
    score.total = 0.55 * score.spatial + 0.35 * score.nameAgreement +
        0.10 * std::clamp(intent.modelConfidence, 0.0, 1.0);
    return score;
}

vision::UiaResolutionResult VisionUiaResolver::SelectBest(
    const std::string& application,
    const std::string& windowTitle,
    const vision::VisionActionIntent& intent,
    const std::vector<vision::UiaCandidate>& candidates,
    const VisionResolverSettings& settings)
{
    vision::UiaResolutionResult result;
    result.candidatesInspected = static_cast<int>(candidates.size());
    struct Ranked
    {
        const vision::UiaCandidate* candidate = nullptr;
        vision::CandidateScore score;
    };
    std::vector<Ranked> ranked;
    for (const vision::UiaCandidate& candidate : candidates)
    {
        const bool supportsAction = intent.action == actions::ActionType::InvokeControl
            ? candidate.supportsInvoke
            : candidate.supportsValue;
        if (!candidate.enabled || candidate.offscreen || !candidate.bounds.IsValid() ||
            candidate.runtimeId.empty() || !supportsAction)
        {
            continue;
        }
        const vision::CandidateScore score = ScoreCandidate(intent, candidate);
        if (score.spatial <= 0.0 || score.nameAgreement < settings.minimumNameAgreement)
        {
            continue;
        }
        ranked.push_back({&candidate, score});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& left, const Ranked& right)
    {
        return left.score.total > right.score.total;
    });
    if (ranked.empty())
    {
        result.reason = "No enabled UI Automation element overlapped the vision region "
            "with sufficient accessible-name agreement.";
        return result;
    }
    if (ranked.front().score.total < settings.minimumConfidence)
    {
        std::ostringstream reason;
        reason << "The best UI Automation match scored " << ranked.front().score.total
            << ", below the required " << settings.minimumConfidence << ".";
        result.reason = reason.str();
        return result;
    }
    if (ranked.size() > 1 &&
        ranked.front().score.total - ranked[1].score.total < settings.ambiguityMargin)
    {
        result.reason = "Two UI Automation elements matched too closely; refusing an "
            "ambiguous screen action.";
        return result;
    }

    result.succeeded = true;
    result.reference.application = application;
    result.reference.windowTitle = windowTitle;
    result.reference.element = *ranked.front().candidate;
    result.reference.modelTarget = intent.targetName;
    result.reference.modelRegion = intent.region;
    result.reference.modelConfidence = intent.modelConfidence;
    result.reference.score = ranked.front().score;
    std::ostringstream reason;
    reason << "Resolved vision target '" << intent.targetName << "' to UI Automation element '"
        << result.reference.element.name << "' at confidence "
        << result.reference.score.total << ".";
    result.reason = reason.str();
    return result;
}

vision::UiaResolutionResult VisionUiaResolver::Resolve(
    const std::string& application,
    const std::string& windowTitle,
    const vision::VisionActionIntent& intent,
    const VisionResolverSettings& settings) const
{
    vision::UiaResolutionResult result;
#ifdef _WIN32
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = initialized == S_OK || initialized == S_FALSE;
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    {
        result.reason = "COM could not initialize for vision-to-UIA resolution.";
        return result;
    }

    IUIAutomation* automation = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_CUIAutomation,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IUIAutomation,
            reinterpret_cast<void**>(&automation))) || automation == nullptr)
    {
        result.reason = "Windows UI Automation is unavailable for vision resolution.";
        if (shouldUninitialize) CoUninitialize();
        return result;
    }
    IUIAutomationElement* window = FindApplicationWindow(automation, application, windowTitle);
    if (window == nullptr)
    {
        result.reason = "The foreground application window is no longer available to UI Automation.";
        Release(automation);
        if (shouldUninitialize) CoUninitialize();
        return result;
    }

    IUIAutomationCondition* condition = nullptr;
    IUIAutomationElementArray* elements = nullptr;
    if (FAILED(automation->CreateTrueCondition(&condition)) || condition == nullptr ||
        FAILED(window->FindAll(TreeScope_Descendants, condition, &elements)) || elements == nullptr)
    {
        result.reason = "Windows UI Automation could not enumerate the target window.";
        Release(elements);
        Release(condition);
        Release(window);
        Release(automation);
        if (shouldUninitialize) CoUninitialize();
        return result;
    }

    int count = 0;
    elements->get_Length(&count);
    count = std::min(count, settings.maxCandidates);
    std::vector<vision::UiaCandidate> candidates;
    candidates.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int index = 0; index < count; ++index)
    {
        IUIAutomationElement* element = nullptr;
        if (FAILED(elements->GetElement(index, &element)) || element == nullptr)
        {
            continue;
        }
        vision::UiaCandidate candidate;
        candidate.name = WideToUtf8(BstrProperty(element, &IUIAutomationElement::get_CurrentName));
        candidate.automationId = WideToUtf8(BstrProperty(
            element,
            &IUIAutomationElement::get_CurrentAutomationId));
        candidate.runtimeId = RuntimeId(element);
        RECT bounds{};
        BOOL enabled = FALSE;
        BOOL offscreen = TRUE;
        element->get_CurrentBoundingRectangle(&bounds);
        element->get_CurrentIsEnabled(&enabled);
        element->get_CurrentIsOffscreen(&offscreen);
        element->get_CurrentControlType(&candidate.controlType);
        candidate.bounds = {bounds.left, bounds.top, bounds.right, bounds.bottom};
        candidate.enabled = enabled != FALSE;
        candidate.offscreen = offscreen != FALSE;
        candidate.supportsInvoke = SupportsPattern(element, UIA_InvokePatternId);
        candidate.supportsValue = SupportsPattern(element, UIA_ValuePatternId);
        candidates.push_back(std::move(candidate));
        Release(element);
    }

    result = SelectBest(application, windowTitle, intent, candidates, settings);
    Release(elements);
    Release(condition);
    Release(window);
    Release(automation);
    if (shouldUninitialize) CoUninitialize();
    return result;
#else
    (void)application;
    (void)windowTitle;
    (void)intent;
    (void)settings;
    result.reason = "Vision-to-UIA resolution is available on Windows only.";
    return result;
#endif
}

} // namespace revia::actions::windows
