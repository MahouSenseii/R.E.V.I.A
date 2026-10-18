#include "Policy/guestCapabilityPolicy.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace revia::policy;
using namespace revia::workspace;

namespace
{
void Check(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

GuestCapabilitySettings Enabled()
{
    GuestCapabilitySettings settings;
    settings.enabled = settings.read = settings.write = settings.execute = true;
    settings.limits.approvedExecutables = {"/usr/bin/python3", "/usr/bin/cmake"};
    return settings;
}

GuestRequest Request(const GuestOperation operation)
{
    GuestRequest request;
    request.operation = operation;
    request.path = operation == GuestOperation::ListDirectory ? "." : "src/main.cpp";
    request.expectedDigest = "missing";
    request.executable = "/usr/bin/python3";
    request.workingDirectory = ".";
    request.expectedIdentity = {"environment-a", "boot-a"};
    return request;
}

void TestCapabilitiesAreIndependent()
{
    for (const auto operation : {GuestOperation::ListDirectory, GuestOperation::ReadText,
             GuestOperation::WriteText, GuestOperation::ExecuteProcess})
    {
        const auto request = Request(operation);
        auto settings = Enabled();
        Check(EvaluateGuestCapability(request, settings).allowed, "Explicitly enabled guest operation was refused.");
        settings.enabled = false;
        Check(!EvaluateGuestCapability(request, settings).allowed, "Disabled guest capability admitted an operation.");
        settings = Enabled();
        settings.read = settings.write = settings.execute = false;
        settings.allowTaskExecution = true;
        Check(!EvaluateGuestCapability(request, settings).allowed, "Task delegation granted an operation capability.");
    }
    auto settings = Enabled();
    settings.execute = false;
    Check(EvaluateGuestCapability(Request(GuestOperation::ReadText), settings).allowed,
        "Revoking execution also revoked independent reading.");
    auto invalid = Request(static_cast<GuestOperation>(999));
    Check(!EvaluateGuestCapability(invalid, Enabled()).allowed, "Unknown operation was admitted.");
}

void TestGuestPathsRemainRelativeAndNormalized()
{
    auto request = Request(GuestOperation::ReadText);
    for (const auto& path : {std::string("src/Main.cpp"), std::string("src/main.cpp"), std::string("notes/\xE6\x97\xA5.txt")})
    {
        request.path = path;
        Check(EvaluateGuestCapability(request, Enabled()).allowed, "Valid Linux workspace path was refused: " + path);
    }
    const std::string nulPath("src/a\0.cpp", 10);
    for (const auto& path : {std::string(), std::string("/etc/passwd"), std::string("C:/Windows/win.ini"),
             std::string("src\\main.cpp"), std::string("../outside"), std::string("src/../outside"),
             std::string("./src/main.cpp"), std::string("src//main.cpp"), std::string("src/"),
             std::string("src/./main.cpp"), std::string("src/line\nname"), std::string("src/\xFF"), nulPath,
             std::string(256, 'x'), std::string(2049, 'x')})
    {
        request.path = path;
        Check(!EvaluateGuestCapability(request, Enabled()).allowed, "Unsafe or non-normal guest path was admitted.");
    }
    request.path = ".";
    Check(!EvaluateGuestCapability(request, Enabled()).allowed, "A file operation accepted the workspace directory.");
    request.operation = GuestOperation::ListDirectory;
    Check(EvaluateGuestCapability(request, Enabled()).allowed, "Listing the workspace root was refused.");
}

void TestWriteRequiresItsExactPrecondition()
{
    auto request = Request(GuestOperation::WriteText);
    auto settings = Enabled();
    request.content = "replacement";
    Check(EvaluateGuestCapability(request, settings).allowed, "Create-only write was refused.");
    request.expectedDigest = std::string(64, 'a');
    Check(EvaluateGuestCapability(request, settings).allowed, "SHA256-guarded write was refused.");
    for (const auto& digest : {std::string(), std::string("absent"), std::string(63, 'a'),
             std::string(65, 'a'), std::string(64, 'z'), std::string(64, 'A')})
    {
        request.expectedDigest = digest;
        Check(!EvaluateGuestCapability(request, settings).allowed, "A write without a canonical digest was admitted.");
    }
    request.expectedDigest = "missing";
    settings.limits.maxWriteBytes = 3;
    request.content = "abc";
    Check(EvaluateGuestCapability(request, settings).allowed, "A write exactly at its limit was refused.");
    request.content = "abcd";
    Check(!EvaluateGuestCapability(request, settings).allowed, "The configured write limit was ignored.");
    request.content = "\xFF";
    Check(!EvaluateGuestCapability(request, settings).allowed, "Malformed UTF-8 was admitted as guest text.");
    request.content = "abc";
    settings.limits.maxWriteBytes = 262145;
    Check(!EvaluateGuestCapability(request, settings).allowed, "A guest hard limit was widened by configuration.");
}

void TestExecutionIsLiteralAndBounded()
{
    auto request = Request(GuestOperation::ExecuteProcess);
    auto settings = Enabled();
    request.arguments = {"-c", "print('guest only')"};
    Check(EvaluateGuestCapability(request, settings).allowed, "Literal allowed guest process was refused.");
    for (const auto& executable : {"python3", "/bin/sh", "/usr/bin/Python3", "/usr/bin/../bin/python3", "C:/Python/python.exe"})
    {
        request.executable = executable;
        Check(!EvaluateGuestCapability(request, settings).allowed, "Unapproved or nonliteral executable was admitted.");
    }
    request.executable = "/usr/bin/python3";
    request.workingDirectory = "src";
    Check(EvaluateGuestCapability(request, settings).allowed, "Relative guest process cwd was refused.");
    request.workingDirectory = "../host";
    Check(!EvaluateGuestCapability(request, settings).allowed, "Escaping guest cwd was admitted.");
    request.workingDirectory = ".";
    settings.limits.maxTimeoutMs = 500;
    for (const int timeout : {0, -1, 501})
    {
        request.timeoutMs = timeout;
        Check(!EvaluateGuestCapability(request, settings).allowed, "An invalid or excessive timeout was admitted.");
    }
    request.timeoutMs = 500;
    Check(EvaluateGuestCapability(request, settings).allowed, "Timeout exactly at its configured ceiling was refused.");
    request.arguments = {std::string("a\0b", 3)};
    Check(!EvaluateGuestCapability(request, settings).allowed, "An embedded NUL entered a guest argument.");
    request.arguments = {"\xFF"};
    Check(!EvaluateGuestCapability(request, settings).allowed, "Malformed UTF-8 entered a guest argument.");
    request.arguments.assign(129, "x");
    Check(!EvaluateGuestCapability(request, settings).allowed, "Unbounded guest argument count was admitted.");
    request.arguments = {std::string(8193, 'x')};
    Check(!EvaluateGuestCapability(request, settings).allowed, "Unbounded guest arguments were admitted.");
    request.arguments.assign(9, std::string(8192, 'x'));
    Check(!EvaluateGuestCapability(request, settings).allowed, "The combined guest argument byte limit was ignored.");
}

void TestCapturedIdentityCannotChange()
{
    auto settings = Enabled();
    settings.expectedIdentity = {"environment-a", "boot-a"};
    auto request = Request(GuestOperation::ReadText);
    Check(EvaluateGuestCapability(request, settings).allowed, "Matching captured guest identity was refused.");
    request.expectedIdentity.bootId = "boot-b";
    Check(!EvaluateGuestCapability(request, settings).allowed, "A task silently rebound to a new guest boot.");
    request.expectedIdentity = {};
    Check(!EvaluateGuestCapability(request, settings).allowed, "A captured task accepted an unstamped request.");
}

void TestIntersectionOnlyNarrowsGuestAuthority()
{
    auto captured = Enabled();
    captured.allowTaskExecution = true;
    captured.expectedIdentity = {"environment-a", "boot-a"};
    auto current = Enabled();
    current.write = false;
    current.limits.approvedExecutables = {"/usr/bin/cmake", "/usr/bin/Python3"};
    current.limits.maxReadBytes = 100;
    current.limits.maxTimeoutMs = 900;
    const auto result = IntersectGuestCapabilities(captured, current);
    Check(result.enabled && result.read && result.execute && !result.write && !result.allowTaskExecution,
        "Guest capability intersection widened a flag or lost independent authority.");
    Check(result.limits.maxReadBytes == 100 && result.limits.maxTimeoutMs == 900 &&
        result.limits.approvedExecutables == std::vector<std::string>{"/usr/bin/cmake"},
        "Guest limits or case-sensitive executable authority were widened.");
    Check(result.expectedIdentity.environmentId == "environment-a" && result.expectedIdentity.bootId == "boot-a",
        "The task's captured guest identity was discarded.");
    current.expectedIdentity = {"environment-a", "boot-b"};
    Check(!IntersectGuestCapabilities(captured, current).enabled, "A scope intersection silently switched guest boot.");
    current = Enabled();
    captured.enabled = false;
    Check(!IntersectGuestCapabilities(captured, current).enabled, "Current authority enabled a previously disabled guest scope.");
}
}

int main()
{
    try
    {
        TestCapabilitiesAreIndependent();
        TestGuestPathsRemainRelativeAndNormalized();
        TestWriteRequiresItsExactPrecondition();
        TestExecutionIsLiteralAndBounded();
        TestCapturedIdentityCannotChange();
        TestIntersectionOnlyNarrowsGuestAuthority();
        std::cout << "Guest capability policy: 6 tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Guest capability policy: " << error.what() << '\n';
        return 1;
    }
}
