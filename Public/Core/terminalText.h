#pragma once

#include <string>

namespace revia::core
{

// Reads standard input one line at a time, as UTF-8.
//
// Everything past the terminal assumes UTF-8, and a Windows console does not provide it:
// std::getline there returns bytes in the console's input code page, so "café" typed
// into cmd.exe arrived as 63 61 66 82 under code page 437. That is not UTF-8, and every
// JSON request built from the turn threw on it, the archive refused it, and the reply
// echoed it back as mojibake. A console is therefore read as UTF-16 and converted here.
//
// Input that is not a console -- a pipe, a redirected file -- and every other platform
// are read with std::getline unchanged, because their bytes are already whatever the
// sender wrote.
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
