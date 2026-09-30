#pragma once

#include <string>

namespace revia::core
{

// Reads UTF-8 lines, converting Windows console input from UTF-16.
// Pipes, redirected files and other platforms retain std::getline byte behavior.
class TerminalLineReader
{
public:
    // Returns false at end of input: end of file, a closed console, or Ctrl+Z on
    // Windows, which is how a console user ends input there.
    bool ReadLine(std::string& line);

private:
    // Console text read past the end of the line being returned, kept for the next call.
    std::wstring pending;
    bool ended = false;
};

// While it exists, a Windows console displays what the process writes as UTF-8, and
// puts the previous output code page back afterwards, because the setting belongs to
// the console window and outlives this process. Does nothing when standard output is not
// a console, and nothing on other platforms.
class Utf8ConsoleOutput
{
public:
    Utf8ConsoleOutput();
    ~Utf8ConsoleOutput();
    Utf8ConsoleOutput(const Utf8ConsoleOutput&) = delete;
    Utf8ConsoleOutput& operator=(const Utf8ConsoleOutput&) = delete;

private:
    unsigned int previousCodePage = 0;
};

} // namespace revia::core
