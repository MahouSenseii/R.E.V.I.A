#include "testSupport.h"

#include "Policy/capabilityPolicy.h"
#include "Process/processExecutor.h"
#include "Filesystem/fileSystemExecutor.h"
#include "Audit/contentDigest.h"

#ifdef _WIN32
#include <windows.h>
#endif

void RunProcessOwnerTests()
{
#ifdef _WIN32
    using namespace revia;
    tests::ScopedTestDirectory directory;
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    const auto fixture = std::filesystem::path(path).parent_path() / "ReviaProcessFixture.exe";
    actions::CapabilitySettings settings;
    settings.approvedRoots = {directory.root};
    settings.process.enabled = true;
    settings.process.approvedExecutables = {fixture};
    settings.process.maxOutputBytes = 128;
    const policy::CapabilityPolicy policy(settings);
    actions::ActionRequest request;
    request.type = actions::ActionType::ExecuteProcess;
    request.process.executable = fixture;
    request.process.workingDirectory = directory.root;
    request.process.arguments = {"output", "space inside", "", "quote\"", "slash\\"};
    request.process.timeoutMs = 1000;
    request.beforeEffect = [](const std::string&) { return std::string(); };
    process::ProcessExecutor executor;
    const auto output = executor.Execute(request, policy.Evaluate(request));
    tests::Check(output.attempted && output.process && output.process->exitCode == 7 &&
                     output.process->standardOutput == "[space inside][][quote\"][slash\\]" &&
                     output.process->standardError == "fixture stderr",
        "Native process owner lost arguments, output or nonzero exit: " + output.message);
    request.process.arguments = {"flood"};
    const auto flood = executor.Execute(request, policy.Evaluate(request));
    tests::Check(flood.succeeded && flood.process->outputTruncated &&
                     flood.process->standardOutput.size() + flood.process->standardError.size() <= 128,
        "Native process owner failed to drain bounded output: " + flood.message);
    request.process.arguments = {"sleep"};
    request.process.timeoutMs = 50;
    const auto timeout = executor.Execute(request, policy.Evaluate(request));
    tests::Check(timeout.process && timeout.process->timedOut && !timeout.succeeded,
        "Native process owner did not stop its timed-out child: " + timeout.message);

    filesystem::FileSystemExecutor files(4096, 64, 64);
    actions::ActionRequest write;
    write.type = actions::ActionType::WriteTextFile;
    write.source = directory.root / "guarded.txt";
    write.expectedDigest = "missing";
    write.value = "original";
    write.beforeEffect = [](const std::string&) { return std::string(); };
    tests::Check(files.Execute(write, policy.Evaluate(write)).succeeded, "Native guarded creation failed.");
    write.value = "replacement";
    tests::Check(!files.Execute(write, policy.Evaluate(write)).succeeded, "Creation overwrote an existing file.");
    write.expectedDigest = audit::ContentDigest("stale");
    tests::Check(!files.Execute(write, policy.Evaluate(write)).succeeded, "A stale write replaced the current file.");
    write.expectedDigest = audit::ContentDigest("original");
    tests::Check(files.Execute(write, policy.Evaluate(write)).succeeded, "Matching file identity did not permit replacement.");
    write.type = actions::ActionType::ReadTextFile;
    const auto read = files.Execute(write, policy.Evaluate(write));
    tests::Check(read.succeeded && read.content == "replacement" && read.entries.front() == "sha256:" + audit::ContentDigest("replacement"),
        "Guarded replacement or exact prior-content digest was lost.");
#endif
}
