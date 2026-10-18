#include "Workspace/vmwareProvider.h"

#include "Actions/actionTypes.h"
#include "Process/processExecutor.h"
#include "Workspace/guestReceipt.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace revia::workspace
{
namespace
{
using nlohmann::json;
constexpr std::size_t RequestLimit = 524288;
constexpr std::size_t ReceiptLimit = 1048576;

bool Redirected(const std::filesystem::path& path)
{
    std::filesystem::path cursor;
    for (const auto& part : path)
    {
        if (part == "..") return true;
        cursor /= part;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(cursor, error);
        if (std::filesystem::is_symlink(status)) return true;
        if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) return true;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(cursor.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    }
    return false;
}

// The sibling file is outside the managed guest inventory. Its existence conveys
// no ownership: only the non-inherited OS handle/lock does, including after Stop.
class EnrollmentLease final
{
  public:
    explicit EnrollmentLease(const std::filesystem::path& root)
    {
        if (Redirected(root)) throw std::runtime_error("invalid_enrollment_path");
        const auto canonicalRoot = std::filesystem::weakly_canonical(root);
        const auto parent = canonicalRoot.parent_path();
        if (canonicalRoot.filename().empty() || Redirected(parent) || !std::filesystem::is_directory(parent))
            throw std::runtime_error("invalid_enrollment_parent");
        auto name = canonicalRoot.filename();
        name += ".revia-session.lock";
        const auto path = parent / name;
#ifdef _WIN32
        // Pin the checked ancestry against rename/reparse replacement while the
        // lease is held. FILE_SHARE_DELETE is deliberately absent.
        std::filesystem::path cursor;
        std::vector<std::filesystem::path> ancestry;
        for (const auto& part : parent)
        {
            cursor /= part;
            if (cursor == parent.root_name()) continue;
            ancestry.push_back(cursor);
        }
        directories.reserve(ancestry.size());
        for (const auto& ancestor : ancestry)
        {
            const auto directory = CreateFileW(ancestor.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (directory == INVALID_HANDLE_VALUE) { Close(); throw std::runtime_error("invalid_enrollment_parent"); }
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(directory, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            { CloseHandle(directory); Close(); throw std::runtime_error("invalid_enrollment_parent"); }
            directories.push_back(directory);
        }
        file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1 ||
            !SetHandleInformation(file, HANDLE_FLAG_INHERIT, 0))
        { Close(); throw std::runtime_error("invalid_enrollment_lease"); }
#else
        const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directory < 0) throw std::runtime_error("invalid_enrollment_parent");
        file = openat(directory, name.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        close(directory);
        if (file < 0) return;
        struct stat info{};
        if (fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
        { Close(); throw std::runtime_error("invalid_enrollment_lease"); }
        if (flock(file, LOCK_EX | LOCK_NB) != 0) Close();
#endif
    }
    ~EnrollmentLease() { Close(); }
    EnrollmentLease(const EnrollmentLease&) = delete;
    EnrollmentLease& operator=(const EnrollmentLease&) = delete;
    [[nodiscard]] bool Held() const
    {
#ifdef _WIN32
        return file != INVALID_HANDLE_VALUE;
#else
        return file >= 0;
#endif
    }
  private:
    void Close() noexcept
    {
#ifdef _WIN32
        if (file != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(file, INVALID_HANDLE_VALUE));
        for (const auto directory : directories) CloseHandle(directory);
        directories.clear();
#else
        if (file >= 0) close(std::exchange(file, -1));
#endif
    }
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<HANDLE> directories;
#else
    int file = -1;
#endif
};

std::string Identifier()
{
    std::random_device random;
    std::array<unsigned char, 16> bytes{};
    for (auto& value : bytes) value = static_cast<unsigned char>(random());
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        if (index == 4 || index == 6 || index == 8 || index == 10) result += '-';
        result += digits[bytes[index] >> 4];
        result += digits[bytes[index] & 15];
    }
    return result;
}

bool ValidIdentifier(const std::string& value)
{
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        if (index == 8 || index == 13 || index == 18 || index == 23)
        { if (value[index] != '-') return false; }
        else if (!((value[index] >= '0' && value[index] <= '9') || (value[index] >= 'a' && value[index] <= 'f'))) return false;
    }
    return true;
}

std::string Text(const json& data, const char* key, std::size_t limit = 512)
{
    if (!data.contains(key)) return {};
    if (!data[key].is_string()) throw std::runtime_error("invalid_receipt");
    auto value = data[key].get<std::string>();
    if (value.size() > limit) throw std::runtime_error("invalid_receipt");
    return value;
}

json Receipt(const actions::ActionResult& result)
{
    if (!result.process || result.process->cancelled || result.process->timedOut || result.process->outputTruncated ||
        result.process->standardOutput.size() > ReceiptLimit) throw std::runtime_error("transport_failed");
    std::set<std::string> fields;
    bool duplicate = false;
    auto data = json::parse(result.process->standardOutput,
        [&](int depth, json::parse_event_t event, json& value)
        {
            if (depth == 1 && event == json::parse_event_t::key && !fields.insert(value.get<std::string>()).second) duplicate = true;
            return true;
        });
    if (duplicate || !data.is_object() || !data.contains("schemaVersion") || data["schemaVersion"] != 1)
        throw std::runtime_error("invalid_receipt");
    return data;
}

class PrivateRequest final
{
  public:
    PrivateRequest(const std::filesystem::path& root, const std::string& id, const std::string& bytes)
        : path(root / ("request-" + id + ".json"))
    {
        if (bytes.size() > RequestLimit || Redirected(root) || !std::filesystem::is_directory(root))
            throw std::runtime_error("invalid_request_directory");
#ifdef _WIN32
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("private_request_failed");
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> storage(size);
        const bool tokenRead = GetTokenInformation(token, TokenUser, storage.data(), size, &size) != FALSE;
        CloseHandle(token);
        if (!tokenRead) throw std::runtime_error("private_request_failed");
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid))
            throw std::runtime_error("private_request_failed");
        const std::wstring sddl = L"D:P(A;;FA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            throw std::runtime_error("private_request_failed");
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        LocalFree(descriptor);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("private_request_failed");
        DWORD written = 0;
        const bool saved = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (!saved)
        {
            std::error_code error;
            std::filesystem::remove(path, error);
            throw std::runtime_error("private_request_failed");
        }
#else
        const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (file < 0) throw std::runtime_error("private_request_failed");
        const bool saved = write(file, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()) && fsync(file) == 0;
        close(file);
        if (!saved) { std::filesystem::remove(path); throw std::runtime_error("private_request_failed"); }
#endif
    }
    ~PrivateRequest() { std::error_code error; std::filesystem::remove(path, error); }
    PrivateRequest(const PrivateRequest&) = delete;
    PrivateRequest& operator=(const PrivateRequest&) = delete;
    std::filesystem::path path;
};

class VmwareProvider final : public IVirtualMachineProvider
{
  public:
    explicit VmwareProvider(VmwareProviderOptions input) : options(std::move(input)) {}

    VirtualMachineSnapshot EnsureReady(std::stop_token stop) override
    {
        VirtualMachineSnapshot result{VirtualMachineState::Blocked, "readiness_unverified", "The private VM has not proved readiness.", {}};
        try
        {
            ValidateOptions();
            if (!lease)
            {
                auto acquired = std::make_unique<EnrollmentLease>(options.root);
                if (!acquired->Held())
                {
                    result.code = "enrollment_in_use";
                    result.message = "The private VM enrollment is unavailable to this session.";
                    return result;
                }
                lease = std::move(acquired);
            }
            const auto initial = Call("ensure", {"--wait-seconds", "600"}, 1800000, stop);
            auto data = Receipt(initial);
            if (!initial.succeeded || !data.value("ok", false) || !data.value("guestReady", false))
            {
                result.code = Text(data, "code");
                result.message = Text(data, "message");
                return result;
            }
            const GuestIdentity admitted{Text(data, "environmentId", 80), Text(data, "bootId", 80)};
            if (!ValidIdentifier(admitted.environmentId) || !ValidIdentifier(admitted.bootId)) return result;
            // ProcessExecutor closes its child job before returning. Probe in a new job:
            // a VM engine killed with the startup helper must never be admitted as ready.
            const auto probe = Call("status", {}, 60000, stop);
            data = Receipt(probe);
            const GuestIdentity identity{Text(data, "environmentId", 80), Text(data, "bootId", 80)};
            if (!probe.succeeded || !data.value("ok", false) || !data.value("guestReady", false) ||
                identity.environmentId != admitted.environmentId || identity.bootId != admitted.bootId || stop.stop_requested()) return result;
            current = identity;
            VirtualMachineSnapshot ready{VirtualMachineState::Ready, "ready", "The private Ubuntu workspace is ready.", identity};
            // What this guest's worker says it can do. A guest provisioned before the code tools
            // existed is not upgraded, and its tools are then simply not offered.
            if (data.contains("health") && data["health"].is_object())
            {
                const auto& health = data["health"];
                if (health.contains("capabilities") && health["capabilities"].is_array() && health["capabilities"].size() <= 32)
                    for (const auto& capability : health["capabilities"])
                        if (capability.is_string() && capability.get_ref<const std::string&>().size() <= 64)
                            ready.capabilities.push_back(capability.get<std::string>());
                ready.sandboxReady = health.value("sandboxReady", false) == true;
            }
            return ready;
        }
        catch (...) { if (stop.stop_requested()) result.code = "cancelled"; return result; }
    }

    GuestResult Execute(const GuestRequest& request, const GuestLimits& limits, std::stop_token stop) override
    {
        GuestResult result;
        result.code = "guest_request_failed";
        if (!lease || !lease->Held()) { result.code = "enrollment_not_owned"; return result; }
        const bool effect = request.operation == GuestOperation::WriteText || request.operation == GuestOperation::ExecuteProcess ||
            request.operation == GuestOperation::RunPython || request.operation == GuestOperation::ImportFiles;
        actions::ActionResult transport;
        bool submitted = false;
        bool transportReturned = false;
        try
        {
            if (stop.stop_requested()) { result.code = "cancelled"; result.cancelled = true; return result; }
            ValidateOptions();
            const auto id = Identifier();
            const char* operation = nullptr;
            switch (request.operation)
            {
            case GuestOperation::ListDirectory: operation = "list_directory"; break;
            case GuestOperation::ReadText: operation = "read_text"; break;
            case GuestOperation::WriteText: operation = "write_text"; break;
            case GuestOperation::ExecuteProcess: operation = "execute"; break;
            case GuestOperation::RunPython: operation = "run_python"; break;
            case GuestOperation::SearchText: operation = "search_text"; break;
            case GuestOperation::ImportFiles: operation = "import_files"; break;
            }
            if (!operation || !ValidIdentifier(request.expectedIdentity.environmentId) || !ValidIdentifier(request.expectedIdentity.bootId)) return result;
            json payload{{"schemaVersion", 1}, {"requestId", id}, {"environmentId", request.expectedIdentity.environmentId},
                {"bootId", request.expectedIdentity.bootId}, {"operation", operation}, {"path", request.path}, {"content", request.content},
                {"expectedDigest", request.expectedDigest}, {"executable", request.executable}, {"arguments", request.arguments},
                {"workingDirectory", request.workingDirectory}, {"timeoutMs", request.timeoutMs},
                {"limits", {{"maxReadBytes", limits.maxReadBytes}, {"maxWriteBytes", limits.maxWriteBytes},
                    {"maxOutputBytes", limits.maxOutputBytes}, {"maxEntries", limits.maxEntries}, {"maxTimeoutMs", limits.maxTimeoutMs},
                    {"approvedExecutables", limits.approvedExecutables}}}};
            // The review text is for the person approving, never for the guest.
            if (request.operation == GuestOperation::RunPython)
            {
                payload["runId"] = request.runId;
                payload["code"] = request.code;
            }
            else if (request.operation == GuestOperation::SearchText)
            {
                payload["pattern"] = request.pattern;
                payload["fixedStrings"] = request.fixedStrings;
                payload["ignoreCase"] = request.ignoreCase;
                payload["maxMatches"] = request.maxMatches;
            }
            else if (request.operation == GuestOperation::ImportFiles)
            {
                payload["files"] = json::array();
                for (const auto& shared : request.files)
                    payload["files"].push_back({{"path", shared.path}, {"content", shared.content}});
                if (!request.publish.empty()) payload["publish"] = request.publish;
            }
            PrivateRequest file(options.root, id, payload.dump());
            submitted = true;
            transport = Call("request", {"--request-file", actions::PathToUtf8(file.path)},
                std::clamp(request.timeoutMs, 1, 120000) + 20000, stop);
            transportReturned = true;
            const auto data = Receipt(transport);
            if (Text(data, "requestId", 80) != id || Text(data, "environmentId", 80) != request.expectedIdentity.environmentId ||
                Text(data, "bootId", 80) != request.expectedIdentity.bootId) throw std::runtime_error("guest_receipt_mismatch");
            result.identity = request.expectedIdentity;
            result.receiptVerified = data.value("receiptVerified", false);
            result.attempted = data.value("attempted", false);
            result.succeeded = data.value("succeeded", false);
            result.uncertainEffect = data.value("uncertainEffect", false);
            result.code = Text(data, "code");
            result.message = Text(data, "message");
            result.content = Text(data, "content", limits.maxReadBytes);
            result.digest = Text(data, "digest", 64);
            result.standardOutput = Text(data, "standardOutput", limits.maxOutputBytes);
            result.standardError = Text(data, "standardError", limits.maxOutputBytes);
            if (result.standardOutput.size() + result.standardError.size() > limits.maxOutputBytes) throw std::runtime_error("invalid_receipt");
            if (data.contains("exitCode") && !data["exitCode"].is_number_integer()) throw std::runtime_error("invalid_receipt");
            result.exitCode = data.value("exitCode", std::int64_t{-1});
            result.timedOut = data.value("timedOut", false);
            result.cancelled = data.value("cancelled", false);
            result.outputTruncated = data.value("outputTruncated", false);
            if (result.succeeded && (!data.value("ok", false) || !result.attempted || result.cancelled || result.timedOut ||
                result.uncertainEffect || !transport.succeeded ||
                ((request.operation == GuestOperation::ExecuteProcess || request.operation == GuestOperation::RunPython) &&
                    result.exitCode != 0))) throw std::runtime_error("invalid_receipt");
            result.networkIsolated = data.value("networkIsolated", false) == true;
            if (request.operation == GuestOperation::RunPython)
            {
                if (!result.networkIsolated && result.attempted) throw std::runtime_error("invalid_receipt");
                result.artifacts = ParseGuestArtifacts(data);
                if (data.contains("artifactsOmitted"))
                {
                    if (!data["artifactsOmitted"].is_array() || data["artifactsOmitted"].size() > 65) throw std::runtime_error("invalid_receipt");
                    for (const auto& item : data["artifactsOmitted"])
                    {
                        if (!item.is_object()) throw std::runtime_error("invalid_receipt");
                        result.artifactsOmitted.push_back(Text(item, "name", 400) + ": " + Text(item, "reason", 64));
                    }
                }
            }
            if (effect && result.attempted && (result.cancelled || result.timedOut)) result.uncertainEffect = true;
            if (data.contains("entries"))
            {
                if (!data["entries"].is_array() || data["entries"].size() > limits.maxEntries) throw std::runtime_error("invalid_receipt");
                result.entries = data["entries"].get<std::vector<std::string>>();
                for (const auto& entry : result.entries) if (entry.size() > 255) throw std::runtime_error("invalid_receipt");
            }
            if (!result.receiptVerified || stop.stop_requested())
            {
                result.succeeded = false;
                result.attempted = true;
                result.uncertainEffect = effect;
            }
            return result;
        }
        catch (...)
        {
            result.succeeded = false;
            result.attempted = transportReturned ? transport.attempted : submitted;
            result.uncertainEffect = effect && result.attempted;
            result.identity = {};
            result.receiptVerified = false;
            result.cancelled = stop.stop_requested() || (transport.process && transport.process->cancelled);
            result.timedOut = transport.process && transport.process->timedOut;
            result.code = result.cancelled ? "cancelled" : "guest_receipt_unverified";
            result.message = "The guest receipt could not be verified; the request was not retried.";
            return result;
        }
    }

    GuestResult StopGuest() override
    {
        GuestResult result;
        if (!lease || !lease->Held()) { result.code = "enrollment_not_owned"; return result; }
        result.code = "shutdown_unverified";
        try
        {
            const auto transport = Call("stop", {}, 150000, {});
            result.attempted = transport.attempted;
            const auto data = Receipt(transport);
            result.succeeded = transport.succeeded && data.value("ok", false) && Text(data, "state") == "stopped";
            result.code = Text(data, "code");
            result.identity = current;
        }
        catch (...) {}
        return result;
    }

  private:
    void ValidateOptions() const
    {
        if (!options.root.is_absolute() || options.root == options.root.root_path() || Redirected(options.root) ||
            !options.pythonExecutable.is_absolute() || !options.bootstrapScript.is_absolute() ||
            Redirected(options.pythonExecutable) || Redirected(options.bootstrapScript) ||
            !std::filesystem::is_regular_file(options.pythonExecutable) || !std::filesystem::is_regular_file(options.bootstrapScript) ||
            options.ownerId.empty() || options.ownerId.size() > 80 ||
            !std::all_of(options.ownerId.begin(), options.ownerId.end(), [](unsigned char c)
                { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }))
            throw std::runtime_error("invalid_provider_configuration");
    }

    actions::ActionResult Call(const std::string& operation, std::vector<std::string> extra, int timeout, std::stop_token stop)
    {
        ValidateOptions();
        actions::ActionRequest request;
        request.type = actions::ActionType::ExecuteProcess;
        request.process.executable = std::filesystem::canonical(options.pythonExecutable);
        request.process.workingDirectory = std::filesystem::canonical(options.bootstrapScript.parent_path());
        request.process.timeoutMs = timeout;
#ifdef _WIN32
        // VMware is a per-user application. Pass only its required host-owned profile
        // locations; generic process actions keep their existing minimal environment.
        for (const char* name : {"USERPROFILE", "APPDATA", "LOCALAPPDATA", "TEMP", "TMP", "ProgramFiles", "ProgramData", "SystemDrive"})
        {
            const std::string key(name);
            const std::wstring wideKey(key.begin(), key.end());
            const DWORD length = GetEnvironmentVariableW(wideKey.c_str(), nullptr, 0);
            if (!length || length > 32768) continue;
            std::wstring value(length, L'\0');
            const DWORD copied = GetEnvironmentVariableW(wideKey.c_str(), value.data(), length);
            if (!copied || copied >= length) throw std::runtime_error("provider_environment_changed");
            value.resize(copied);
            request.process.environment[key] = actions::PathToUtf8(std::filesystem::path(value));
        }
#endif
        request.process.arguments = {"-I", "-B", actions::PathToUtf8(std::filesystem::canonical(options.bootstrapScript)), operation,
            "--root", actions::PathToUtf8(options.root), "--owner-id", options.ownerId};
        request.process.arguments.insert(request.process.arguments.end(), extra.begin(), extra.end());
        request.beforeEffect = [stop](const std::string&) { return stop.stop_requested() ? "VM operation cancelled." : std::string{}; };
        actions::PolicyDecision decision;
        decision.verdict = actions::PolicyVerdict::Allowed;
        decision.canonicalExecutable = request.process.executable;
        decision.canonicalSource = request.process.workingDirectory;
        decision.processOutputLimitBytes = ReceiptLimit;
        return executor.Execute(request, decision);
    }

    VmwareProviderOptions options;
    process::ProcessExecutor executor;
    GuestIdentity current;
    std::unique_ptr<EnrollmentLease> lease;
};
}

std::shared_ptr<IVirtualMachineProvider> CreateVmwareProvider(VmwareProviderOptions options)
{
    return std::make_shared<VmwareProvider>(std::move(options));
}
}
