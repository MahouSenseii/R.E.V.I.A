// A disposable Windows application for testing Revia's desktop safeguards.
//
// It exists so the native tests never touch a real editor, browser, mailbox, or payment
// page. Every "consequence" here is a line appended to a log file: nothing is sent,
// bought, deleted, or changed outside this process.
//
// It is deliberately built from plain Win32 controls, because those are what UI
// Automation reads most predictably, and the point is to test Revia's reading of a
// real accessibility tree rather than a simulation of one.
//
// Usage:
//   desktopFixture.exe --log <path>
// The log is the test's window into what actually arrived.

#include <windows.h>
#include <commctrl.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace
{

std::wstring logPath;
std::mutex logMutex;

HWND mainWindow = nullptr;
HWND secondWindow = nullptr;
HWND documentField = nullptr;
HWND secondField = nullptr;
HWND passwordField = nullptr;
HWND vanishingButton = nullptr;
HWND shiftyButton = nullptr;

// Control ids. The names are what UI Automation reports, so they are also what the
// consequence classifier will read.
constexpr int IdHarmless        = 1001;
constexpr int IdDocumentField   = 1002;
constexpr int IdSecondField     = 1003;
constexpr int IdPasswordField   = 1004;
constexpr int IdSave            = 1005;
constexpr int IdSend            = 1006;
constexpr int IdDelete          = 1007;
constexpr int IdBuy             = 1008;
constexpr int IdAmbiguous       = 1009;
constexpr int IdOpenDeleteAcct  = 1010;
constexpr int IdOpenPrefs       = 1011;
constexpr int IdVanishing       = 1012;
constexpr int IdShifty          = 1013;
constexpr int IdDragSource      = 1014;
constexpr int IdSafeDrop        = 1015;
constexpr int IdDangerousDrop   = 1016;

void Record(const std::string& line)
{
    std::lock_guard lock(logMutex);
    if (logPath.empty()) return;
    // std::filesystem::path rather than the wide string directly: libstdc++
    // does not take a wstring here, and the path overload is portable.
    std::ofstream file(std::filesystem::path(logPath), std::ios::app);
    file << line << "\n";
}

std::string Utf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

std::string TextOf(HWND control)
{
    wchar_t buffer[1024]{};
    const int length = GetWindowTextW(control, buffer, 1023);
    return length > 0 ? Utf8(std::wstring(buffer, static_cast<std::size_t>(length)))
                      : std::string{};
}

// The simulated consequences. Each one only writes a line: the whole point of the
// fixture is that "Send" and "Buy" are safe to press ten thousand times.
void SimulateEffect(const char* effect, const std::string& detail)
{
    Record(std::string("EFFECT ") + effect + " " + detail);
}

// A modal-style dialog whose window title supplies the meaning its Confirm button
// withholds. Two of these exist so the same label can be tested in a dangerous context
// and a harmless one.
INT_PTR CALLBACK ConfirmDialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM)
{
    if (message == WM_COMMAND && LOWORD(wparam) == IDOK)
    {
        wchar_t title[256]{};
        GetWindowTextW(dialog, title, 255);
        SimulateEffect("dialog_confirmed", Utf8(title));
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)
    {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void ShowConfirmWindow(const wchar_t* title)
{
    // A real top-level window rather than a message box, so it has a stable class and
    // its Confirm button is an ordinary UIA element.
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"ReviaFixtureDialog", title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        420, 260, 320, 160, mainWindow, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (dialog == nullptr) return;
    CreateWindowExW(0, L"BUTTON", L"Confirm",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 40, 70, 100, 30,
        dialog, reinterpret_cast<HMENU>(IDOK), GetModuleHandleW(nullptr), nullptr);
    CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 160, 70, 100, 30,
        dialog, reinterpret_cast<HMENU>(IDCANCEL), GetModuleHandleW(nullptr), nullptr);
    Record(std::string("DIALOG_OPENED ") + Utf8(title));
}

LRESULT CALLBACK DialogWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_COMMAND)
    {
        const int id = LOWORD(wparam);
        if (id == IDOK || id == IDCANCEL)
        {
            wchar_t title[256]{};
            GetWindowTextW(window, title, 255);
            if (id == IDOK)
            {
                SimulateEffect("dialog_confirmed", Utf8(title));
            }
            DestroyWindow(window);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_COMMAND:
        {
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);
            if (code == EN_CHANGE)
            {
                // Records the field's whole current value, which is how a test asserts
                // that text landed where it was aimed -- and, more importantly, that it
                // did not land where it was not.
                if (id == IdDocumentField)
                    Record("FIELD document=" + TextOf(documentField));
                else if (id == IdSecondField)
                    Record("FIELD second=" + TextOf(secondField));
                else if (id == IdPasswordField)
                {
                    // Length only. Even a fixture should not write a password field's
                    // contents to a file.
                    Record("FIELD password_length=" +
                        std::to_string(TextOf(passwordField).size()));
                }
                return 0;
            }
            if (code != BN_CLICKED) return 0;

            switch (id)
            {
                case IdHarmless:  Record("CLICK harmless"); break;
                case IdSave:      SimulateEffect("saved", TextOf(documentField)); break;
                case IdSend:      SimulateEffect("sent", TextOf(documentField)); break;
                case IdDelete:    SimulateEffect("deleted", "item"); break;
                case IdBuy:       SimulateEffect("purchased", "item"); break;
                case IdAmbiguous: Record("CLICK ambiguous_confirm"); break;
                case IdOpenDeleteAcct: ShowConfirmWindow(L"Delete account"); break;
                case IdOpenPrefs:      ShowConfirmWindow(L"Preferences"); break;
                case IdVanishing:
                    Record("CLICK vanishing");
                    break;
                case IdShifty:
                    Record("CLICK shifty=" + TextOf(shiftyButton));
                    break;
                default: break;
            }
            return 0;
        }
        case WM_APP + 1:
            // Test hook: make the vanishing control disappear on demand, so a test can
            // create the gap between observing a control and acting on it.
            if (vanishingButton != nullptr)
            {
                DestroyWindow(vanishingButton);
                vanishingButton = nullptr;
                Record("CONTROL vanished");
            }
            return 0;
        case WM_APP + 2:
            // Test hook: relabel a control, so an observation can go stale in the one
            // way a window-level check cannot see.
            if (shiftyButton != nullptr)
            {
                SetWindowTextW(shiftyButton, L"Delete everything");
                Record("CONTROL relabelled");
            }
            return 0;
        case WM_APP + 5:
            // Test hook: clear every field and mark the log. Tests assert on the field's
            // whole value, so leftover text from an earlier test makes a later assertion
            // impossible to write correctly -- deterministic state beats a cleverer
            // substring match.
            SetWindowTextW(documentField, L"");
            SetWindowTextW(secondField, L"");
            SetWindowTextW(passwordField, L"");
            Record("STATE cleared");
            return 0;
        case WM_APP + 4:
            // Test hook: put focus back on the document field, so a test can establish
            // where typing starts before it asserts anything about where it stops.
            SetFocus(documentField);
            Record("FOCUS document_field");
            return 0;
        case WM_APP + 3:
            // Test hook: move focus to the second field without changing the window,
            // which is the same-HWND focus change the binding has to notice.
            SetFocus(secondField);
            Record("FOCUS second_field");
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void CreateControls(HWND parent)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const auto button = [&](const wchar_t* text, const int id, const int x, const int y)
    {
        return CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, x, y, 150, 28,
            parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    const auto field = [&](const int id, const int x, const int y, const DWORD extra)
    {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra, x, y, 320, 26,
            parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };

    documentField = field(IdDocumentField, 20, 20, 0);
    secondField = field(IdSecondField, 20, 56, 0);
    passwordField = field(IdPasswordField, 20, 92, ES_PASSWORD);

    button(L"Zoom in", IdHarmless, 20, 130);
    button(L"Save", IdSave, 180, 130);
    button(L"Send", IdSend, 340, 130);
    button(L"Delete", IdDelete, 20, 166);
    button(L"Buy now", IdBuy, 180, 166);
    button(L"Confirm", IdAmbiguous, 340, 166);
    button(L"Open delete account", IdOpenDeleteAcct, 20, 202);
    button(L"Open preferences", IdOpenPrefs, 180, 202);
    vanishingButton = button(L"Vanishing control", IdVanishing, 340, 202);
    shiftyButton = button(L"Rename file", IdShifty, 20, 238);

    // Drag surfaces. Static controls with names UI Automation can read.
    CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Draggable item",
        WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 285, 140, 60,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdDragSource)),
        instance, nullptr);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Safe folder",
        WS_CHILD | WS_VISIBLE | SS_CENTER, 180, 285, 140, 60,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdSafeDrop)),
        instance, nullptr);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Delete permanently",
        WS_CHILD | WS_VISIBLE | SS_CENTER, 340, 285, 140, 60,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdDangerousDrop)),
        instance, nullptr);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int)
{
    // --log <path>
    {
        const std::wstring arguments = commandLine == nullptr ? L"" : commandLine;
        const std::size_t flag = arguments.find(L"--log ");
        if (flag != std::wstring::npos)
        {
            logPath = arguments.substr(flag + 6);
            while (!logPath.empty() && (logPath.front() == L'"' || logPath.front() == L' '))
                logPath.erase(logPath.begin());
            while (!logPath.empty() && (logPath.back() == L'"' || logPath.back() == L' '))
                logPath.pop_back();
        }
    }

    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.lpfnWndProc = MainWindowProc;
    mainClass.hInstance = instance;
    mainClass.lpszClassName = L"ReviaFixtureMain";
    mainClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    mainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&mainClass);

    WNDCLASSEXW dialogClass = mainClass;
    dialogClass.lpfnWndProc = DialogWindowProc;
    dialogClass.lpszClassName = L"ReviaFixtureDialog";
    RegisterClassExW(&dialogClass);

    mainWindow = CreateWindowExW(0, L"ReviaFixtureMain", L"Revia Fixture - Main",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 520, 400,
        nullptr, nullptr, instance, nullptr);
    CreateControls(mainWindow);

    // A second top-level window owned by the same process. This is the case a process
    // check cannot see and a window handle can.
    secondWindow = CreateWindowExW(0, L"ReviaFixtureMain", L"Revia Fixture - Second",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 660, 100, 400, 260,
        nullptr, nullptr, instance, nullptr);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 20, 20, 320, 26,
        secondWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdDocumentField)),
        instance, nullptr);

    Record("FIXTURE ready");
    SetForegroundWindow(mainWindow);
    SetFocus(documentField);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Record("FIXTURE closed");
    return 0;
}
