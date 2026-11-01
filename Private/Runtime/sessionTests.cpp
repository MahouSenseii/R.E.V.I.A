#include "Runtime/reviaSession.h"
#include "Actions/actionTypes.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <map>
#include <set>
#include <sstream>

namespace revia::runtime
{
namespace
{
const char* TestUsage =
    "Usage: /tests | /tests quick | /tests response | /tests run <approved-group> | "
    "/tests status | /tests catalog [page] | /tests last | /tests details | /tests cancel";

bool IsTestInput(const std::string& input)
{
    return input == "/tests" || (input.starts_with("/tests") && input.size() > 6 &&
        std::isspace(static_cast<unsigned char>(input[6])));
}

bool IsTestGroup(const std::string& group)
{
    return !group.empty() && group.size() <= 64 && std::all_of(group.begin(), group.end(), [](const unsigned char value)
        { return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '_' || value == '-'; });
}

std::string DeveloperEnvironment(const char* name)
{
#ifdef _WIN32
    const std::string narrowName(name);
    const std::wstring wideName(narrowName.begin(), narrowName.end());
    const wchar_t* value = _wgetenv(wideName.c_str());
    if (!value)
        return {};
    const auto encoded = std::filesystem::path(value).u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

TestRunConfig DeveloperTestConfig()
{
    TestRunConfig config;
    config.enabled = DeveloperEnvironment("REVIA_TESTS_ENABLED") == "1";
    if (!config.enabled)
        return config;
    config.sourceRoot = std::filesystem::u8path(DeveloperEnvironment("REVIA_TESTS_SOURCE_ROOT"));
    config.buildRoot = std::filesystem::u8path(DeveloperEnvironment("REVIA_TESTS_BUILD_ROOT"));
    config.ctestExecutable = std::filesystem::u8path(DeveloperEnvironment("REVIA_TESTS_CTEST"));
    config.policyFile = std::filesystem::u8path(DeveloperEnvironment("REVIA_TESTS_POLICY"));
    config.outputRoot = std::filesystem::u8path(DeveloperEnvironment("REVIA_TESTS_OUTPUT_ROOT"));
    config.sourceIdentity = DeveloperEnvironment("REVIA_TESTS_SOURCE_IDENTITY");
    config.configuration = DeveloperEnvironment("REVIA_TESTS_CONFIGURATION");
    config.environment = {{"LANG", "C"}, {"LC_ALL", "C"}, {"TZ", "UTC"}};
    if (const auto path = DeveloperEnvironment("REVIA_TESTS_RUNTIME_PATH"); !path.empty())
        config.environment.emplace("PATH", path);
    return config;
}

std::string TestState(const TestRunSnapshot& snapshot, const std::string& configurationError, const bool showSetup = false)
{
    std::ostringstream text;
    text << "Developer test catalog: " << (configurationError.empty() ? snapshot.phase : "blocked") << '\n';
    if (!configurationError.empty())
        text << configurationError << '\n';
    else if (!snapshot.message.empty())
        text << snapshot.message << '\n';
    if (!snapshot.activeTest.empty())
        text << "Active: " << snapshot.activeTest << '\n';
    if (!snapshot.runId.empty())
        text << "Run: " << snapshot.runId << '\n';
    if (showSetup && (snapshot.phase == "disabled" || !configurationError.empty()))
    {
        text << "Developer setup requires REVIA_TESTS_ENABLED=1 and explicit absolute REVIA_TESTS_SOURCE_ROOT, "
                "REVIA_TESTS_BUILD_ROOT, REVIA_TESTS_CTEST, REVIA_TESTS_POLICY, REVIA_TESTS_OUTPUT_ROOT, plus "
                "REVIA_TESTS_SOURCE_IDENTITY. Set REVIA_TESTS_CONFIGURATION for a multi-config build and "
                "REVIA_TESTS_RUNTIME_PATH only when test dependencies need it; restart the session after setup. "
                "Packaged releases may lack developer test binaries. /tests never builds or installs them.\n";
    }
    return text.str();
}

std::string TestCatalogMenu(const TestRunSnapshot& snapshot)
{
    std::ostringstream text;
    struct GroupCount
    {
        std::size_t modules = 0;
        std::size_t runnable = 0;
    };
    std::map<std::string, GroupCount> groups;
    std::size_t held = 0, unknown = 0;
    for (const auto& entry : snapshot.catalog)
    {
        const std::set<std::string> memberships(entry.groups.begin(), entry.groups.end());
        for (const auto& membership : memberships)
        {
            auto& group = groups[membership];
            ++group.modules;
            group.runnable += entry.availability == "runnable";
        }
        held += entry.availability != "runnable";
        unknown += entry.scope == "unknown";
    }
    text << "Select a reviewed policy group with /tests run <group>.\n"
            "Conveniences: /tests quick selects the safe defaults; /tests response selects synthetic timing logic.\n";
    if (snapshot.catalog.empty())
        text << "Groups and module counts are unavailable until discovery completes.\n";
    else
    {
        for (const auto& [id, count] : groups)
        {
            std::string label = id;
            std::replace(label.begin(), label.end(), '_', ' ');
            std::replace(label.begin(), label.end(), '-', ' ');
            if (!label.empty() && label.front() >= 'a' && label.front() <= 'z')
                label.front() = static_cast<char>(label.front() - 'a' + 'A');
            text << label << ": " << count.modules << " modules, " << count.runnable << " currently runnable; /tests run "
                 << id << '\n';
        }
        text << "Registered modules: " << snapshot.catalog.size() << "; held or blocked: " << held
             << "; unknown scope: " << unknown << ".\n";
    }
    text << "Unknown, full, live, hardware and unreviewed checks remain held. No real model, audio or GPU measurement.\n"
            "/tests catalog [page] lists every independent module, its purpose, prerequisites and evidence applicability.\n"
            "/tests status retrieves background progress; /tests details shows recorded outcomes; /tests cancel stops test work.\n";
    return text.str();
}

std::string TestRunStatus(const TestRunSnapshot& snapshot)
{
    std::map<std::string, std::size_t> outcomes;
    std::size_t fresh = 0, reused = 0;
    for (const auto& result : snapshot.results)
    {
        ++outcomes[result.outcome];
        fresh += result.fresh;
        reused += result.outcome == "reused" || !result.reusedFrom.empty();
    }
    std::ostringstream text;
    text << "Recorded modules: " << snapshot.results.size() << "; fresh: " << fresh << "; reused: " << reused << ".\n";
    for (const auto& [outcome, count] : outcomes)
        text << outcome << ": " << count << "; ";
    if (!outcomes.empty())
        text << '\n';
    text << "Use /tests catalog [page] for the inventory or /tests details for evidence.\n";
    return text.str();
}

std::string TestCatalogPage(const TestRunSnapshot& snapshot, const std::size_t page)
{
    constexpr std::size_t pageSize = 16;
    const auto pages = std::max<std::size_t>(1, (snapshot.catalog.size() + pageSize - 1) / pageSize);
    std::ostringstream text;
    text << "Catalog page " << page << '/' << pages << " (" << snapshot.catalog.size() << " independent modules).\n";
    if (!snapshot.catalogIdentity.empty())
        text << "Catalog identity: " << snapshot.catalogIdentity << '\n';
    if (snapshot.catalog.empty())
        text << "No catalog is available yet. Use /tests status to check discovery.\n";
    const auto end = std::min(snapshot.catalog.size(), page * pageSize);
    for (std::size_t index = (page - 1) * pageSize; index < end; ++index)
    {
        const auto& entry = snapshot.catalog[index];
        text << "- " << entry.purpose << " (" << entry.id << ")\n  " << entry.group << "; " << entry.scope << "; "
             << entry.availability << "; latest=" << entry.latestOutcome << "; applicability=" << entry.applicability
             << "\n  Prerequisites: ";
        if (entry.prerequisites.empty())
            text << "unverified";
        for (std::size_t prerequisite = 0; prerequisite < entry.prerequisites.size(); ++prerequisite)
            text << (prerequisite ? "; " : "") << entry.prerequisites[prerequisite];
        text << '\n';
    }
    if (page < pages)
        text << "Next: /tests catalog " << page + 1 << '\n';
    return text.str();
}

std::string TestRunDetails(const TestRunSnapshot& snapshot)
{
    if (snapshot.runId.empty() && snapshot.results.empty())
        return "No test run has been recorded in this session. Nothing is verified.\n";
    std::ostringstream text;
    if (!snapshot.outputDirectory.empty())
        text << "Evidence directory: " << actions::PathToUtf8(snapshot.outputDirectory) << '\n';
    if (snapshot.results.empty())
        text << "No module outcome has been recorded yet. Nothing is verified.\n";
    for (const auto& result : snapshot.results)
    {
        text << "- " << result.id << ": " << result.outcome << "; " << (result.fresh ? "fresh" : "not fresh")
             << "; elapsed=" << result.elapsedMs << "ms; exit=" << result.exitCode << '\n' << "  " << result.detail << '\n';
        if (!result.observedAt.empty())
            text << "  Observed: " << result.observedAt << '\n';
        if (!result.reusedFrom.empty())
            text << "  Reused from: " << result.reusedFrom << '\n';
        if (!result.logFile.empty())
            text << "  Log: " << actions::PathToUtf8(result.logFile) << '\n';
        if (!result.junitFile.empty())
            text << "  JUnit: " << actions::PathToUtf8(result.junitFile) << '\n';
    }
    return text.str();
}
}

bool ReviaSession::TryHandleTestInput(const std::string& input, const agents::InputSource source,
    SessionResult& result, const bool hasAttachments)
{
    if (!IsTestInput(input))
        return false;
    result.stamp = Stamp();
    result.audienceRevision = Audience().revision;
    const auto refuse = [&](const std::string& why)
    {
        result.succeeded = false;
        result.text = result.reason = why;
        return true;
    };
    if (source != agents::InputSource::Typed)
        return refuse("/tests requires a typed local developer command.");
    if (hasAttachments)
        return refuse("/tests does not consume attachments. " + std::string(TestUsage));
    std::istringstream arguments(input);
    std::string command, action, group, pageArgument, extra;
    arguments >> command >> action;
    if (action == "run")
        arguments >> group;
    else if (action == "catalog")
        arguments >> pageArgument;
    else if (action == "quick" || action == "response")
        group = action;
    arguments >> extra;
    const bool run = action == "run" ? IsTestGroup(group) : action == "quick" || action == "response";
    if (!extra.empty() || (action == "run" && !run) ||
        (!action.empty() && !run && action != "status" && action != "catalog" && action != "last" &&
            action != "details" && action != "cancel"))
        return refuse(TestUsage);
    unsigned int page = 1;
    if (!pageArgument.empty())
    {
        const auto parsed = std::from_chars(pageArgument.data(), pageArgument.data() + pageArgument.size(), page);
        if (parsed.ec != std::errc{} || parsed.ptr != pageArgument.data() + pageArgument.size() || page == 0)
            return refuse(TestUsage);
    }

    std::lock_guard lock(testCommandMutex);
    if (!testCommandsOpen || !started.load() || !sessionIdentity.IsCurrent(result.stamp))
        return refuse("The developer test owner is unavailable because this session is stopped or retiring.");
    if (!testRunConfigured)
    {
        testRunConfigured = true;
        try
        {
            if (!testRunRuntime.Configure(DeveloperTestConfig(), testRunConfigurationError) && testRunConfigurationError.empty())
                testRunConfigurationError = "The developer test configuration is unavailable.";
        }
        catch (const std::exception& error)
        {
            testRunConfigurationError = std::string("The developer test configuration is invalid: ") + error.what();
        }
    }
    auto snapshot = testRunRuntime.Snapshot();
    if (run)
    {
        if (!testRunConfigurationError.empty() || snapshot.phase == "disabled")
            return refuse(TestState(snapshot, testRunConfigurationError, true));
        std::string error;
        if (!testRunRuntime.Start(group, error))
            return refuse(error + " Use /tests to discover the catalog, then /tests status.");
        result.text = "Started the " + group + " group in the background. Use /tests status and /tests details for results.";
    }
    else if (action == "cancel")
    {
        const bool active = snapshot.phase == "running" || snapshot.phase == "discovering";
        testRunRuntime.Cancel();
        result.text = active ? "Test cancellation requested. Use /tests status to confirm the recorded outcome."
                             : "No test work is active.";
    }
    else if (action == "last" || action == "details")
        result.text = TestState(snapshot, testRunConfigurationError) + TestRunDetails(snapshot);
    else
    {
        if (testRunConfigurationError.empty() &&
            ((action.empty() && snapshot.phase != "disabled" && snapshot.phase != "discovering" && snapshot.phase != "running") ||
                (action == "catalog" && snapshot.phase == "idle")))
        {
            std::string error;
            if (!testRunRuntime.Discover(error))
                return refuse(error);
            snapshot = testRunRuntime.Snapshot();
        }
        result.text = TestState(snapshot, testRunConfigurationError, action.empty());
        if (action == "catalog")
        {
            const auto pages = std::max<std::size_t>(1, (snapshot.catalog.size() + 15) / 16);
            if (page > pages)
                return refuse("Catalog page is out of range. Use /tests catalog with a page from 1 to " + std::to_string(pages) + ".");
            result.text += TestCatalogPage(snapshot, page);
        }
        else
            result.text += action.empty() ? TestCatalogMenu(snapshot) : TestRunStatus(snapshot);
    }
    return true;
}

void ReviaSession::OpenTestCommands()
{
    std::lock_guard lock(testCommandMutex);
    if (!testCommandsOpen)
    {
        testRunConfigured = false;
        testRunConfigurationError.clear();
        testCommandsOpen = true;
    }
}

void ReviaSession::StopTestCommands()
{
    std::lock_guard lock(testCommandMutex);
    testCommandsOpen = false;
    // This worker borrows no session state or callbacks, so it can join under the admission lock.
    testRunRuntime.Shutdown();
}
}
