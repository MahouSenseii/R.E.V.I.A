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
