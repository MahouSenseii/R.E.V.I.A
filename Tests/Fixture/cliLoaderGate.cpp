#include <Windows.h>

#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "Usage: ReviaCliLoaderGate absolute-executable-path\n");
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    wchar_t windowsDirectory[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(windowsDirectory, MAX_PATH))
    {
        return 2;
    }
    const std::wstring systemPath = std::wstring(windowsDirectory) + L"\\System32;" + windowsDirectory;
    SetEnvironmentVariableW(L"PATH", systemPath.c_str());
    SetDllDirectoryW(L"");
    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(argv[1], command.data(), nullptr, nullptr, FALSE,
        DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    {
        std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }
    bool loaded = false;
    bool exited = false;
    const ULONGLONG deadline = GetTickCount64() + 30000;
    while (!exited && GetTickCount64() < deadline)
    {
        DEBUG_EVENT event = {};
        if (!WaitForDebugEvent(&event, 500))
        {
            if (GetLastError() == ERROR_SEM_TIMEOUT)
            {
                continue;
            }
            break;
        }
        DWORD continuation = DBG_CONTINUE;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT && event.u.CreateProcessInfo.hFile)
        {
            CloseHandle(event.u.CreateProcessInfo.hFile);
        }
        else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile)
        {
            CloseHandle(event.u.LoadDll.hFile);
        }
        else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT)
        {
            const DWORD code = event.u.Exception.ExceptionRecord.ExceptionCode;
            if (code == EXCEPTION_BREAKPOINT)
            {
                // The loader breakpoint precedes the executable entry point. Stop
                // here so a packaging check cannot initialize models or user state.
                loaded = true;
                TerminateProcess(process.hProcess, 0);
            }
            else
            {
                std::fprintf(stderr, "Loader exception: 0x%08lx\n", code);
                continuation = DBG_EXCEPTION_NOT_HANDLED;
            }
        }
        else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
        {
            std::printf("Child exit: 0x%08lx\n", event.u.ExitProcess.dwExitCode);
            exited = true;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation);
    }
    if (!exited)
    {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!loaded || !exited)
    {
        std::fprintf(stderr, "CLI loader did not reach its pre-entry breakpoint with only Windows directories on PATH.\n");
        return 1;
    }
    std::printf("CLI imports resolved with only Windows directories on PATH; stopped before entry point.\n");
    return 0;
}
