#include "testSupport.h"
#include "Core/terminalText.h"

#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
using revia::tests::Check;

#ifdef _WIN32
// Keystrokes, as the console receives them from a keyboard: a key-down and a key-up per
// UTF-16 unit, so a character outside the basic plane arrives as its two surrogates.
std::vector<INPUT_RECORD> Keystrokes(const std::wstring& text)
{
    std::vector<INPUT_RECORD> records;
    for (const wchar_t character : text)
    {
        INPUT_RECORD record{};
        record.EventType = KEY_EVENT;
        record.Event.KeyEvent.wRepeatCount = 1;
        record.Event.KeyEvent.uChar.UnicodeChar = character;
        if (character == L'\r')
        {
            record.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        }
        record.Event.KeyEvent.bKeyDown = TRUE;
        records.push_back(record);
        record.Event.KeyEvent.bKeyDown = FALSE;
        records.push_back(record);
    }
    return records;
}

void TestConsoleInputArrivesAsUtf8()
{
    // A console of this process's own when it has none, which is how CTest starts it.
    HANDLE console = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (console == INVALID_HANDLE_VALUE)
    {
        Check(AllocConsole() != FALSE, "No console could be created to type into.");
        console = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    Check(console != INVALID_HANDLE_VALUE, "The console input buffer could not be opened.");

    DWORD originalMode = 0;
    GetConsoleMode(console, &originalMode);
    // Cooked line input, the mode a terminal user types in.
    SetConsoleMode(console, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    const HANDLE originalInput = GetStdHandle(STD_INPUT_HANDLE);
    SetStdHandle(STD_INPUT_HANDLE, console);
    FlushConsoleInputBuffer(console);

    std::vector<INPUT_RECORD> records = Keystrokes(L"café ☕ \U0001F600\r");
    const std::vector<INPUT_RECORD> second = Keystrokes(L"second line\r");
    records.insert(records.end(), second.begin(), second.end());
    // Ctrl+Z, then Enter: how a Windows console user ends input.
    const std::vector<INPUT_RECORD> end = Keystrokes(L"\x1a\r");
    records.insert(records.end(), end.begin(), end.end());
    DWORD written = 0;
    const BOOL typed = WriteConsoleInputW(
        console, records.data(), static_cast<DWORD>(records.size()), &written);

    revia::core::TerminalLineReader reader;
    std::string first;
    std::string next;
    std::string after;
    const bool readFirst = typed && reader.ReadLine(first);
    const bool readNext = readFirst && reader.ReadLine(next);
    const bool readAfter = readNext && reader.ReadLine(after);

    SetStdHandle(STD_INPUT_HANDLE, originalInput);
    SetConsoleMode(console, originalMode);
    CloseHandle(console);

    Check(typed && written == records.size(), "The keystrokes could not be typed.");
    Check(readFirst && first == "caf\xC3\xA9 \xE2\x98\x95 \xF0\x9F\x98\x80",
        "Console input did not arrive as UTF-8: got " + std::to_string(first.size()) +
        " bytes.");
    Check(readNext && next == "second line", "The second console line was not read intact.");
    Check(!readAfter, "Ctrl+Z did not end console input.");
}
#endif

} // namespace

void RunTerminalInputTests()
{
#ifdef _WIN32
    TestConsoleInputArrivesAsUtf8();
    std::cout << "Console input reaches the session as UTF-8, line by line, and Ctrl+Z "
        "ends it.\n";
#else
    std::cout << "Terminal input tests are Windows-only; other platforms read bytes "
        "unchanged.\n";
#endif
}
