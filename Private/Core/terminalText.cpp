#include "Core/terminalText.h"

#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::core
{

#ifdef _WIN32
namespace
{

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    // Without WC_ERR_INVALID_CHARS a lone surrogate becomes U+FFFD rather than failing
    // the whole line, which is the same promise utf8::Sanitize makes everywhere else.
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }
    std::string converted(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        converted.data(), size, nullptr, nullptr);
    return converted;
}

HANDLE ConsoleHandle(const DWORD which)
{
    const HANDLE handle = GetStdHandle(which);
    DWORD mode = 0;
    return handle != nullptr && handle != INVALID_HANDLE_VALUE &&
            GetConsoleMode(handle, &mode)
        ? handle
        : nullptr;
}

} // namespace
#endif

bool TerminalLineReader::ReadLine(std::string& line)
{
    line.clear();
    if (ended)
    {
        return false;
    }
#ifdef _WIN32
    if (const HANDLE console = ConsoleHandle(STD_INPUT_HANDLE); console != nullptr)
    {
        std::size_t newline = pending.find(L'\n');
        while (newline == std::wstring::npos)
        {
            wchar_t buffer[1024];
            DWORD read = 0;
            SetLastError(ERROR_SUCCESS);
            const BOOL succeeded = ReadConsoleW(console, buffer, 1024, &read, nullptr);
            if (!succeeded || read == 0)
            {
                // Ctrl+C interrupts the read rather than ending input. Whether the
                // process goes on is the signal handler's decision, not this loop's.
                if (GetLastError() == ERROR_OPERATION_ABORTED)
                {
                    continue;
                }
                break;
            }
            pending.append(buffer, read);
            newline = pending.find(L'\n');
        }

        std::wstring text;
        if (newline == std::wstring::npos)
        {
            // The console closed. What arrived before it is still a line.
            text = std::move(pending);
            pending.clear();
            ended = true;
        }
        else
        {
            text = pending.substr(0, newline);
            pending.erase(0, newline + 1);
        }
        if (!text.empty() && text.back() == L'\r')
        {
            text.pop_back();
        }
        // Ctrl+Z ends console input, as it does for the C runtime reading one. Anything
        // typed before it on the same line is still delivered.
        if (const std::size_t endOfInput = text.find(L'\x1a');
            endOfInput != std::wstring::npos)
        {
            text.resize(endOfInput);
            pending.clear();
            ended = true;
        }
        if (ended && text.empty())
        {
            return false;
        }
        line = ToUtf8(text);
        return true;
    }
#endif
    if (!std::getline(std::cin, line))
    {
        ended = true;
        return false;
    }
    return true;
}

Utf8ConsoleOutput::Utf8ConsoleOutput()
{
#ifdef _WIN32
    if (ConsoleHandle(STD_OUTPUT_HANDLE) == nullptr)
    {
        return;
    }
    const UINT current = GetConsoleOutputCP();
    if (current != 0 && current != CP_UTF8 && SetConsoleOutputCP(CP_UTF8))
    {
        previousCodePage = current;
    }
#endif
}

Utf8ConsoleOutput::~Utf8ConsoleOutput()
{
#ifdef _WIN32
    if (previousCodePage != 0)
    {
        // Anything still buffered was written for the UTF-8 page, so it goes out first.
        std::cout.flush();
        std::cerr.flush();
        SetConsoleOutputCP(previousCodePage);
    }
#endif
}

} // namespace revia::core
