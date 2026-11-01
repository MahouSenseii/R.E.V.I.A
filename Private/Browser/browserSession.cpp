#include "Browser/browserSession.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::browser
{
namespace
{

nlohmann::json Grant(const BrowserSettings& settings)
{
    return {{"enabled", settings.enabled}, {"navigate", settings.navigate}, {"interact", settings.interact},
        {"allowLoopback", settings.allowLoopback}, {"approvedOrigins", settings.approvedOrigins}, {"timeoutMs", settings.timeoutMs},
        {"maxTextBytes", settings.maxTextBytes}, {"maxElements", settings.maxElements}, {"maxValueBytes", settings.maxValueBytes}};
}

#ifdef _WIN32
struct Handle
{
    HANDLE value = nullptr;
    ~Handle()
    {
        Reset();
    }
    void Reset(HANDLE next = nullptr)
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = next;
    }
};

std::wstring Quote(const std::wstring& value)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value)
    {
        if (character == L'\\')
        {
            ++slashes;
            continue;
        }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result.push_back(character);
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::filesystem::path NodePath()
{
    const auto* programFiles = _wgetenv(L"ProgramFiles");
    const std::filesystem::path conventional = std::filesystem::path(programFiles ? programFiles : L"C:/Program Files") / "nodejs/node.exe";
    if (std::filesystem::is_regular_file(conventional))
        return conventional;
    wchar_t path[32768]{};
    const auto count = SearchPathW(nullptr, L"node.exe", nullptr, 32768, path, nullptr);
    return count > 0 && count < 32768 ? std::filesystem::path(path) : std::filesystem::path{};
}
#endif

} // namespace

struct BrowserSession::State
{
    std::filesystem::path worker;
    std::filesystem::path root;
    std::filesystem::path profile;
    std::mutex operationMutex;
    std::recursive_mutex handleMutex;
    std::atomic_bool stopped = false;
    std::atomic<std::uint64_t> cancellationRevision = 0;
    std::atomic<std::uint64_t> observationRevision = 0;
    runtime::RuntimeStamp stamp;
    std::string grant;
    std::string pending;
    std::optional<BrowserReceipt> observation;
#ifdef _WIN32
    Handle process, job, input, output;
    std::atomic<std::uint64_t> generation = 0;
    std::mutex terminationMutex;
    std::condition_variable terminationChanged;
    std::uint64_t requestedRevision = 0, processedRevision = 0, requestedGeneration = 0;
    bool terminationPending = false, terminationClosing = false;
    std::thread terminationWorker;

    void RunTermination()
    {
        std::unique_lock lock(terminationMutex);
        for (;;)
        {
            terminationChanged.wait(lock, [&]() { return terminationPending || terminationClosing; });
            if (!terminationPending && terminationClosing)
                return;
            const auto revision = requestedRevision;
            const auto target = requestedGeneration;
            terminationPending = false;
            lock.unlock();
            {
                // Queue ownership never spans the handle wait; callbacks may synchronously Stop.
                std::lock_guard handleLock(handleMutex);
                if (generation.load() == target && job.value)
                    TerminateJobObject(job.value, 1);
            }
            lock.lock();
            processedRevision = std::max(processedRevision, revision);
            terminationChanged.notify_all();
        }
    }

    void DrainTermination()
    {
        std::unique_lock lock(terminationMutex);
        const auto revision = requestedRevision;
        terminationChanged.wait(lock, [&]() { return processedRevision >= revision; });
    }

    void Close()
    {
        std::lock_guard lock(handleMutex);
        if (job.value)
            TerminateJobObject(job.value, 1);
        job.Reset();
        if (process.value)
            WaitForSingleObject(process.value, 2000);
        process.Reset();
        input.Reset();
        output.Reset();
        pending.clear();
        observation.reset();
        if (!profile.empty() && profile.parent_path() == root && profile.filename().wstring().starts_with(L"session-"))
        {
            std::error_code error;
            std::filesystem::remove_all(profile, error);
        }
        profile.clear();
    }

    bool Start(const actions::ActionRequest& request, const std::uint64_t revision, std::string& error)
    {
        std::lock_guard lock(handleMutex);
        Close();
        stopped = false;
        if (!Admitted(revision, error))
        {
            stopped = true;
            return false;
        }
        ++generation;
        const auto node = NodePath();
        if (node.empty() || !worker.is_absolute() || !std::filesystem::is_regular_file(worker) || !root.is_absolute())
        {
            error = "The host-owned browser worker, Node runtime or private runtime directory is unavailable.";
            return false;
        }
        profile = root / ("session-" + actions::NewActionId());
        std::error_code filesystemError;
        std::filesystem::create_directories(profile, filesystemError);
        if (filesystemError)
        {
            error = "The isolated browser profile could not be created.";
            return false;
        }
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle childInput, childOutput, childError;
        if (!CreatePipe(&childInput.value, &input.value, &security, 65536) ||
            !CreatePipe(&output.value, &childOutput.value, &security, 65536) ||
            !SetHandleInformation(input.value, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0))
        {
            error = "The private browser protocol pipes could not be created.";
            Close();
            return false;
        }
        childError.Reset(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &security, OPEN_EXISTING, 0, nullptr));
        if (childError.value == INVALID_HANDLE_VALUE)
        {
            error = "The browser worker error sink could not be opened.";
            Close();
            return false;
        }
        std::wstring environment;
        for (const auto* name : {L"SystemRoot", L"WINDIR", L"TEMP", L"TMP", L"LOCALAPPDATA", L"USERPROFILE"})
        {
            const auto* value = _wgetenv(name);
            if (value)
            {
                environment += name;
                environment += L"=";
                environment += value;
                environment.push_back(L'\0');
            }
        }
        environment.push_back(L'\0');
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<unsigned char> attributes(attributeBytes);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &attributeBytes))
        {
            error = "The browser worker handle allowlist could not be allocated.";
            Close();
            return false;
        }
        const HANDLE inherited[] = {childInput.value, childOutput.value, childError.value};
        const bool attached = UpdateProcThreadAttribute(
            list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, const_cast<HANDLE*>(inherited), sizeof(inherited), nullptr, nullptr);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = childInput.value;
        startup.StartupInfo.hStdOutput = childOutput.value;
        startup.StartupInfo.hStdError = childError.value;
        startup.lpAttributeList = list;
        std::wstring command = Quote(node.wstring()) + L" " + Quote(worker.wstring()) + L" --profile " + Quote(profile.wstring());
        PROCESS_INFORMATION created{};
        error = request.beforeEffect(request.browser.url);
        const BOOL started = attached && Admitted(revision, error) && error.empty() &&
                             CreateProcessW(node.c_str(), command.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                                 environment.data(), worker.parent_path().c_str(), &startup.StartupInfo, &created);
        DeleteProcThreadAttributeList(list);
        if (!started)
        {
            if (error.empty())
                error = "The owned browser worker failed to start.";
            Close();
            return false;
        }
        process.Reset(created.hProcess);
        Handle thread;
        thread.Reset(created.hThread);
        job.Reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job.value, process.value))
        {
            TerminateProcess(process.value, 1);
            error = "The browser worker could not be contained in its child job.";
            Close();
            return false;
        }
        error = request.beforeEffect(request.browser.url);
        if (!Admitted(revision, error) || !error.empty() || ResumeThread(thread.value) == static_cast<DWORD>(-1))
        {
            if (error.empty())
                error = "The browser worker could not resume.";
            Close();
            return false;
        }
        stamp = request.authorityStamp;
        if (!Admitted(revision, error))
        {
            Close();
            return false;
        }
        return true;
    }
#endif

    bool Admitted(const std::uint64_t revision, std::string& error) const
    {
        if (cancellationRevision.load() == revision)
            return true;
        error = "The owned browser operation was cancelled.";
        return false;
    }
};

BrowserSession::BrowserSession(std::filesystem::path worker, std::filesystem::path privateRoot) : state(std::make_unique<State>())
{
    state->worker = std::move(worker);
    state->root = std::move(privateRoot);
#ifdef _WIN32
    state->terminationWorker = std::thread([owner = state.get()]() { owner->RunTermination(); });
#endif
}

BrowserSession::~BrowserSession()
{
    Stop();
    DrainStop();
#ifdef _WIN32
    {
        std::lock_guard lock(state->terminationMutex);
        state->terminationClosing = true;
    }
    state->terminationChanged.notify_all();
    state->terminationWorker.join();
#endif
}

void BrowserSession::RequestStop()
{
#ifdef _WIN32
    {
        std::lock_guard lock(state->terminationMutex);
        state->requestedRevision = state->cancellationRevision.fetch_add(1) + 1;
        state->stopped = true;
        state->requestedGeneration = state->generation.load();
        state->terminationPending = true;
    }
    state->terminationChanged.notify_all();
#else
    ++state->cancellationRevision;
    state->stopped = true;
#endif
}

void BrowserSession::Stop()
{
    ++state->cancellationRevision;
    state->stopped = true;
#ifdef _WIN32
    std::lock_guard lock(state->handleMutex);
    if (state->job.value)
        TerminateJobObject(state->job.value, 1);
#endif
}

void BrowserSession::DrainStop()
{
    std::lock_guard lock(state->operationMutex);
#ifdef _WIN32
    state->DrainTermination();
    state->Close();
#endif
}

std::optional<BrowserReceipt> BrowserSession::Observation() const
{
    const auto revision = state->cancellationRevision.load();
    if (state->stopped || state->observationRevision.load() != revision)
        return std::nullopt;
    std::lock_guard lock(state->operationMutex);
    return state->stopped || state->cancellationRevision.load() != revision || state->observationRevision.load() != revision
        ? std::nullopt : state->observation;
}

actions::ActionResult BrowserSession::Execute(const actions::ActionRequest& request, const BrowserSettings& settings)
{
    const auto revision = state->cancellationRevision.load();
    std::lock_guard lock(state->operationMutex);
    actions::ActionResult result;
    result.backend = "owned_interactive_browser";
    if (!state->Admitted(revision, result.message))
        return result;
    if (!request.beforeEffect || !ValidateSettings(settings, result.message) || !IsApprovedUrl(request.browser.url, settings))
    {
        result.message = "The interactive browser requires runtime admission and an enabled exact origin grant.";
        return result;
    }
    result.message = request.beforeEffect(request.browser.url);
    if (!state->Admitted(revision, result.message) || !result.message.empty())
        return result;
    if (request.dryRun)
    {
        result.succeeded = true;
        result.dryRun = true;
        result.message = "Interactive browser dry run admitted.";
        return result;
    }
#ifndef _WIN32
    result.message = "The interactive browser owner currently requires Windows.";
    return result;
#else
    state->DrainTermination();
    if (!state->Admitted(revision, result.message))
        return result;
    const auto grant = Grant(settings);
    const std::string encodedGrant = grant.dump();
    if (!state->process.value || !state->stamp.SameSession(request.authorityStamp) ||
        state->stamp.taskId != request.authorityStamp.taskId || state->stamp.policyVersion != request.authorityStamp.policyVersion ||
        state->grant != encodedGrant || state->stopped)
    {
        if (request.type != actions::ActionType::BrowserNavigate)
        {
            result.message = "The owned browser session changed; navigate and observe before further interaction.";
            return result;
        }
        if (!state->Start(request, revision, result.message))
            return result;
        state->grant = encodedGrant;
    }
    const auto& input = request.browser;
    const char* operation = request.type == actions::ActionType::BrowserNavigate  ? "navigate"
                            : request.type == actions::ActionType::BrowserObserve ? "observe"
                            : request.type == actions::ActionType::BrowserClick   ? "click"
                                                                                  : "fill";
    const std::string encoded =
        nlohmann::json({{"operation", operation}, {"url", input.url}, {"session", input.session}, {"generation", input.generation},
                           {"element", input.element}, {"value", input.value}, {"grant", grant}})
            .dump() +
        "\n";
    result.message = request.beforeEffect(input.url);
    if (!state->Admitted(revision, result.message) || !result.message.empty())
    {
        state->Close();
        return result;
    }
    DWORD written = 0;
    const bool dispatchedMutation = request.type != actions::ActionType::BrowserObserve;
    result.attempted = dispatchedMutation;
    if (encoded.size() > 65536 || !WriteFile(state->input.value, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr) ||
        written != encoded.size())
    {
        result.message = "The browser request could not be delivered; any partially delivered effect is uncertain.";
        if (result.attempted)
        {
            result.browser.emplace();
            result.browser->uncertainEffect = true;
            result.browser->authorityStamp = request.authorityStamp;
        }
        state->Close();
        return result;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(settings.timeoutMs + 5000);
    for (;;)
    {
        result.message = request.beforeEffect(input.url);
        if (!state->Admitted(revision, result.message) || !result.message.empty() || state->stopped ||
            std::chrono::steady_clock::now() >= deadline)
        {
            if (result.message.empty())
                result.message = state->stopped ? "The owned browser was stopped." : "The owned browser operation timed out.";
            state->Close();
            break;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(state->output.value, nullptr, 0, nullptr, &available, nullptr))
        {
            result.message = "The owned browser disconnected; any dispatched effect is uncertain.";
            state->Close();
            break;
        }
        if (available)
        {
            char buffer[8192];
            DWORD count = 0;
            if (!ReadFile(state->output.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr))
            {
                result.message = "The browser observation could not be read.";
                state->Close();
                break;
            }
            state->pending.append(buffer, count);
            if (state->pending.size() > 262144)
            {
                result.message = "The browser observation exceeded its bound.";
                state->Close();
                break;
            }
            const auto newline = state->pending.find('\n');
            if (newline != std::string::npos)
            {
                try
                {
                    const auto response = nlohmann::json::parse(state->pending.substr(0, newline));
                    state->pending.erase(0, newline + 1);
                    result.message = response.value("message", "Browser returned no evidence.");
                    result.attempted = dispatchedMutation || response.value("attempted", false);
                    result.succeeded = response.value("succeeded", false);
                    if (result.succeeded)
                    {
                        const auto& observed = response.at("receipt");
                        BrowserReceipt receipt;
                        receipt.session = observed.at("session").get<std::string>();
                        receipt.generation = observed.at("generation").get<std::uint64_t>();
                        receipt.url = observed.at("url").get<std::string>();
                        receipt.title = observed.at("title").get<std::string>();
                        receipt.text = observed.at("text").get<std::string>();
                        receipt.fingerprint = observed.at("fingerprint").get<std::string>();
                        receipt.authorityStamp = request.authorityStamp;
                        if (!IsApprovedUrl(receipt.url, settings) || receipt.text.size() > settings.maxTextBytes ||
                            receipt.session.size() > 128 || receipt.generation == 0 || receipt.title.size() > 1024 ||
                            observed.at("elements").size() > settings.maxElements)
                            throw std::runtime_error("The browser receipt exceeded its admitted contract.");
                        for (const auto& element : observed.at("elements"))
                        {
                            BrowserElement field{element.at("id").get<std::string>(), element.at("name").get<std::string>(),
                                element.at("role").get<std::string>(), element.at("clickable").get<bool>(),
                                element.at("editable").get<bool>(), element.at("value").get<std::string>(),
                                element.at("valueAvailable").get<bool>()};
                            if (field.id.empty() || field.id.size() > 128 || field.name.size() > 720 || field.role.size() > 64 ||
                                field.value.size() > settings.maxValueBytes || (field.valueAvailable && !field.editable) ||
                                (!field.valueAvailable && !field.value.empty()))
                                throw std::runtime_error("The browser field exceeded its admitted contract.");
                            receipt.elements.push_back(std::move(field));
                        }
                        result.content = receipt.text;
                        result.browser = std::move(receipt);
                    }
                    const auto refusal = request.beforeEffect(result.browser ? result.browser->url : input.url);
                    if (!state->Admitted(revision, result.message) || !refusal.empty())
                    {
                        if (!refusal.empty())
                            result.message = refusal;
                        result.succeeded = false;
                        state->Close();
                    }
                    if (!result.succeeded && response.value("uncertainEffect", false))
                        state->Close();
                    if (result.succeeded)
                    {
                        state->observation = result.browser;
                        state->observationRevision = revision;
                    }
                }
                catch (const std::exception&)
                {
                    result.succeeded = false;
                    result.message = "The browser returned an invalid bounded observation.";
                    state->Close();
                }
                break;
            }
        }
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!state->Admitted(revision, result.message))
    {
        result.succeeded = false;
        state->Close();
    }
    if (!result.succeeded && result.attempted)
    {
        if (!result.browser)
            result.browser.emplace();
        result.browser->uncertainEffect = true;
        result.browser->authorityStamp = request.authorityStamp;
    }
    return result;
#endif
}

} // namespace revia::browser
