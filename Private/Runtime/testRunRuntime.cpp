#include "Runtime/testRunRuntime.h"

#include "Actions/actionTypes.h"
#include "Audit/contentDigest.h"
#include "Core/atomicFile.h"
#include "Core/utf8.h"
#include "Process/ownedProcess.h"

#include <expat.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::runtime
{
namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::size_t MetadataLimit = 2U * 1024U * 1024U;
constexpr std::size_t LogLimit = 65536;
constexpr std::size_t JunitLimit = 512U * 1024U;
constexpr std::size_t ApprovedLimit = 256;
constexpr std::size_t LatestLimit = 128U * 1024U;

std::string PathText(const fs::path& path) { return actions::PathToUtf8(path); }

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool Within(const fs::path& path, const fs::path& root)
{
    auto child = PathText(path.lexically_normal());
    auto parent = PathText(root.lexically_normal());
#ifdef _WIN32
    child = Lower(std::move(child));
    parent = Lower(std::move(parent));
#endif
    if (child == parent) return true;
    if (!parent.ends_with('/')) parent += '/';
    return child.starts_with(parent);
}

void CheckStop(const std::stop_token stop)
{
    if (stop.stop_requested()) throw std::runtime_error("Cancelled before a complete observation.");
}

std::string ReadFile(const fs::path& path, const std::size_t maximum)
{
    const auto size = fs::file_size(path);
    if (size > maximum) throw std::runtime_error("An input exceeded its bounded file budget: " + PathText(path.filename()));
    const auto modified = fs::last_write_time(path);
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("An input could not be read: " + PathText(path.filename()));
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if ((!input && !bytes.empty()) || fs::file_size(path) != size || fs::last_write_time(path) != modified)
        throw std::runtime_error("An input changed while it was being read.");
    return bytes;
}

std::string DigestFile(const fs::path& path, const std::size_t maximum = 256U * 1024U * 1024U)
{
    return audit::ContentDigest(ReadFile(path, maximum));
}

std::string ObservedAt()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream text;
    text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return text.str();
}

bool ReparsePoint(const fs::path& path)
{
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    const auto error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) throw std::runtime_error("An artifact path could not be inspected safely.");
    return false;
#else
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) throw std::runtime_error("An artifact path could not be inspected safely.");
    return fs::is_symlink(status);
#endif
}

void GuardArtifact(const fs::path& path, const fs::path& root)
{
    const auto text = PathText(path);
    if (text.size() > 32768 || text.find('\0') != std::string::npos) throw std::runtime_error("An artifact path exceeded its bounded path budget.");
    if (!path.is_absolute() || !Within(path.lexically_normal(), root) || fs::weakly_canonical(root) != root || ReparsePoint(root))
        throw std::runtime_error("An artifact path escaped or replaced the approved output root.");
    auto current = root;
    std::size_t components = 0;
    for (const auto& part : path.lexically_normal().lexically_relative(root))
    {
        if (part == ".") continue;
        if (part == ".." || ++components > 128) throw std::runtime_error("An artifact path exceeded its approved bounded scope.");
        current /= part;
        if (ReparsePoint(current) || !Within(fs::weakly_canonical(current), root))
            throw std::runtime_error("Artifact links and reparse points are outside the approved output scope.");
    }
}

std::string ReadArtifact(const fs::path& path, const fs::path& root, const std::size_t maximum)
{
    GuardArtifact(path, root);
    auto bytes = ReadFile(path, maximum);
    GuardArtifact(path, root);
    return bytes;
}

std::string DigestArtifact(const fs::path& path, const fs::path& root, const std::size_t maximum)
{
    return audit::ContentDigest(ReadArtifact(path, root, maximum));
}

void WriteFile(const fs::path& path, const std::string& bytes, const fs::path& root)
{
    GuardArtifact(path, root);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("An owned result artifact could not be saved.");
    GuardArtifact(path, root);
}

void WriteAtomic(const fs::path& path, const std::string& bytes, const fs::path& root)
{
    GuardArtifact(path, root);
    GuardArtifact(actions::Utf8ToPath(PathText(path) + ".bak"), root);
    const auto temporary = path.parent_path() / (PathText(path.filename()) + ".pending-" + actions::NewActionId());
    WriteFile(temporary, bytes, root);
    std::string error;
    if (!core::ReplaceFileAtomically(path, temporary, error)) throw std::runtime_error(error);
    GuardArtifact(path, root);
}

bool Busy(const TestRunSnapshot& snapshot) { return snapshot.phase == "discovering" || snapshot.phase == "running"; }

TestRunConfig ValidateConfig(TestRunConfig config)
{
    if (!config.enabled) return config;
    for (const auto* path : {&config.sourceRoot, &config.buildRoot, &config.ctestExecutable, &config.policyFile, &config.outputRoot})
        if (!path->is_absolute()) throw std::runtime_error("Tests require explicit absolute developer source, build, CTest, policy and output paths.");
    config.sourceRoot = fs::canonical(config.sourceRoot);
    config.buildRoot = fs::canonical(config.buildRoot);
    config.ctestExecutable = fs::canonical(config.ctestExecutable);
    config.policyFile = fs::canonical(config.policyFile);
    config.outputRoot = fs::weakly_canonical(config.outputRoot);
    if (!fs::is_directory(config.sourceRoot) || !fs::is_directory(config.buildRoot) ||
        !fs::is_regular_file(config.sourceRoot / "CMakeLists.txt") || !fs::is_regular_file(config.buildRoot / "CMakeCache.txt"))
        throw std::runtime_error("The configured developer source/build is unavailable; no build or setup is attempted.");
    const auto toolName = Lower(PathText(config.ctestExecutable.filename()));
    if (!fs::is_regular_file(config.ctestExecutable) || (toolName != "ctest.exe" && toolName != "ctest"))
        throw std::runtime_error("An explicit native CTest executable is required.");
    if (!Within(config.policyFile, config.sourceRoot) || Within(config.outputRoot, config.sourceRoot) ||
        Within(config.sourceRoot, config.outputRoot) || config.outputRoot == config.buildRoot)
        throw std::runtime_error("Policy must belong to the approved source; results need a separate owned output root.");
    for (auto ancestor = config.outputRoot; !ancestor.empty();)
    {
        if (fs::is_regular_file(ancestor / "results.json") && fs::is_regular_file(ancestor / "results.junit.xml"))
            throw std::runtime_error("An existing run directory cannot be reused as an output root; prior runs are read-only resume inputs.");
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    if (config.sourceIdentity.empty() || config.sourceIdentity.size() > 256 || config.configuration.size() > 64)
        throw std::runtime_error("A bounded declared source identity and configuration are required.");
    const auto cache = ReadFile(config.buildRoot / "CMakeCache.txt", MetadataLimit);
    const std::string key = "CMAKE_HOME_DIRECTORY:INTERNAL=";
    const auto at = cache.find(key);
    if (at == std::string::npos) throw std::runtime_error("The developer build cache does not identify its source root.");
    auto source = cache.substr(at + key.size(), cache.find('\n', at) - at - key.size());
    if (!source.empty() && source.back() == '\r') source.pop_back();
    if (fs::canonical(actions::Utf8ToPath(source)) != config.sourceRoot)
        throw std::runtime_error("The build cache belongs to a different developer source root.");
    std::size_t bytes = 0;
    for (const auto& [name, value] : config.environment)
    {
        if (name != "PATH" && name != "LANG" && name != "LC_ALL" && name != "TZ")
            throw std::runtime_error("Only PATH, LANG, LC_ALL and TZ may supplement the minimal test environment.");
        bytes += name.size() + value.size();
        if (value.find('\0') != std::string::npos || bytes > 16384)
            throw std::runtime_error("The test environment exceeded its bounded explicit allowlist.");
    }
    return config;
}

bool ExcludedDirectory(const std::string& name)
{
    const auto lower = Lower(name);
    static const std::set<std::string> excluded = {".git", ".cache", "__pycache__", "node_modules", "thirdparty", "models",
        "runtimedata", "build", ".venv", "venv", "dist", "out"};
    return excluded.contains(lower) || lower.starts_with("cmake-build");
}

std::vector<fs::path> SourceFiles(const fs::path& root, const std::stop_token stop)
{
    std::vector<fs::path> files;
    std::size_t visited = 0;
    for (const auto* scope : {"Public", "Private", "Desktop", "Tools", "Tests", "Config"})
    {
        const auto directory = root / scope;
        if (!fs::exists(directory)) continue;
        if (fs::is_symlink(directory) || !Within(fs::canonical(directory), root))
            throw std::runtime_error("A first-party source scope is an escaping link.");
        for (fs::recursive_directory_iterator item(directory), end; item != end; ++item)
        {
            CheckStop(stop);
            if (++visited > 10000) throw std::runtime_error("The source inventory exceeded its bounded entry budget.");
            const bool authoredBuild = Lower(PathText(item->path().lexically_relative(root))) == "tools/build";
            if (item->is_directory() && ExcludedDirectory(PathText(item->path().filename())) && !authoredBuild)
            {
                item.disable_recursion_pending();
                continue;
            }
            if (item->is_symlink() || !Within(fs::canonical(item->path()), root))
                throw std::runtime_error("Linked first-party inputs cannot establish a bounded source fingerprint.");
            if (item->is_regular_file()) files.push_back(item->path());
            if (files.size() > 4096) throw std::runtime_error("The source inventory exceeded its bounded file budget.");
        }
    }
    for (const auto* manifest : {"CMakeLists.txt", "CMakePresets.json", "vcpkg.json", "vcpkg-configuration.json", "conanfile.txt",
             "conanfile.py", ".gitmodules", "package.json", "package-lock.json"})
    {
        const auto file = root / manifest;
        if (!fs::exists(file)) continue;
        if (fs::is_symlink(file) || !Within(fs::canonical(file), root)) throw std::runtime_error("A root manifest is linked.");
        files.push_back(file);
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string SourceFingerprint(const fs::path& root, const std::stop_token stop)
{
    const auto files = SourceFiles(root, stop);
    std::string fingerprints;
    std::vector<fs::file_time_type> modified;
    std::uintmax_t total = 0;
    for (const auto& file : files)
    {
        CheckStop(stop);
        total += fs::file_size(file);
        if (total > 64U * 1024U * 1024U) throw std::runtime_error("First-party source exceeded its 64 MiB fingerprint budget.");
        modified.push_back(fs::last_write_time(file));
        fingerprints += PathText(fs::relative(file, root)) + '\n' + DigestFile(file, 8U * 1024U * 1024U) + '\n';
    }
    if (SourceFiles(root, stop) != files) throw std::runtime_error("Source inventory changed during fingerprinting; discover again.");
    for (std::size_t index = 0; index < files.size(); ++index)
        if (fs::last_write_time(files[index]) != modified[index]) throw std::runtime_error("Source changed during fingerprinting; discover again.");
    return audit::ContentDigest(fingerprints);
}

struct ProcessObservation
{
    bool started = false, cancelled = false, timedOut = false, overflow = false;
    std::int64_t exitCode = -1, elapsedMs = 0;
    std::string output, error;
};

ProcessObservation Observe(const TestRunConfig& config, const std::vector<std::string>& arguments,
    const fs::path& scratch, const std::size_t outputLimit, const std::chrono::milliseconds timeout, const std::stop_token stop,
    const fs::path& workingDirectory = {}, const std::map<std::string, std::string>* contextEnvironment = nullptr)
{
    ProcessObservation result;
    if (stop.stop_requested()) { result.cancelled = true; return result; }
    GuardArtifact(scratch, config.outputRoot);
    fs::create_directories(scratch);
    GuardArtifact(scratch, config.outputRoot);
    process::OwnedProcessRequest request;
    request.executable = config.ctestExecutable;
    request.arguments = arguments;
    request.workingDirectory = workingDirectory.empty() ? config.buildRoot : workingDirectory;
    request.environment = config.environment;
#ifdef _WIN32
    wchar_t windows[MAX_PATH + 1]{};
    const auto windowsLength = GetWindowsDirectoryW(windows, MAX_PATH + 1);
    if (windowsLength > 0 && windowsLength <= MAX_PATH)
    {
        auto command = (fs::path(windows) / "System32/cmd.exe").make_preferred().u8string();
        request.environment["COMSPEC"] = std::string(reinterpret_cast<const char*>(command.data()), command.size());
        request.environment["SystemDrive"] = PathText(fs::path(windows).root_name());
    }
    request.environment["OS"] = "Windows_NT";
    request.environment["PATHEXT"] = ".COM;.EXE;.BAT;.CMD";
#endif
    request.environment["TEMP"] = PathText(scratch);
    request.environment["TMP"] = PathText(scratch);
    if (contextEnvironment)
        for (const auto& [name, value] : *contextEnvironment) request.environment[name] = value;
    request.maxPendingOutputBytes = outputLimit;
    request.maxErrorBytes = 8192;
    process::OwnedProcess child;
    const auto began = Clock::now();
    if (!child.Start(request, result.error)) return result;
    result.started = true;
    for (;;)
    {
        auto chunk = child.Read(20ms);
        if (result.output.size() + chunk.size() > outputLimit)
        {
            result.overflow = true;
            chunk.resize(outputLimit - result.output.size());
        }
        result.output += chunk;
        result.cancelled = stop.stop_requested();
        result.timedOut = Clock::now() - began > timeout;
        if (result.cancelled || result.timedOut || result.overflow)
        {
            child.Terminate();
            break;
        }
        if (!child.Running())
        {
            if (child.ExitCode().has_value()) break;
            std::this_thread::sleep_for(5ms);
        }
    }
    result.exitCode = child.ExitCode().value_or(-1);
    result.error = child.FinalErrorOutput(100ms);
    if (!child.StopReason().empty())
    {
        result.overflow = true;
        result.error += "\n" + child.StopReason();
    }
    result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
    result.output = utf8::Sanitize(result.output);
    result.error = utf8::Sanitize(result.error);
    return result;
}

std::vector<std::string> CtestArguments(const TestRunConfig& config, const fs::path& directory = {})
{
    std::vector<std::string> arguments = {"--test-dir", PathText(directory.empty() ? config.buildRoot : directory)};
    if (!config.configuration.empty()) { arguments.push_back("-C"); arguments.push_back(config.configuration); }
    return arguments;
}

bool GroupName(const std::string& name)
{
    return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](const unsigned char c)
        { return c < 128 && (std::isalnum(c) || c == '_' || c == '-'); });
}

struct NativeTest
{
    TestCatalogEntry entry;
    std::vector<std::string> command;
    Json properties = Json::object();
    Json execution = Json::object();
};
bool Member(const NativeTest& test, const std::string& group)
{
    return std::find(test.entry.groups.begin(), test.entry.groups.end(), group) != test.entry.groups.end();
}
struct CatalogData
{
    std::vector<NativeTest> tests;
    std::set<std::string> approvedGroups;
    std::string identity, sourceFingerprint, buildFingerprint, executionIdentity;
};

struct PolicyAnnotation
{
    std::string purpose, scope, group;
    std::vector<std::string> prerequisites, approvedGroups;
    Json execution = Json::object();
    Json history = Json::object();
};

std::map<std::string, PolicyAnnotation> ReadPolicy(const Json& policy, CatalogData& catalog)
{
    if (!policy.is_object() || policy.value("schema", 0) != 1 || !policy.contains("groups") || !policy.contains("modules") ||
        !policy.at("groups").is_object() || !policy.at("modules").is_object() || policy.at("groups").size() > 32 || policy.at("modules").size() > ApprovedLimit)
        throw std::runtime_error("Unknown or oversized checked test policy schema.");
    for (const auto& [key, value] : policy.items())
        if (key != "schema" && key != "groups" && key != "modules") throw std::runtime_error("Unknown checked policy field; commands belong only to CTest.");
    std::map<std::string, PolicyAnnotation> annotations;
    for (const auto& [id, module] : policy.at("modules").items())
    {
        if (id.empty() || id.size() > 256 || !module.is_object()) throw std::runtime_error("Invalid reviewed module metadata.");
        for (const auto& [key, value] : module.items())
            if (key != "purpose" && key != "prerequisites" && key != "execution" && key != "history")
                throw std::runtime_error("Unknown reviewed module field; no command registry is accepted.");
        PolicyAnnotation annotation;
        annotation.purpose = module.at("purpose").get<std::string>();
        annotation.prerequisites = module.value("prerequisites", std::vector<std::string>{});
        annotation.execution = module.value("execution", Json::object());
        annotation.history = module.value("history", Json::object());
        if (!annotation.execution.is_object() || !annotation.history.is_object()) throw std::runtime_error("Invalid reviewed execution/history metadata.");
        static const std::set<std::string> executionFields = {"approval", "adapter", "cwd", "environment", "requiredFiles", "registrationDigest",
            "interpreterArguments", "holdReason", "requiresOwnedLoopback", "requiredPrograms"};
        for (const auto& [key, value] : annotation.execution.items())
            if (!executionFields.contains(key)) throw std::runtime_error("Unknown execution-context constraint; commands belong only to CTest.");
        if (annotation.purpose.empty() || annotation.purpose.size() > 512 || annotation.prerequisites.size() > 8)
            throw std::runtime_error("Reviewed module descriptions exceeded their bounded metadata budget.");
        for (const auto& prerequisite : annotation.prerequisites)
            if (prerequisite.size() > 512) throw std::runtime_error("A module prerequisite exceeded its metadata budget.");
        annotations.emplace(id, std::move(annotation));
    }
    for (const auto& [group, definition] : policy.at("groups").items())
    {
        if (!GroupName(group) || !definition.is_object() || !definition.contains("ids") || !definition.at("ids").is_array() || definition.at("ids").size() > ApprovedLimit)
            throw std::runtime_error("Invalid or oversized checked policy group.");
        for (const auto& [key, value] : definition.items())
            if (key != "approval" && key != "scope" && key != "purpose" && key != "ids") throw std::runtime_error("Unknown group safety constraint.");
        const auto scope = definition.value("scope", "unknown");
        const bool approved = (scope == "quick" && definition.value("approval", "") == "reviewed-pure-native") ||
            (scope == "controlled" && definition.value("approval", "") == "reviewed-controlled");
        if (approved) catalog.approvedGroups.insert(group);
        std::set<std::string> identities;
        for (const auto& idValue : definition.at("ids"))
        {
            const auto id = idValue.get<std::string>();
            if (id.empty() || id.size() > 256 || !identities.insert(id).second) throw std::runtime_error("Invalid or duplicate policy module identity.");
            if (approved && !annotations.contains(id)) throw std::runtime_error("An approved module needs explicit reviewed purpose/prerequisite metadata.");
            auto& annotation = annotations[id];
            if (annotation.group.empty()) { annotation.group = group; annotation.scope = scope; }
            if (approved && scope == "quick") annotation.scope = "quick";
            if (approved) annotation.approvedGroups.push_back(group);
        }
        if (annotations.size() > ApprovedLimit) throw std::runtime_error("Policy module inventory exceeded its bounded limit.");
    }
    return annotations;
}

std::string ContainedPath(const std::string& value, const TestRunConfig& config)
{
    const auto path = actions::Utf8ToPath(value);
    if (!path.is_absolute()) return value;
    const auto normalized = path.lexically_normal();
    auto roots = std::vector<std::pair<fs::path, std::string>>{
        {config.sourceRoot, "<SOURCE_ROOT>"}, {config.buildRoot, "<BUILD_ROOT>"}};
    std::sort(roots.begin(), roots.end(), [](const auto& left, const auto& right) { return PathText(left.first).size() > PathText(right.first).size(); });
    for (const auto& [root, token] : roots)
        if (Within(normalized, root))
        {
            const auto suffix = PathText(normalized).substr(PathText(root).size());
            return token + suffix;
        }
    return value;
}

bool Interpreter(const fs::path& path, const std::string& kind)
{
    const auto name = Lower(PathText(path.filename()));
    return path.is_absolute() && ((kind == "python" && (name == "python.exe" || name == "python3.exe" || name == "python" || name == "python3")) ||
        (kind == "node" && (name == "node.exe" || name == "node")) ||
        (kind == "powershell" && (name == "powershell.exe" || name == "pwsh.exe" || name == "pwsh")));
}

std::map<std::string, std::string> RegisteredEnvironment(const NativeTest& test)
{
    std::map<std::string, std::string> environment;
    std::set<std::string> names;
    for (const auto& item : test.properties.value("ENVIRONMENT", std::vector<std::string>{}))
    {
        const auto equals = item.find('=');
        if (equals == std::string::npos || equals == 0 || !names.insert(Lower(item.substr(0, equals))).second ||
            !environment.emplace(item.substr(0, equals), item.substr(equals + 1)).second)
            throw std::runtime_error("Ambiguous registered environment needs review.");
    }
    return environment;
}

Json RegistrationIdentity(const NativeTest& test, const TestRunConfig& config)
{
    Json command = test.command;
    std::set<std::size_t> toolRoles;
    for (const auto& role : test.execution.value("interpreterArguments", Json::array()))
    {
        const auto index = role.at("index").get<std::size_t>();
        const auto kind = role.at("kind").get<std::string>();
        if (index >= test.command.size() || !toolRoles.insert(index).second || !Interpreter(actions::Utf8ToPath(test.command[index]), kind))
            throw std::runtime_error("The reviewed interpreter token role does not match current CTest argv.");
        command[index] = "<TOOL:" + kind + ">";
    }
    for (std::size_t index = 0; index < test.command.size(); ++index)
        if (!toolRoles.contains(index)) command[index] = ContainedPath(test.command[index], config);
    Json properties = test.properties;
    if (properties.contains("WORKING_DIRECTORY"))
        properties["WORKING_DIRECTORY"] = ContainedPath(properties.at("WORKING_DIRECTORY").get<std::string>(), config);
    if (properties.contains("ENVIRONMENT"))
        for (auto& item : properties["ENVIRONMENT"])
        {
            const auto original = item.get<std::string>();
            const auto equals = original.find('=');
            if (equals == std::string::npos) throw std::runtime_error("Malformed original environment property.");
            const auto key = original.substr(0, equals);
            const auto value = original.substr(equals + 1);
            item = key + "=" + (key == "QT_QPA_PLATFORM_PLUGIN_PATH" && actions::Utf8ToPath(value).is_absolute() ?
                "<REGISTERED_ENV:QT_QPA_PLATFORM_PLUGIN_PATH>" : ContainedPath(value, config));
        }
    return Json{{"command", command}, {"properties", properties}};
}

std::string Expand(const std::string& value, const TestRunConfig& config, const NativeTest& test, const fs::path& runRoot = {})
{
    std::string result = value;
    for (const auto& [token, replacement] : std::vector<std::pair<std::string, std::string>>{
             {"<SOURCE_ROOT>", PathText(config.sourceRoot)}, {"<BUILD_ROOT>", PathText(config.buildRoot)}, {"<RUN_ROOT>", PathText(runRoot)}})
    {
        if (result.starts_with(token)) result.replace(0, token.size(), replacement);
    }
    const std::string registered = "<REGISTERED_ENV:";
    if (result.starts_with(registered) && result.ends_with('>'))
    {
        const auto key = result.substr(registered.size(), result.size() - registered.size() - 1);
        const auto environment = RegisteredEnvironment(test);
        if (!environment.contains(key)) throw std::runtime_error("A reviewed registered environment prerequisite is missing.");
        result = environment.at(key);
    }
    const std::string tool = "<INTERPRETER_ROOT:";
    if (result.starts_with(tool))
    {
        const auto end = result.find('>');
        const auto kind = result.substr(tool.size(), end - tool.size());
        bool found = false;
        for (const auto& role : test.execution.value("interpreterArguments", Json::array()))
            if (role.at("kind") == kind)
            {
                const auto index = role.at("index").get<std::size_t>();
                result.replace(0, end + 1, PathText(actions::Utf8ToPath(test.command.at(index)).parent_path()));
                found = true;
                break;
            }
        if (!found) throw std::runtime_error("The reviewed interpreter-root prerequisite is unavailable.");
    }
    if (result.find('<') != std::string::npos || result.find('\0') != std::string::npos || result.size() > 32768)
        throw std::runtime_error("Unsupported or oversized reviewed context template.");
    return result;
}

std::map<std::string, std::string> OwnedEnvironment(const TestRunConfig& config, const NativeTest& test, const fs::path& root)
{
    auto environment = config.environment;
    const std::map<std::string, std::string> owned = {{"TEMP", PathText(root / "temp")}, {"TMP", PathText(root / "temp")},
        {"REVIA_LOG_DIR", PathText(root / "logs")}, {"USERPROFILE", PathText(root / "profile")}, {"APPDATA", PathText(root / "profile/Roaming")},
        {"LOCALAPPDATA", PathText(root / "profile/Local")}, {"HF_HOME", PathText(root / "profile/hf")}, {"TORCH_HOME", PathText(root / "profile/torch")},
        {"GIT_CONFIG_GLOBAL", PathText(root / "profile/absent-git-config")}, {"PYTHONDONTWRITEBYTECODE", "1"}, {"PYTHONNOUSERSITE", "1"},
        {"PYTHONUTF8", "1"}, {"PYTHONUNBUFFERED", "1"}, {"CUDA_VISIBLE_DEVICES", "-1"}, {"HF_HUB_OFFLINE", "1"}, {"TRANSFORMERS_OFFLINE", "1"},
        {"HF_DATASETS_OFFLINE", "1"}, {"GIT_CONFIG_NOSYSTEM", "1"}, {"GIT_TERMINAL_PROMPT", "0"}, {"no_proxy", "127.0.0.1,localhost,::1"}};
    for (const auto& [name, value] : owned) environment[name] = value;
    const auto registered = RegisteredEnvironment(test);
    const auto reviewedEnvironment = test.execution.value("environment", Json::object());
    for (const auto& [name, value] : reviewedEnvironment.items())
    {
        const auto lowered = Lower(name);
        static const std::set<std::string> refused = {"path", "systemroot", "windir", "comspec", "systemdrive", "pythonpath", "pythonhome", "pythonstartup",
            "node_options", "http_proxy", "https_proxy", "all_proxy", "ld_preload", "dyld_insert_libraries"};
        if (refused.contains(lowered) || lowered.starts_with("revia_live_") || lowered == "revia_web_enabled" || lowered == "revia_training_smoke_model")
            throw std::runtime_error("A context attempts to replace explicit tool/platform state or enable an unreviewed live hook.");
        const auto expanded = Expand(value.get<std::string>(), config, test, root);
        const auto control = std::find_if(owned.begin(), owned.end(), [&](const auto& item) { return Lower(item.first) == lowered; });
        if (control != owned.end())
        {
            if (expanded != control->second) throw std::runtime_error("Reviewed environment may not replace owned profile/temp/log/offline controls.");
        }
        else
        {
            if (!registered.contains(name) || expanded != registered.at(name))
                throw std::runtime_error("A context environment value does not match its pinned original property.");
            environment[name] = expanded;
        }
    }
    for (const auto& [name, value] : registered)
    {
        const auto control = std::find_if(owned.begin(), owned.end(), [&](const auto& item) { return Lower(item.first) == Lower(name); });
        if (control != owned.end()) continue;
        if (!environment.contains(name) || environment.at(name) != value)
            throw std::runtime_error("Every original environment value needs an exact reviewed context mapping.");
    }
    if (test.execution.value("adapter", "native") == "qt-offscreen" &&
        (!environment.contains("QT_QPA_PLATFORM") || environment.at("QT_QPA_PLATFORM") != "offscreen"))
        throw std::runtime_error("Qt fixture context must retain its offscreen platform.");
    return environment;
}

std::vector<fs::path> InputFiles(const NativeTest& test, const TestRunConfig& config)
{
    std::vector<fs::path> files;
    for (const auto& argument : test.command)
    {
        const auto path = actions::Utf8ToPath(argument);
        const auto extension = Lower(PathText(path.extension()));
        if (path.is_absolute() && (fs::is_regular_file(path) || extension == ".py" || extension == ".mjs" || extension == ".js" || extension == ".ps1" || extension == ".exe"))
            files.push_back(path);
    }
    for (const auto& role : test.execution.value("interpreterArguments", Json::array()))
    {
        const auto index = role.at("index").get<std::size_t>();
        if (index < test.command.size()) files.push_back(actions::Utf8ToPath(test.command[index]));
    }
    for (const auto& file : test.execution.value("requiredFiles", Json::array()))
        files.push_back(actions::Utf8ToPath(Expand(file.at("path").get<std::string>(), config, test)));
    for (const auto& [name, value] : RegisteredEnvironment(test))
    {
        if (name == "REVIA_MCP_SERVER_FIXTURE" || name == "REVIA_DATASET_EXPORT") files.push_back(actions::Utf8ToPath(value));
        if (name == "QT_QPA_PLATFORM_PLUGIN_PATH")
        {
#ifdef _WIN32
            files.push_back(actions::Utf8ToPath(value) / "qoffscreen.dll");
#else
            files.push_back(actions::Utf8ToPath(value) / "libqoffscreen.so");
#endif
        }
    }
    for (const auto& program : test.execution.value("requiredPrograms", std::vector<std::string>{}))
    {
        if (program.empty() || program.size() > 128 || actions::Utf8ToPath(program).has_parent_path())
            throw std::runtime_error("Invalid reviewed prerequisite program name.");
        std::istringstream paths(config.environment.contains("PATH") ? config.environment.at("PATH") : "");
        std::string directory;
        while (std::getline(paths, directory, ';'))
            if (!directory.empty() && fs::is_regular_file(actions::Utf8ToPath(directory) / program))
            {
                files.push_back(actions::Utf8ToPath(directory) / program);
                break;
            }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

void ReviewAvailability(NativeTest& test, const TestRunConfig& config)
{
    if (!test.execution.value("holdReason", "").empty()) { test.entry.availability = "held: " + test.execution.at("holdReason").get<std::string>(); return; }
    if (test.command.empty()) { test.entry.availability = "blocked: registered binary is not built; no build is attempted"; return; }
    const auto adapter = test.execution.value("adapter", "native");
    const auto executable = actions::Utf8ToPath(test.command.front());
    if (!executable.is_absolute() || !fs::is_regular_file(executable)) { test.entry.availability = "blocked: registered executable is unavailable"; return; }
    if (adapter == "native" || adapter == "qt-offscreen")
    {
        if (!Within(fs::weakly_canonical(executable), config.buildRoot) || actions::IsCommandSurfaceExecutable(PathText(executable.filename())) ||
#ifdef _WIN32
            Lower(PathText(executable.extension())) != ".exe" ||
#endif
            !fs::is_regular_file(executable))
            throw std::runtime_error("Reviewed native executable is outside its developer build.");
    }
    else if (!Interpreter(executable, adapter)) throw std::runtime_error("Registered interpreter does not match its reviewed adapter.");
    static const std::set<std::string> semantic = {"TIMEOUT", "LABELS", "ENVIRONMENT", "SKIP_RETURN_CODE", "WORKING_DIRECTORY"};
    for (const auto& [key, value] : test.properties.items())
        if (!semantic.contains(key)) { test.entry.availability = "held: unreviewed registration property " + key; return; }
    if (!test.execution.empty())
    {
        if (test.execution.value("approval", "") != "reviewed-controlled" || test.execution.value("registrationDigest", "") !=
            audit::ContentDigest(RegistrationIdentity(test, config).dump()))
        { test.entry.availability = "held: original argv or semantic registration properties differ from their reviewed identity"; return; }
        const auto cwd = test.execution.value("cwd", "registered");
        if (cwd != "registered" && cwd != "owned-config") throw std::runtime_error("Unknown reviewed working-directory context.");
        static_cast<void>(OwnedEnvironment(config, test, config.outputRoot / "context-validation"));
    }
    else if (test.properties.contains("ENVIRONMENT")) { test.entry.availability = "held: environment needs an explicit reviewed context"; return; }
    for (const auto& file : InputFiles(test, config))
        if (!fs::is_regular_file(file)) { test.entry.availability = "blocked: required existing fixture/dependency file missing: " + PathText(file.filename()); return; }
    for (const auto& file : test.execution.value("requiredFiles", Json::array()))
        if (file.contains("version"))
        {
            const auto path = actions::Utf8ToPath(Expand(file.at("path").get<std::string>(), config, test));
            if (Json::parse(ReadFile(path, 65536)).value("version", "") != file.at("version").get<std::string>())
            { test.entry.availability = "blocked: existing dependency package version does not match its reviewed prerequisite"; return; }
        }
    for (const auto& program : test.execution.value("requiredPrograms", std::vector<std::string>{}))
    {
        if (program.empty() || program.size() > 128 || actions::Utf8ToPath(program).has_parent_path()) throw std::runtime_error("Invalid reviewed prerequisite program name.");
        bool found = false;
        std::istringstream paths(config.environment.contains("PATH") ? config.environment.at("PATH") : "");
        std::string directory;
        while (std::getline(paths, directory, ';'))
            if (!directory.empty() && fs::is_regular_file(actions::Utf8ToPath(directory) / program)) { found = true; break; }
        if (!found) { test.entry.availability = "blocked: required existing tool is absent from the explicit runtime PATH: " + program; return; }
    }
    const auto working = fs::weakly_canonical(actions::Utf8ToPath(test.properties.value("WORKING_DIRECTORY", PathText(config.buildRoot))));
    if (!Within(working, config.sourceRoot) && !Within(working, config.buildRoot)) { test.entry.availability = "held: original working directory is outside approved roots"; return; }
    if (test.entry.timeoutSeconds > 300) { test.entry.availability = "blocked: registered timeout exceeds the reviewed bounded adapter"; return; }
    if (test.entry.timeoutSeconds <= 0) test.entry.timeoutSeconds = 120;
    test.entry.availability = "runnable";
    if (test.execution.value("requiresOwnedLoopback", false)) test.entry.prerequisites.push_back("Requires newly owned loopback listeners; host permission is unverified and no network probe is started.");
}

CatalogData ReadCatalog(const TestRunConfig& config, const fs::path& scratch, const std::stop_token stop)
{
    const auto policyBytes = ReadFile(config.policyFile, MetadataLimit);
    const auto policy = Json::parse(policyBytes);
    CatalogData catalog;
    const auto annotations = ReadPolicy(policy, catalog);
    if (fs::exists(config.buildRoot / "CTestCustom.cmake")) throw std::runtime_error("Custom CTest hooks need separate review.");
    auto arguments = CtestArguments(config);
    arguments.push_back("--show-only=json-v1");
    const auto observation = Observe(config, arguments, scratch, MetadataLimit, 15s, stop);
    if (!observation.started || observation.cancelled || observation.timedOut || observation.overflow || observation.exitCode != 0)
    {
        std::ostringstream diagnostic;
        diagnostic << "CTest discovery incomplete: started=" << std::boolalpha << observation.started << " exit=" << observation.exitCode
            << " cancelled=" << observation.cancelled << " timed_out=" << observation.timedOut << " overflow=" << observation.overflow
            << " elapsed_ms=" << observation.elapsedMs << " stdout_bytes=" << observation.output.size() << " stderr_bytes=" << observation.error.size();
        if (!observation.output.empty())
            diagnostic << " stdout_tail=" << observation.output.substr(observation.output.size() > 512 ? observation.output.size() - 512 : 0);
        if (!observation.error.empty())
            diagnostic << " stderr_tail=" << observation.error.substr(observation.error.size() > 512 ? observation.error.size() - 512 : 0);
        throw std::runtime_error(diagnostic.str());
    }
    const auto metadata = Json::parse(observation.output);
    if (metadata.value("kind", "") != "ctestInfo" || !metadata.contains("tests") || !metadata.at("tests").is_array() || metadata.at("tests").size() > 4096)
        throw std::runtime_error("Unsupported or oversized CTest registration metadata.");
    catalog.sourceFingerprint = SourceFingerprint(config.sourceRoot, stop);
    std::map<std::string, std::string> identities;
    for (const auto& item : metadata.at("tests"))
    {
        CheckStop(stop);
        NativeTest test;
        test.entry.id = item.at("name").get<std::string>();
        if (test.entry.id.empty() || test.entry.id.size() > 256) throw std::runtime_error("Oversized CTest registration identity.");
        const auto prior = identities.find(test.entry.id);
        if (prior != identities.end())
        {
            if (prior->second != item.dump()) throw std::runtime_error("Conflicting duplicate CTest registration identities.");
            continue;
        }
        identities.emplace(test.entry.id, item.dump());
        Json properties = Json::object();
        for (const auto& property : item.value("properties", Json::array())) properties[property.at("name").get<std::string>()] = property.at("value");
        test.properties = properties;
        if (properties.contains("LABELS")) test.entry.labels = properties.at("LABELS").get<std::vector<std::string>>();
        test.entry.timeoutSeconds = properties.value("TIMEOUT", 0.0);
        if (item.contains("command")) test.command = item.at("command").get<std::vector<std::string>>();
        test.entry.group = "unreviewed";
        test.entry.scope = "unknown";
        test.entry.purpose = "Existing independent CTest registration: " + test.entry.id + ". Purpose not yet reviewed.";
        test.entry.prerequisites = {"Explicit developer build; module prerequisites and live/resource effects are unreviewed."};
        test.entry.availability = "held: unreviewed scope and prerequisites";
        test.entry.applicability = "unverified: no current owned process observation";
        if (const auto annotation = annotations.find(test.entry.id); annotation != annotations.end())
        {
            if (!annotation->second.group.empty()) test.entry.group = annotation->second.group;
            if (!annotation->second.scope.empty()) test.entry.scope = annotation->second.scope;
            if (!annotation->second.purpose.empty()) test.entry.purpose = annotation->second.purpose;
            test.entry.prerequisites = annotation->second.prerequisites;
            test.entry.groups = annotation->second.approvedGroups;
            test.execution = annotation->second.execution;
            if (!annotation->second.history.empty())
            {
                test.entry.latestOutcome = "historical: " + annotation->second.history.value("outcome", "unverified");
                test.entry.applicability = "Historical controlled evidence for source " + annotation->second.history.value("sourceIdentity", "unknown") + "; no fresh execution or current binary qualification.";
            }
        }
        if (!test.entry.groups.empty())
        {
            const auto primary = std::find_if(test.entry.groups.begin(), test.entry.groups.end(), [](const std::string& group)
                { return group != "all-reviewed" && group != "benchmark-contract" && group != "panels"; });
            test.entry.group = primary == test.entry.groups.end() ? test.entry.groups.front() : *primary;
            test.entry.prerequisites.push_back("Explicit developer build and reviewed owned context; external dependency closure remains partial.");
            try { ReviewAvailability(test, config); }
            catch (const std::exception& error) { test.entry.availability = "held: " + std::string(error.what()); }
        }
        else if (!test.execution.value("holdReason", "").empty()) test.entry.availability = "held: " + test.execution.at("holdReason").get<std::string>();
        catalog.tests.push_back(std::move(test));
    }
    const Json configIdentity = {{"source", PathText(config.sourceRoot)}, {"build", PathText(config.buildRoot)}, {"ctest", PathText(config.ctestExecutable)},
        {"output", PathText(config.outputRoot)}, {"declaredSource", config.sourceIdentity}, {"configuration", config.configuration},
        {"environmentDigest", audit::ContentDigest(Json(config.environment).dump())}};
    catalog.buildFingerprint = audit::ContentDigest(ReadFile(config.buildRoot / "CMakeCache.txt", MetadataLimit) + DigestFile(config.ctestExecutable));
    catalog.identity = audit::ContentDigest(catalog.sourceFingerprint + catalog.buildFingerprint + metadata.dump() + policyBytes + configIdentity.dump());
    return catalog;
}

std::string SelectedIdentity(const CatalogData& catalog, const std::string& group, const TestRunConfig& config, const std::stop_token stop)
{
    Json digests = Json::object();
    std::set<fs::path> directories = {config.buildRoot};
    bool qt = false;
    for (const auto& test : catalog.tests)
        if (Member(test, group))
        {
            CheckStop(stop);
            digests["registration:" + test.entry.id] = test.entry.availability;
            if (test.entry.availability != "runnable") continue;
            qt = qt || test.execution.value("adapter", "native") == "qt-offscreen";
            for (const auto& file : InputFiles(test, config))
            {
                CheckStop(stop);
                if (fs::is_regular_file(file))
                {
                    const auto path = fs::canonical(file);
                    if (!digests.contains(PathText(path))) digests[PathText(path)] = DigestFile(path);
                    directories.insert(path.parent_path());
                }
                else digests[PathText(file)] = "missing";
            }
            for (const auto& role : test.execution.value("interpreterArguments", Json::array()))
                if (role.at("kind") == "python")
                {
                    const auto root = actions::Utf8ToPath(test.command.at(role.at("index").get<std::size_t>())).parent_path();
                    std::size_t libraries = 0;
                    for (const auto& item : fs::directory_iterator(root))
                        if (item.is_regular_file() && Lower(PathText(item.path().filename())).starts_with("python") &&
                            Lower(PathText(item.path().extension())) == ".dll")
                        {
                            CheckStop(stop);
                            if (++libraries > 16) throw std::runtime_error("Interpreter runtime dependency inventory exceeded its bounded scope.");
                            digests[PathText(item.path())] = DigestFile(item.path());
                        }
                }
        }
    if (config.environment.contains("PATH"))
    {
        std::istringstream paths(config.environment.at("PATH"));
        std::string path;
        while (std::getline(paths, path, ';'))
            if (!path.empty() && actions::Utf8ToPath(path).is_absolute()) directories.insert(actions::Utf8ToPath(path));
    }
    for (const auto* dependency : {"libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Svg.dll"})
    {
        if (!qt && std::string(dependency).starts_with("Qt")) continue;
        for (const auto& directory : directories)
            if (fs::is_regular_file(directory / dependency))
            {
                CheckStop(stop);
                const auto path = fs::canonical(directory / dependency);
                digests[PathText(path)] = DigestFile(path);
            }
    }
    return audit::ContentDigest(catalog.identity + group + digests.dump());
}

std::string CmakeLiteral(const std::string& value)
{
    std::string equals = "=";
    while (value.find("]" + equals + "]") != std::string::npos) equals += '=';
    return "[" + equals + "[" + value + "]" + equals + "]";
}

void OwnedDirectory(const fs::path& directory, const fs::path& root)
{
    GuardArtifact(directory, root);
    fs::create_directories(directory);
    GuardArtifact(directory, root);
}

fs::path StageTest(const NativeTest& test, const TestRunConfig& config, const fs::path& root)
{
    GuardArtifact(root, config.outputRoot);
    if (!fs::create_directory(root)) throw std::runtime_error("A fresh per-module controlled context could not be created.");
    for (const auto* path : {"temp", "logs", "profile/Roaming", "profile/Local", "profile/hf", "profile/torch", "ctest"})
        OwnedDirectory(root / path, config.outputRoot);
    Json properties = test.properties;
    if (test.execution.value("cwd", "registered") == "owned-config")
    {
        OwnedDirectory(root / "launch/Config", config.outputRoot);
        properties["WORKING_DIRECTORY"] = PathText(root / "launch");
    }
    else if (!properties.contains("WORKING_DIRECTORY")) properties["WORKING_DIRECTORY"] = PathText(config.buildRoot);
    // Pinned original values are admitted through the complete process environment;
    // keeping a second CTest ENVIRONMENT list could override owned paths or split values.
    properties.erase("ENVIRONMENT");
    std::string file = "add_test(" + CmakeLiteral(test.entry.id);
    for (const auto& argument : test.command) file += " " + CmakeLiteral(argument);
    file += ")\nset_tests_properties(" + CmakeLiteral(test.entry.id) + " PROPERTIES";
    for (const auto& [name, value] : properties.items())
    {
        std::string text;
        if (value.is_array())
        {
            for (const auto& item : value)
            {
                if (!text.empty()) text += ';';
                auto element = item.get<std::string>();
                std::size_t position = 0;
                while ((position = element.find(';', position)) != std::string::npos) { element.insert(position, "\\"); position += 2; }
                text += element;
            }
        }
        else text = value.is_string() ? value.get<std::string>() : value.dump();
        file += " " + name + " " + CmakeLiteral(text);
    }
    file += ")\n";
    const auto directory = root / "ctest";
    WriteFile(directory / "CTestTestfile.cmake", file, config.outputRoot);
    return directory;
}

std::string ExactRegex(const std::string& name)
{
    std::string expression = "^";
    for (const char c : name)
    {
        if (std::string("\\.^$|()[]*+?{}").find(c) != std::string::npos) expression += '\\';
        expression += c;
    }
    return expression + '$';
}

struct JunitObservation
{
    XML_Parser parser = nullptr;
    std::string expected;
    int cases = 0;
    bool matching = false, failed = false, skipped = false, timedOut = false, forbidden = false;
};

void XMLCALL JunitStart(void* data, const XML_Char* element, const XML_Char** attributes)
{
    auto& state = *static_cast<JunitObservation*>(data);
    const std::string name(element);
    if (name == "testcase")
    {
        ++state.cases;
        for (std::size_t index = 0; attributes[index]; index += 2)
        {
            const std::string key(attributes[index]), value(attributes[index + 1]);
            if (key == "name") state.matching = value == state.expected;
            if (key == "status" && value != "run" && value != "passed")
            {
                state.failed = value == "fail";
                state.skipped = value != "fail";
            }
        }
    }
    if (name == "failure" || name == "error")
    {
        state.failed = true;
        for (std::size_t index = 0; attributes[index]; index += 2)
            if (Lower(attributes[index + 1]).find("timeout") != std::string::npos) state.timedOut = true;
    }
    if (name == "skipped" || name == "disabled") state.skipped = true;
}

void XMLCALL JunitDoctype(void* data, const XML_Char*, const XML_Char*, const XML_Char*, int)
{
    auto& state = *static_cast<JunitObservation*>(data);
    state.forbidden = true;
    XML_StopParser(state.parser, XML_FALSE);
}

JunitObservation InspectJunit(const fs::path& path, const std::string& id, const fs::path& root)
{
    const auto bytes = ReadArtifact(path, root, JunitLimit);
    JunitObservation state;
    state.expected = id;
    state.parser = XML_ParserCreate(nullptr);
    if (!state.parser) throw std::runtime_error("JUnit parser could not be created.");
    XML_SetUserData(state.parser, &state);
    XML_SetStartElementHandler(state.parser, JunitStart);
    XML_SetStartDoctypeDeclHandler(state.parser, JunitDoctype);
    const bool valid = XML_Parse(state.parser, bytes.data(), static_cast<int>(bytes.size()), XML_TRUE) == XML_STATUS_OK;
    XML_ParserFree(state.parser);
    state.parser = nullptr;
    if (!valid || state.forbidden || state.cases != 1 || !state.matching) throw std::runtime_error("JUnit did not prove exactly one matching completed registration.");
    return state;
}

Json ResultJson(const TestModuleResult& result, const fs::path& root)
{
    Json row = {{"id", result.id}, {"outcome", result.outcome}, {"detail", result.detail}, {"elapsedMs", result.elapsedMs}, {"exitCode", result.exitCode},
        {"fresh", result.fresh}, {"observedAt", result.observedAt}, {"reusedFrom", result.reusedFrom}, {"logFile", PathText(result.logFile)}, {"junitFile", PathText(result.junitFile)}};
    if (!result.logFile.empty()) GuardArtifact(result.logFile, root);
    if (!result.junitFile.empty()) GuardArtifact(result.junitFile, root);
    if (fs::is_regular_file(result.logFile)) row["logDigest"] = DigestArtifact(result.logFile, root, LogLimit + 8192 + 32);
    if (fs::is_regular_file(result.junitFile)) row["junitDigest"] = DigestArtifact(result.junitFile, root, JunitLimit);
    return row;
}

std::string XmlEscape(const std::string& text)
{
    std::string escaped;
    for (const unsigned char c : text)
    {
        if (c == '&') escaped += "&amp;";
        else if (c == '<') escaped += "&lt;";
        else if (c == '>') escaped += "&gt;";
        else if (c == '"') escaped += "&quot;";
        else if (c >= 32 || c == '\n' || c == '\t' || c == '\r') escaped += static_cast<char>(c);
    }
    return escaped;
}

void SaveCheckpoint(const TestRunSnapshot& snapshot, const TestRunConfig& config, const CatalogData& catalog)
{
    Json rows = Json::array();
    std::string junit = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"Revia existing test launcher\" tests=\"" + std::to_string(snapshot.results.size()) + "\">\n";
    for (const auto& result : snapshot.results)
    {
        rows.push_back(ResultJson(result, config.outputRoot));
        junit += "<testcase name=\"" + XmlEscape(result.id) + "\" time=\"" + std::to_string(result.elapsedMs / 1000.0) + "\">";
        if (result.outcome == "failed" || result.outcome == "timed_out") junit += "<failure message=\"" + XmlEscape(result.outcome + ": " + result.detail) + "\"/>";
        else if (result.outcome != "passed" || !result.fresh) junit += "<skipped message=\"" + XmlEscape(result.fresh ? result.outcome : "No fresh pass: " + result.outcome) + "\"/>";
        junit += "</testcase>\n";
    }
    junit += "</testsuite>\n";
    const Json document = {{"schema", 2}, {"runId", snapshot.runId}, {"phase", snapshot.phase}, {"message", snapshot.message}, {"catalogIdentity", catalog.identity},
        {"executionIdentity", catalog.executionIdentity}, {"executionContext", "Owned staged CTest; original discovered argv and reviewed semantic properties, with explicit controlled environment/CWD redirections. Not an unchanged original CTest-context pass."},
        {"sourceFingerprint", catalog.sourceFingerprint}, {"buildFingerprint", catalog.buildFingerprint}, {"declaredSourceIdentity", config.sourceIdentity},
        {"configuration", config.configuration}, {"fingerprintScope", "Public/Private/Desktop/Tools/Tests/Config and root build/dependency manifests; excludes build/model/runtime-data/user/cache roots and links"},
        {"applicability", snapshot.message.starts_with("Stale context:") ? "stale/unverified: captured inputs changed or post-run revalidation failed" :
            "partial: captured inputs only; external dependency closure and rebuild freshness are unverified"}, {"results", rows}};
    WriteAtomic(snapshot.outputDirectory / "results.json", document.dump(2) + '\n', config.outputRoot);
    WriteAtomic(snapshot.outputDirectory / "results.junit.xml", junit, config.outputRoot);
    Json latest = {{"schema", 1}, {"modules", Json::object()}};
    GuardArtifact(config.outputRoot / "latest.json", config.outputRoot);
    if (fs::is_regular_file(config.outputRoot / "latest.json")) latest = Json::parse(ReadArtifact(config.outputRoot / "latest.json", config.outputRoot, LatestLimit));
    if (latest.value("schema", 0) != 1 || !latest.contains("modules") || !latest.at("modules").is_object() || latest.at("modules").size() > ApprovedLimit)
        throw std::runtime_error("The latest-result pointer is malformed; previous evidence has been retained.");
    latest["directory"] = PathText(snapshot.outputDirectory.filename());
    for (const auto& result : snapshot.results) latest["modules"][result.id] = PathText(snapshot.outputDirectory.filename());
    WriteAtomic(config.outputRoot / "latest.json", latest.dump() + '\n', config.outputRoot);
}

std::map<std::string, TestModuleResult> ReusableResults(const fs::path& directory, const TestRunConfig& config, const CatalogData& catalog)
{
    if (directory.empty()) return {};
    const auto approved = fs::absolute(directory).lexically_normal();
    GuardArtifact(approved, config.outputRoot);
    if (!Within(approved, config.outputRoot) || approved == config.outputRoot) throw std::runtime_error("Resume must name a prior owned run below the output root.");
    const auto document = Json::parse(ReadArtifact(approved / "results.json", config.outputRoot, MetadataLimit));
    if (document.value("schema", 0) != 2 || document.value("catalogIdentity", "") != catalog.identity || catalog.executionIdentity.empty() ||
        document.value("executionIdentity", "") != catalog.executionIdentity ||
        !document.contains("results") || !document.at("results").is_array() || document.at("results").size() > ApprovedLimit)
        throw std::runtime_error("Resume source/build/catalog/config identity differs; prior outcomes remain historical.");
    std::map<std::string, TestModuleResult> reusable;
    for (const auto& row : document.at("results"))
    {
        if (row.value("outcome", "") != "passed") continue;
        TestModuleResult result;
        result.id = row.at("id").get<std::string>();
        result.logFile = actions::Utf8ToPath(row.at("logFile").get<std::string>());
        result.junitFile = actions::Utf8ToPath(row.at("junitFile").get<std::string>());
        GuardArtifact(result.logFile, config.outputRoot);
        GuardArtifact(result.junitFile, config.outputRoot);
        if (!Within(result.logFile, config.outputRoot) || !Within(result.junitFile, config.outputRoot) ||
            DigestArtifact(result.logFile, config.outputRoot, LogLimit + 8192 + 32) != row.value("logDigest", "") ||
            DigestArtifact(result.junitFile, config.outputRoot, JunitLimit) != row.value("junitDigest", ""))
            throw std::runtime_error("Resume artifacts do not match their bounded prior evidence.");
        const auto junit = InspectJunit(result.junitFile, result.id, config.outputRoot);
        if (junit.failed || junit.skipped) throw std::runtime_error("A claimed reusable pass has no successful matching JUnit observation.");
        result.outcome = "passed";
        result.detail = "Historical successful observation reused; no fresh execution.";
        result.elapsedMs = row.at("elapsedMs").get<std::int64_t>();
        result.exitCode = row.at("exitCode").get<std::int64_t>();
        result.observedAt = row.at("observedAt").get<std::string>();
        result.reusedFrom = PathText(approved);
        if (result.exitCode != 0 || result.observedAt.empty() || result.observedAt.size() > 64 || !reusable.emplace(result.id, result).second)
            throw std::runtime_error("A reusable observation has invalid or duplicate provenance.");
    }
    return reusable;
}

fs::path PriorDirectory(const Json& value, const TestRunConfig& config)
{
    const auto relative = actions::Utf8ToPath(value.get<std::string>());
    if (relative.empty() || relative == "." || relative == ".." || relative.has_parent_path() || relative.is_absolute()) throw std::runtime_error("Invalid latest-result directory.");
    const auto directory = config.outputRoot / relative;
    GuardArtifact(directory, config.outputRoot);
    if (!Within(directory, config.outputRoot) || directory == config.outputRoot) throw std::runtime_error("Latest results escape the approved output root.");
    return directory;
}

void LoadLatest(TestRunSnapshot& snapshot, const TestRunConfig& config)
{
    GuardArtifact(config.outputRoot / "latest.json", config.outputRoot);
    if (!fs::is_regular_file(config.outputRoot / "latest.json")) return;
    const auto latest = Json::parse(ReadArtifact(config.outputRoot / "latest.json", config.outputRoot, LatestLimit));
    if (latest.value("schema", 0) != 1 || !latest.at("modules").is_object() || latest.at("modules").size() > ApprovedLimit)
        throw std::runtime_error("Latest-result pointer is unsupported; saved outcomes remain unverified.");
    std::map<fs::path, Json> documents;
    const auto load = [&](const fs::path& directory) -> const Json&
    {
        if (!documents.contains(directory))
        {
            auto document = Json::parse(ReadArtifact(directory / "results.json", config.outputRoot, MetadataLimit));
            if ((document.value("schema", 0) != 1 && document.value("schema", 0) != 2) || !document.at("results").is_array() || document.at("results").size() > ApprovedLimit)
                throw std::runtime_error("Latest result checkpoint is unsupported.");
            documents.emplace(directory, std::move(document));
        }
        return documents.at(directory);
    };
    for (auto& entry : snapshot.catalog)
        if (latest.at("modules").contains(entry.id))
        {
            const auto& document = load(PriorDirectory(latest.at("modules").at(entry.id), config));
            for (const auto& row : document.at("results"))
                if (row.value("id", "") == entry.id)
                {
                    entry.latestOutcome = row.at("outcome").get<std::string>();
                    entry.applicability = document.value("catalogIdentity", "") == snapshot.catalogIdentity ?
                        "historical catalog context matches; selected binary/execution identity is not yet checked; no fresh execution" : "stale: captured source/build/config inputs differ";
                    if (document.value("phase", "") == "running") entry.applicability = "unverified: owning run did not finalize context revalidation";
                    if (document.value("message", "").starts_with("Stale context:")) entry.applicability = "stale: owning run detected changed inputs";
                    if (!row.value("observedAt", "").empty()) entry.applicability += "; observed " + row.at("observedAt").get<std::string>();
                }
        }
    const auto directory = PriorDirectory(latest.at("directory"), config);
    const auto& document = load(directory);
    snapshot.runId = document.value("runId", "");
    snapshot.outputDirectory = directory;
    snapshot.results.clear();
    for (const auto& row : document.at("results"))
    {
        TestModuleResult result;
        result.id = row.at("id").get<std::string>();
        result.outcome = row.at("outcome").get<std::string>();
        result.detail = "Historical checkpoint; no fresh execution. " + row.value("detail", "");
        result.elapsedMs = row.value("elapsedMs", std::int64_t{0});
        result.exitCode = row.value("exitCode", std::int64_t{-1});
        result.observedAt = row.value("observedAt", "");
        result.reusedFrom = PathText(directory);
        result.logFile = actions::Utf8ToPath(row.value("logFile", ""));
        result.junitFile = actions::Utf8ToPath(row.value("junitFile", ""));
        if (!result.logFile.empty()) GuardArtifact(result.logFile, config.outputRoot);
        if (!result.junitFile.empty()) GuardArtifact(result.junitFile, config.outputRoot);
        if (document.value("catalogIdentity", "") != snapshot.catalogIdentity) result.detail = "Stale inputs. " + result.detail;
        snapshot.results.push_back(std::move(result));
    }
}
} // namespace

struct TestRunRuntime::Impl
{
    std::mutex lifecycle;
    mutable std::mutex mutex;
    TestRunConfig config;
    TestRunSnapshot snapshot;
    CatalogData catalog;
    std::jthread worker;

    void Fail(const std::string& message, const std::stop_token stop)
    {
        std::lock_guard lock(mutex);
        snapshot.phase = stop.stop_requested() ? "cancelled" : "failed";
        snapshot.message = utf8::Sanitize(message);
        snapshot.activeTest.clear();
    }

    void PublishCatalog(CatalogData data)
    {
        TestRunSnapshot discovered;
        for (const auto& test : data.tests) discovered.catalog.push_back(test.entry);
        discovered.catalogIdentity = data.identity;
        discovered.phase = "ready";
        discovered.message = "All registrations discovered; only explicitly reviewed safe policy groups may run. Other scopes remain held.";
        try { LoadLatest(discovered, config); }
        catch (const std::exception&) { discovered.message += " Saved latest pointer could not be read; prior outcomes remain unverified."; }
        std::lock_guard lock(mutex);
        catalog = std::move(data);
        snapshot = std::move(discovered);
    }

    void Run(const std::string& group, const fs::path& resume, const std::stop_token stop)
    {
        try
        {
            GuardArtifact(config.outputRoot, config.outputRoot);
            const auto approved = ValidateConfig(config);
            auto current = ReadCatalog(approved, approved.outputRoot / ("inspect-" + actions::NewActionId()), stop);
            if (current.identity != catalog.identity) throw std::runtime_error("Captured source/build/catalog/config inputs changed; discover again before running or resuming.");
            current.executionIdentity = SelectedIdentity(current, group, approved, stop);
            const auto reusable = ReusableResults(resume, approved, current);
            TestRunSnapshot state;
            { std::lock_guard lock(mutex); state = snapshot; }
            state.runId = "test-" + actions::NewActionId();
            GuardArtifact(approved.outputRoot, approved.outputRoot);
            fs::create_directories(approved.outputRoot);
            state.outputDirectory = approved.outputRoot / state.runId;
            GuardArtifact(state.outputDirectory, approved.outputRoot);
            if (!fs::create_directory(state.outputDirectory)) throw std::runtime_error("Could not create a fresh exclusive owned result directory.");
            GuardArtifact(state.outputDirectory, approved.outputRoot);
            for (const auto& test : current.tests)
                if (Member(test, group))
                {
                    TestModuleResult result;
                    result.id = test.entry.id;
                    result.outcome = "not_run";
                    result.detail = "Not started; no process observation.";
                    state.results.push_back(std::move(result));
                }
            { std::lock_guard lock(mutex); snapshot = state; }
            SaveCheckpoint(state, approved, current);
            std::size_t index = 0;
            for (const auto& test : current.tests)
            {
                if (!Member(test, group)) continue;
                auto& result = state.results.at(index++);
                if (stop.stop_requested()) break;
                if (const auto prior = reusable.find(result.id); prior != reusable.end()) result = prior->second;
                else if (test.entry.availability != "runnable")
                {
                    result.outcome = "blocked";
                    result.detail = test.entry.availability;
                    result.observedAt = ObservedAt();
                }
                else
                {
                    state.activeTest = result.id;
                    result.outcome = "unverified";
                    result.detail = "Owned execution admitted; an interrupted observation stays unverified.";
                    const auto stem = std::to_string(index);
                    result.logFile = state.outputDirectory / (stem + ".log");
                    result.junitFile = state.outputDirectory / (stem + ".ctest.junit.xml");
                    { std::lock_guard lock(mutex); snapshot = state; }
                    SaveCheckpoint(state, approved, current);
                    const auto staged = StageTest(test, approved, state.outputDirectory / (stem + "-context"));
                    const auto environment = OwnedEnvironment(approved, test, state.outputDirectory / (stem + "-context"));
                    auto arguments = CtestArguments(approved, staged);
                    const auto seconds = static_cast<int>(test.entry.timeoutSeconds);
                    arguments.insert(arguments.end(), {"-R", ExactRegex(result.id), "--no-tests=error", "--output-on-failure", "--parallel", "1",
                        "--timeout", std::to_string(seconds), "--output-junit", PathText(result.junitFile), "--test-output-size-passed", "8192", "--test-output-size-failed", "8192"});
                    const auto observation = Observe(approved, arguments, state.outputDirectory / (stem + "-scratch"), LogLimit, std::chrono::seconds(seconds + 3), stop, staged, &environment);
                    WriteFile(result.logFile, observation.output + "\n--- stderr ---\n" + observation.error, approved.outputRoot);
                    result.elapsedMs = observation.elapsedMs;
                    result.exitCode = observation.exitCode;
                    result.observedAt = ObservedAt();
                    result.fresh = observation.started;
                    result.outcome = "unverified";
                    result.detail = "No complete matching JUnit observation; process exit alone proves no pass.";
                    GuardArtifact(result.junitFile, approved.outputRoot);
                    if (observation.cancelled) { result.outcome = "cancelled"; result.detail = "Cancelled before a complete owned process observation; owned children joined."; }
                    else if (!observation.started) { result.outcome = "blocked"; result.detail = observation.error; }
                    else if (observation.timedOut) { result.outcome = "timed_out"; result.detail = "The owned process deadline expired and its tree was stopped."; }
                    else if (observation.overflow) result.detail = "The bounded process output budget was exceeded and its tree stopped.";
                    else if (fs::is_regular_file(result.junitFile))
                    {
                        const auto junit = InspectJunit(result.junitFile, result.id, approved.outputRoot);
                        if (junit.timedOut) result.outcome = "timed_out";
                        else if (junit.skipped) result.outcome = "skipped";
                        else if (junit.failed || observation.exitCode != 0) result.outcome = "failed";
                        else result.outcome = "passed";
                        result.detail = "Matching owned staged CTest JUnit observation under reviewed controls; original CTest context is not requalified. External dependency closure remains partial.";
                    }
                }
                state.activeTest.clear();
                SaveCheckpoint(state, approved, current);
                { std::lock_guard lock(mutex); snapshot = state; }
            }
            state.activeTest.clear();
            std::string staleReason;
            if (!stop.stop_requested())
            {
                try
                {
                    const auto after = ReadCatalog(approved, state.outputDirectory / "post-run-inspection", stop);
                    if (after.identity != current.identity || SelectedIdentity(after, group, approved, stop) != current.executionIdentity)
                        staleReason = "Captured source/build/catalog/config or selected executable/helper/plugin inputs changed during execution.";
                }
                catch (const std::exception& error) { staleReason = "Post-run identity could not be verified: " + std::string(error.what()); }
            }
            state.phase = stop.stop_requested() ? "cancelled" : staleReason.empty() ? "completed" : "failed";
            state.message = stop.stop_requested() ? "Cancelled; unstarted modules stay not_run and interrupted work stays unverified." :
                staleReason.empty() ? "Existing observations saved; inspect each outcome. External dependency closure remains unverified." : "Stale context: " + staleReason;
            if (!staleReason.empty())
                for (auto& result : state.results)
                {
                    result.detail = "Stale context; raw exit/JUnit/log evidence retained. " + result.detail;
                    if (result.outcome == "passed") result.outcome = "unverified";
                }
            for (auto& entry : state.catalog)
                for (const auto& result : state.results)
                    if (entry.id == result.id)
                    {
                        entry.latestOutcome = result.outcome;
                        entry.applicability = !staleReason.empty() ? "stale/unverified: captured inputs changed during the run" :
                            result.fresh ? "partial: fresh process observation for captured inputs; external dependencies unverified" :
                            "historical or unverified: no fresh process pass";
                    }
            SaveCheckpoint(state, approved, current);
            std::lock_guard lock(mutex);
            snapshot = std::move(state);
        }
        catch (const std::exception& error) { Fail(error.what(), stop); }
    }
};

TestRunRuntime::TestRunRuntime() : impl(std::make_unique<Impl>()) {}

TestRunRuntime::TestRunRuntime(const TestRunConfig& config) : TestRunRuntime()
{
    std::string error;
    if (!Configure(config, error))
    {
        std::lock_guard lock(impl->mutex);
        impl->snapshot.phase = "failed";
        impl->snapshot.message = error;
    }
}

TestRunRuntime::~TestRunRuntime() { Shutdown(); }

bool TestRunRuntime::Configure(const TestRunConfig& config, std::string& error)
{
    std::lock_guard lifecycle(impl->lifecycle);
    error.clear();
    {
        std::lock_guard lock(impl->mutex);
        if (Busy(impl->snapshot)) { error = "The owned worker is busy; cancel or wait before changing configuration."; return false; }
    }
    try
    {
        const auto approved = ValidateConfig(config);
        if (impl->worker.joinable()) impl->worker.join();
        std::lock_guard lock(impl->mutex);
        impl->config = approved;
        impl->catalog = {};
        impl->snapshot = {};
        impl->snapshot.phase = approved.enabled ? "idle" : "disabled";
        impl->snapshot.message = approved.enabled ? "Developer context configured; discover existing tests first." :
            "Tests are disabled until an explicit trusted developer build is configured.";
        return true;
    }
    catch (const std::exception& exception) { error = utf8::Sanitize(exception.what()); return false; }
}

bool TestRunRuntime::Discover(std::string& error)
{
    std::lock_guard lifecycle(impl->lifecycle);
    error.clear();
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->config.enabled || Busy(impl->snapshot))
        {
            error = !impl->config.enabled ? "Tests require explicit enabled developer configuration." : "The owned test worker is busy.";
            return false;
        }
    }
    if (impl->worker.joinable()) impl->worker.join();
    {
        std::lock_guard lock(impl->mutex);
        impl->snapshot.phase = "discovering";
        impl->snapshot.message = "Discovering CTest metadata and bounded approved input fingerprints.";
    }
    impl->worker = std::jthread([owner = impl.get()](const std::stop_token stop)
        {
            try
            {
                GuardArtifact(owner->config.outputRoot, owner->config.outputRoot);
                const auto config = ValidateConfig(owner->config);
                auto catalog = ReadCatalog(config, config.outputRoot / ("discover-" + actions::NewActionId()), stop);
                CheckStop(stop);
                owner->PublishCatalog(std::move(catalog));
            }
            catch (const std::exception& exception) { owner->Fail(exception.what(), stop); }
        });
    return true;
}

bool TestRunRuntime::Start(const std::string& group, std::string& error, const fs::path& resumeFrom)
{
    std::lock_guard lifecycle(impl->lifecycle);
    error.clear();
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->config.enabled || Busy(impl->snapshot) || impl->catalog.identity.empty() || !GroupName(group) || !impl->catalog.approvedGroups.contains(group))
        {
            error = "Discover a configured idle catalog, then select a literal group explicitly approved by the checked safe policy.";
            return false;
        }
        if (std::none_of(impl->catalog.tests.begin(), impl->catalog.tests.end(), [&](const NativeTest& test) { return Member(test, group); }))
        {
            error = "The reviewed group has no registrations in this developer build.";
            return false;
        }
    }
    if (impl->worker.joinable()) impl->worker.join();
    {
        std::lock_guard lock(impl->mutex);
        impl->snapshot.phase = "running";
        impl->snapshot.message = "Revalidating captured inputs before existing module execution.";
        impl->snapshot.results.clear();
        impl->snapshot.activeTest.clear();
        impl->snapshot.runId.clear();
        impl->snapshot.outputDirectory.clear();
    }
    impl->worker = std::jthread([owner = impl.get(), group, resumeFrom](const std::stop_token stop) { owner->Run(group, resumeFrom, stop); });
    return true;
}

TestRunSnapshot TestRunRuntime::Snapshot() const
{
    std::lock_guard lock(impl->mutex);
    return impl->snapshot;
}

void TestRunRuntime::Cancel()
{
    std::lock_guard lifecycle(impl->lifecycle);
    if (impl->worker.joinable()) impl->worker.request_stop();
}

void TestRunRuntime::Shutdown()
{
    std::lock_guard lifecycle(impl->lifecycle);
    if (impl->worker.joinable()) { impl->worker.request_stop(); impl->worker.join(); }
}

std::string ReadTestRunCheckpoint(const fs::path& outputRoot, const fs::path& directory)
{
    const auto root = fs::absolute(outputRoot).lexically_normal();
    const auto run = fs::absolute(directory).lexically_normal();
    if (run == root) throw std::runtime_error("A saved run must be below the configured output root.");
    GuardArtifact(run, root);
    return ReadArtifact(run / "results.json", root, MetadataLimit);
}
} // namespace revia::runtime
