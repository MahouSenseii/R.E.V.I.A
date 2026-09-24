#include "Improvement/workbench.h"
#include "Core/utf8.h"

#include "Actions/actionTypes.h"
#include "Improvement/sourceCatalog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::improvement
{

namespace
{

using json = nlohmann::json;

constexpr const char* MirroredFolders[] = {
    "Config", "Desktop", "Private", "Public", "Tests", "Tools"};
constexpr const char* MirroredFiles[] = {"CMakeLists.txt", "CMakePresets.json"};

std::string ReadWhole(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Script output as UTF-8, whatever PowerShell wrote. Windows PowerShell's file cmdlets
// default to UTF-16 with a byte-order mark, and a UTF-16 CTest summary read as bytes has
// a zero after every character -- the proof then saw no test results at all.
std::string ReadScriptText(const std::filesystem::path& path)
{
    std::string raw = ReadWhole(path);
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF)
    {
        return raw.substr(3);
    }
    if (raw.size() < 2 || static_cast<unsigned char>(raw[0]) != 0xFF ||
        static_cast<unsigned char>(raw[1]) != 0xFE)
    {
        return raw;
    }
    std::string text;
    text.reserve(raw.size() / 2);
    for (std::size_t index = 2; index + 1 < raw.size(); index += 2)
    {
        const unsigned int unit = static_cast<unsigned char>(raw[index]) |
            (static_cast<unsigned int>(static_cast<unsigned char>(raw[index + 1])) << 8);
        // CTest and compiler output is ASCII; anything wider is replaced rather than
        // decoded, which is all a summary parser needs.
        text.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
    }
    return text;
}

std::string Stamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return {};
    const auto written = std::filesystem::last_write_time(path, error);
    if (error) return {};
    return std::to_string(size) + ":" +
        std::to_string(written.time_since_epoch().count());
}

// Compiler output names files however the build spelled them -- forward slashes from
// CMake, backslashes from Windows, and sometimes both in one path.
std::string StripRoot(std::string text, const std::filesystem::path& root)
{
    const std::string forward = actions::PathToUtf8(root);
    std::string backward = forward;
    std::replace(backward.begin(), backward.end(), '/', '\\');
    for (const std::string& prefix : {forward, backward})
    {
        for (const char separator : {'/', '\\'})
        {
            const std::string form = prefix + separator;
            for (std::size_t at = text.find(form); at != std::string::npos; at = text.find(form, at))
                text.erase(at, form.size());
        }
    }
    return text;
}

} // namespace

std::vector<TestOutcome> ParseCtestOutput(const std::string& output)
{
    static const std::regex line(
        R"(^\s*\d+/\d+\s+Test\s+#\d+:\s+(\S+)\s+\.*\s*(\*+)?\s*(Passed|Failed|Not Run|Timeout|Exception|SEGFAULT|Subprocess aborted|Child aborted|Exit code \S+))");
    std::vector<TestOutcome> outcomes;
    std::istringstream stream(output);
    for (std::string text; std::getline(stream, text);)
    {
        if (!text.empty() && text.back() == '\r') text.pop_back();
        std::smatch match;
        if (std::regex_search(text, match, line))
            outcomes.push_back({match[1].str(), match[3].str() == "Passed"});
    }
    return outcomes;
}

std::string ExtractBuildErrors(const std::string& log, const std::size_t maximumLines)
{
    std::ostringstream errors;
    std::size_t kept = 0;
    std::istringstream stream(log);
    for (std::string text; std::getline(stream, text) && kept < maximumLines;)
    {
        if (!text.empty() && text.back() == '\r') text.pop_back();
        const bool relevant = text.find("error:") != std::string::npos ||
            text.find("undefined reference") != std::string::npos ||
            (text.find("error ") != std::string::npos && text.find(": error") != std::string::npos);
        if (!relevant) continue;
        if (text.size() > 400) text = utf8::Prefix(text, 400) + "...";
        errors << text << '\n';
        ++kept;
    }
    return errors.str();
}

bool Workbench::IsMirrored(const std::string& relativePath)
{
    if (relativePath.empty() || relativePath.find("..") != std::string::npos) return false;
    for (const char* file : MirroredFiles)
        if (relativePath == file) return true;
    if (relativePath.find("__pycache__") != std::string::npos ||
        (relativePath.size() >= 8 &&
            relativePath.compare(relativePath.size() - 8, 8, ".pending") == 0))
    {
        return false;
    }
    for (const char* folder : MirroredFolders)
    {
        const std::string prefix = std::string(folder) + "/";
        if (relativePath.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

Workbench::Workbench(
    std::filesystem::path sourceRoot, std::filesystem::path workbenchRoot, BuildRunner buildRunner)
    : source(std::move(sourceRoot)), root(std::move(workbenchRoot)), runner(std::move(buildRunner))
{
    LoadManifest();
}

bool Workbench::Inside(const std::filesystem::path& path) const
{
    std::error_code error;
    const std::filesystem::path base = std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    const std::filesystem::path candidate = std::filesystem::weakly_canonical(path, error);
    if (error) return false;
    const std::filesystem::path relative = candidate.lexically_relative(base);
    return !relative.empty() && relative.native().rfind(std::filesystem::path("..").native(), 0) != 0;
}

bool Workbench::WriteMirrorFile(
    const std::string& relativePath, const std::string& content, std::string& outError) const
{
    const std::filesystem::path target = MirrorRoot() / actions::Utf8ToPath(relativePath);
    // The one property this class exists to keep: nothing it writes lands outside it.
    if (!Inside(target))
    {
        outError = "Refused to write outside the workbench: " + relativePath;
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file.good())
    {
        outError = "Could not write " + relativePath + " in the workbench.";
        return false;
    }
    return true;
}

void Workbench::LoadManifest()
{
    manifest.clear();
    std::error_code error;
    const std::filesystem::path path = root / "manifest.json";
    if (!std::filesystem::is_regular_file(path, error)) return;
    try
    {
        const json data = json::parse(ReadWhole(path));
        const json files = data.value("files", json::object());
        for (const auto& [file, stamp] : files.items())
            if (stamp.is_string()) manifest[file] = stamp.get<std::string>();
        baselineStamp = data.value("baselineStamp", std::string{});
        for (const auto& failure : data.value("baselineFailures", json::array()))
            if (failure.is_string()) baselineFailures.insert(failure.get<std::string>());
    }
    catch (const std::exception&)
    {
        // A lost manifest only means copying everything again.
        manifest.clear();
    }
}

void Workbench::SaveManifest() const
{
    json data = {{"files", json::object()}, {"baselineStamp", baselineStamp},
        {"baselineFailures", json::array()}};
    for (const auto& [file, stamp] : manifest) data["files"][file] = stamp;
    for (const std::string& failure : baselineFailures) data["baselineFailures"].push_back(failure);
    std::error_code error;
    std::filesystem::create_directories(root, error);
    std::ofstream file(root / "manifest.json", std::ios::binary | std::ios::trunc);
    file << data.dump(1);
}

std::string Workbench::ManifestStamp() const
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& [file, stamp] : manifest)
    {
        for (const unsigned char character : file + "=" + stamp + ";")
        {
            hash ^= character;
            hash *= 1099511628211ULL;
        }
    }
    return std::to_string(hash);
}

std::size_t Workbench::Sync(std::string& outError)
{
    outError.clear();
    std::error_code error;
    std::filesystem::create_directories(MirrorRoot(), error);
    if (error)
    {
        outError = "The workbench could not be created at " + actions::PathToUtf8(root) + ".";
        return 0;
    }
    std::size_t changed = 0;
    std::set<std::string> seen;
    const auto copyOne = [&](const std::filesystem::path& from, const std::string& relative)
    {
        seen.insert(relative);
        const std::string stamp = Stamp(from);
        const std::filesystem::path to = MirrorRoot() / actions::Utf8ToPath(relative);
        std::error_code copyError;
        if (manifest[relative] == stamp && std::filesystem::exists(to, copyError)) return;
        if (!Inside(to)) return;
        std::filesystem::create_directories(to.parent_path(), copyError);
        // Removed first rather than copied over: MinGW's copy_file refuses an existing
        // target even with overwrite_existing. A fresh timestamp is wanted anyway -- the
        // build must see the copy as newer than what it last built, even when the
        // source edit is older than that build.
        std::filesystem::remove(to, copyError);
        copyError.clear();
        std::filesystem::copy_file(from, to, copyError);
        if (copyError)
        {
            if (outError.empty()) outError = "Could not copy " + relative + ".";
            return;
        }
        manifest[relative] = stamp;
        ++changed;
    };

    for (const char* file : MirroredFiles)
    {
        if (std::filesystem::is_regular_file(source / file, error)) copyOne(source / file, file);
    }
    for (const char* folder : MirroredFolders)
    {
        const std::filesystem::path base = source / folder;
        if (!std::filesystem::is_directory(base, error)) continue;
        auto iterator = std::filesystem::recursive_directory_iterator(
            base, std::filesystem::directory_options::skip_permission_denied, error);
        for (; !error && iterator != std::filesystem::recursive_directory_iterator();
            iterator.increment(error))
        {
            if (!iterator->is_regular_file(error)) continue;
            const std::string relative = actions::PathToUtf8(
                std::filesystem::relative(iterator->path(), source, error));
            if (!error && IsMirrored(relative)) copyOne(iterator->path(), relative);
        }
        error.clear();
    }

    // What the source no longer has must leave the copy too: the build globs its
    // folders, so a deleted file left behind would still be compiled.
    for (auto entry = manifest.begin(); entry != manifest.end();)
    {
        if (seen.count(entry->first) != 0)
        {
            ++entry;
            continue;
        }
        const std::filesystem::path stale = MirrorRoot() / actions::Utf8ToPath(entry->first);
        if (Inside(stale)) std::filesystem::remove(stale, error);
        entry = manifest.erase(entry);
        ++changed;
    }
    SaveManifest();
    return changed;
}

VerificationResult Workbench::Verify(const CodeChange& change, const std::stop_token stopToken)
{
    VerificationResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [&](VerificationResult finished)
    {
        finished.seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        return finished;
    };

    if (!SourceCatalog::IsReviewable(change.path))
    {
        result.concluded = true;
        result.summary = change.path + " is not a file she may change.";
        return finish(result);
    }
    std::string error;
    Sync(error);
    if (!error.empty())
    {
        result.summary = error;
        return finish(result);
    }
    const std::string original = ReadWhole(MirrorRoot() / actions::Utf8ToPath(change.path));
    const std::optional<std::string> patched = ApplyChange(original, change);
    if (!patched)
    {
        result.concluded = true;
        result.summary = "The change no longer applies to the current source.";
        return finish(result);
    }
    if (!WriteMirrorFile(change.path, *patched, error))
    {
        result.summary = error;
        return finish(result);
    }
    const BuildOutcome run = runner(MirrorRoot(), BuildRoot(), stopToken);
    // Reverted before anything else, whatever happened, so the copy never carries one
    // proposal into the next.
    std::string restoreError;
    const bool restored = WriteMirrorFile(change.path, original, restoreError);
    if (!restored)
    {
        // The manifest no longer describes this file; the next sync copies it again.
        manifest.erase(change.path);
        SaveManifest();
    }

    if (!run.completed)
    {
        result.summary = run.failure.empty() ? "The build did not finish." : run.failure;
        return finish(result);
    }
    if (!run.built)
    {
        result.concluded = true;
        result.buildErrors = StripRoot(ExtractBuildErrors(run.buildLog), MirrorRoot());
        result.summary = "Does not build" + (result.buildErrors.empty() ? std::string(".")
            : ": " + result.buildErrors.substr(0, result.buildErrors.find('\n')));
        return finish(result);
    }
    const std::vector<TestOutcome> tests = ParseCtestOutput(run.testOutput);
    if (!run.testsRan || tests.empty())
    {
        result.concluded = true;
        result.summary = "Built, but the test suites did not run, so it is not proven.";
        return finish(result);
    }
    std::vector<std::string> failed;
    for (const TestOutcome& test : tests)
        if (!test.passed) failed.push_back(test.name);
    const std::size_t total = tests.size();
    if (failed.empty())
    {
        result.concluded = true;
        result.verified = true;
        result.summary = "Builds; " + std::to_string(total) + "/" + std::to_string(total) +
            " test suites pass.";
        return finish(result);
    }

    // Something failed. Whether the change caused it is a question for the same copy
    // without the change -- measured once per source state, and only when needed.
    if (baselineStamp != ManifestStamp())
    {
        // Still patched, the comparison would fail the same way and excuse the change.
        if (!restored)
        {
            result.summary = "The change could not be reverted in the workbench, so there is "
                "nothing to compare against: " + restoreError;
            return finish(result);
        }
        const BuildOutcome baseline = runner(MirrorRoot(), BuildRoot(), stopToken);
        if (!baseline.completed)
        {
            result.summary = "The comparison build did not finish.";
            return finish(result);
        }
        if (!baseline.built)
        {
            result.concluded = true;
            result.summary = "The unchanged source does not build in the workbench either, so "
                "nothing can be proven until it does.";
            return finish(result);
        }
        baselineFailures.clear();
        for (const TestOutcome& test : ParseCtestOutput(baseline.testOutput))
            if (!test.passed) baselineFailures.insert(test.name);
        baselineStamp = ManifestStamp();
        SaveManifest();
    }
    std::vector<std::string> broken;
    for (const std::string& name : failed)
        if (baselineFailures.count(name) == 0) broken.push_back(name);
    result.concluded = true;
    if (broken.empty())
    {
        result.verified = true;
        result.summary = "Builds; " + std::to_string(total - failed.size()) + "/" +
            std::to_string(total) + " test suites pass, and the " +
            std::to_string(failed.size()) + " failing already fail without the change.";
        return finish(result);
    }
    std::string names;
    for (const std::string& name : broken) names += (names.empty() ? "" : ", ") + name;
    result.summary = "Builds, but breaks " + names + ".";
    return finish(result);
}

BuildRunner MakeScriptRunner(
    std::filesystem::path scriptPath,
    std::filesystem::path dependencyRoot,
    std::filesystem::path logDirectory,
    const int parallelJobs,
    const int timeoutMinutes)
{
    return [scriptPath, dependencyRoot, logDirectory, parallelJobs, timeoutMinutes](
        const std::filesystem::path& sourceRoot,
        const std::filesystem::path& buildRoot,
        const std::stop_token stopToken) -> BuildOutcome
    {
        BuildOutcome outcome;
#ifdef _WIN32
        const auto started = std::chrono::steady_clock::now();
        std::error_code error;
        std::filesystem::create_directories(logDirectory, error);
        const std::string stamp = std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count());
        const std::filesystem::path logPath = logDirectory / ("build-" + stamp + ".log");
        const std::filesystem::path testPath = logDirectory / ("ctest-" + stamp + ".txt");
        const std::filesystem::path resultPath = logDirectory / ("result-" + stamp + ".json");
        if (!std::filesystem::is_regular_file(scriptPath, error))
        {
            outcome.failure = "The workbench build script is missing: " +
                actions::PathToUtf8(scriptPath);
            return outcome;
        }

        const auto quote = [](const std::wstring& value)
        {
            return L"\"" + value + L"\"";
        };
        std::wstring command = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
            quote(scriptPath.wstring()) +
            L" -SourceRoot " + quote(sourceRoot.wstring()) +
            L" -BuildRoot " + quote(buildRoot.wstring()) +
            L" -DepsRoot " + quote(dependencyRoot.wstring()) +
            L" -TestOutput " + quote(testPath.wstring()) +
            L" -ResultPath " + quote(resultPath.wstring()) +
            L" -Jobs " + std::to_wstring(std::max(1, parallelJobs));

        SECURITY_ATTRIBUTES inherit{};
        inherit.nLength = sizeof(inherit);
        inherit.bInheritHandle = TRUE;
        HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE nothing = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE || nothing == INVALID_HANDLE_VALUE)
        {
            if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
            if (nothing != INVALID_HANDLE_VALUE) CloseHandle(nothing);
            outcome.failure = "Could not open the workbench build log.";
            return outcome;
        }

        // Killing powershell alone would leave ninja and a dozen compilers running; the
        // job takes the whole tree down with it.
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nothing;
        startup.hStdOutput = log;
        startup.hStdError = log;
        PROCESS_INFORMATION process{};
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        // Below normal, and inherited by every compiler it starts: a proof is never
        // allowed to make her conversation or voice stutter.
        const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
            BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
            sourceRoot.c_str(), &startup, &process);
        CloseHandle(log);
        CloseHandle(nothing);
        if (!created)
        {
            if (job != nullptr) CloseHandle(job);
            outcome.failure = "PowerShell could not be started for the workbench build.";
            return outcome;
        }
        if (job != nullptr) AssignProcessToJobObject(job, process.hProcess);
        ResumeThread(process.hThread);
        CloseHandle(process.hThread);

        const auto deadline = started + std::chrono::minutes(std::max(5, timeoutMinutes));
        bool stopped = false;
        while (WaitForSingleObject(process.hProcess, 500) == WAIT_TIMEOUT)
        {
            if (stopToken.stop_requested() || std::chrono::steady_clock::now() > deadline)
            {
                stopped = true;
                if (job != nullptr) TerminateJobObject(job, 1);
                else TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 10000);
                break;
            }
        }
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        if (job != nullptr) CloseHandle(job);

        outcome.seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        outcome.buildLog = ReadScriptText(logPath);
        if (stopped)
        {
            outcome.failure = stopToken.stop_requested()
                ? "The workbench build was stopped."
                : "The workbench build ran past its time limit.";
            return outcome;
        }
        try
        {
            const json result = json::parse(ReadScriptText(resultPath));
            outcome.completed = true;
            outcome.built = result.value("built", false);
            outcome.testsRan = result.value("testsRan", false);
            outcome.testOutput = ReadScriptText(testPath);
        }
        catch (const std::exception&)
        {
            outcome.failure = "The workbench build script ended (exit " +
                std::to_string(exitCode) + ") without reporting a result.";
        }
#else
        (void)scriptPath;
        (void)dependencyRoot;
        (void)logDirectory;
        (void)parallelJobs;
        (void)timeoutMinutes;
        (void)sourceRoot;
        (void)buildRoot;
        (void)stopToken;
        outcome.failure = "Proving changes needs the Windows toolchain.";
#endif
        return outcome;
    };
}

} // namespace revia::improvement
