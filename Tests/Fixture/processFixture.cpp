#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    const std::string mode = argv[1];
    if (mode == "output")
    {
        for (int index = 2; index < argc; ++index)
            std::cout << '[' << argv[index] << ']';
        std::cerr << "fixture stderr";
        return 7;
    }
    // A stand-in for an interpreter given a script on its command line: the script passes unless
    // it says "fail".
    if (mode == "-c")
    {
        // A script with an error of its own, as Python reports one raised in the script's lines.
        if (argc > 2 && std::string(argv[2]).find("broken") != std::string::npos)
        {
            std::cerr << "Traceback (most recent call last):\n  File \"<string>\", line 1, in <module>\nNameError: name 'broken' is not defined\n";
            return 1;
        }
        if (argc > 2 && std::string(argv[2]).find("fail") != std::string::npos)
        {
            std::cerr << "Traceback\nAssertionError: fixture check failed\n";
            return 1;
        }
        return 0;
    }
    if (mode == "cwd")
    {
        std::cout << std::filesystem::current_path().generic_string();
        return 0;
    }
    if (mode == "flood")
    {
        std::cout << std::string(256 * 1024, 'o');
        std::cerr << std::string(256 * 1024, 'e');
        return 0;
    }
    if (mode == "sleep")
    {
        std::cout << "ready" << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
#ifdef _WIN32
    if (mode == "tree" && argc == 3)
    {
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr, path, 32768);
        std::wstring command = L"\"" + std::wstring(path) + L"\" sleep";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child))
            return 3;
        {
            std::ofstream receipt(argv[2]);
            receipt << child.dwProcessId;
            if (!receipt.good())
                return 4;
        }
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
#endif
    return 5;
}
