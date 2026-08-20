#include "Resources/resourcePlanner.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <regex>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <dxgi1_2.h>
#include <windows.h>
#endif

namespace revia::resources
{

namespace
{
    constexpr std::uint64_t MiB = 1024ull * 1024ull;

    std::string ToUpper(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::toupper(character));
            });
        return value;
    }

    std::filesystem::path ResolveRuntimePath(const std::string& configured)
    {
        const std::filesystem::path value(configured);
        if (value.is_absolute())
        {
            return value.lexically_normal();
        }
        std::error_code error;
        const std::filesystem::path current =
            std::filesystem::absolute(value, error).lexically_normal();
        if (!error && std::filesystem::exists(current))
        {
            return current;
        }
#ifdef _WIN32
        std::vector<wchar_t> module(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (length > 0 && length < module.size())
        {
            const std::filesystem::path executableDirectory =
                std::filesystem::path(std::wstring(module.data(), length)).parent_path();
            for (const std::filesystem::path& root : {
                executableDirectory,
                executableDirectory.parent_path(),
                executableDirectory.parent_path().parent_path()})
            {
                const std::filesystem::path candidate = (root / value).lexically_normal();
                if (std::filesystem::exists(candidate))
                {
                    return candidate;
                }
            }
        }
#endif
        return current;
    }

    std::uint64_t FileMiB(const std::string& configured)
    {
        std::error_code error;
        const std::uint64_t bytes = std::filesystem::file_size(
            ResolveRuntimePath(configured), error);
        return error ? 0 : (bytes + MiB - 1) / MiB;
    }

#ifdef _WIN32
    std::string WideToUtf8(const wchar_t* value)
    {
        if (value == nullptr || *value == L'\0')
        {
            return {};
        }
        const int required = WideCharToMultiByte(
            CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (required <= 1)
        {
            return {};
        }
        std::string output(static_cast<std::size_t>(required), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, value, -1, output.data(), required, nullptr, nullptr);
        output.pop_back();
        return output;
    }

    std::wstring QuoteWindowsArgument(const std::wstring& argument)
    {
        if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        {
            return argument;
        }
        std::wstring quoted = L"\"";
        std::size_t backslashes = 0;
        for (const wchar_t character : argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted.push_back(character);
                backslashes = 0;
                continue;
            }
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
        quoted.append(backslashes * 2, L'\\');
        quoted.push_back(L'\"');
        return quoted;
    }

    std::string CaptureDeviceList(const std::filesystem::path& executable)
    {
        if (!std::filesystem::is_regular_file(executable))
        {
            return {};
        }
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &attributes, 0))
        {
            return {};
        }
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        std::wstring command = QuoteWindowsArgument(executable.wstring()) + L" --list-devices";
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        const std::wstring workingDirectory = executable.parent_path().wstring();
        const BOOL created = CreateProcessW(
            executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startup, &process);
        CloseHandle(writePipe);
        if (!created)
        {
            CloseHandle(readPipe);
            return {};
        }
        CloseHandle(process.hThread);
        const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
        if (wait == WAIT_TIMEOUT)
        {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
        }
        CloseHandle(process.hProcess);

        std::string output;
        char buffer[4096];
        DWORD read = 0;
        while (output.size() < 1024 * 1024 &&
            ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        {
            output.append(buffer, buffer + read);
        }
        CloseHandle(readPipe);
        return output;
    }

    std::vector<GpuDevice> ParseBackendDevices(const std::string& output)
    {
        std::vector<GpuDevice> devices;
        const std::regex pattern(
            R"(^\s*([^:\s]+):\s*(.+)\s+\(([0-9]+)\s+MiB,\s*([0-9]+)\s+MiB free\)\s*$)");
        std::istringstream lines(output);
        std::string line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            std::smatch match;
            if (!std::regex_match(line, match, pattern))
            {
                continue;
            }
            GpuDevice device;
            device.backendId = match[1].str();
            device.name = match[2].str();
            device.totalMemoryMiB = std::stoull(match[3].str());
            device.freeMemoryMiB = std::stoull(match[4].str());
            const std::smatch ordinalMatch = [&device]()
            {
                std::smatch result;
                std::regex_search(device.backendId, result, std::regex(R"(([0-9]+)$)"));
                return result;
            }();
            if (ordinalMatch.size() > 1)
            {
                device.ordinal = std::stoi(ordinalMatch[1].str());
            }
            devices.push_back(std::move(device));
        }
        return devices;
    }

    std::vector<GpuDevice> DetectDxgiAdapters()
    {
        std::vector<GpuDevice> devices;
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(
                __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        {
            return devices;
        }
        for (UINT index = 0;; ++index)
        {
            IDXGIAdapter1* adapter = nullptr;
            const HRESULT result = factory->EnumAdapters1(index, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(result) || adapter == nullptr)
            {
                break;
            }
            DXGI_ADAPTER_DESC1 description{};
            if (SUCCEEDED(adapter->GetDesc1(&description)) &&
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                description.DedicatedVideoMemory >= 512ull * MiB)
            {
                GpuDevice device;
                device.name = WideToUtf8(description.Description);
                device.totalMemoryMiB = description.DedicatedVideoMemory / MiB;
                device.freeMemoryMiB = device.totalMemoryMiB;
                devices.push_back(std::move(device));
            }
            adapter->Release();
        }
        factory->Release();
        return devices;
    }
#endif

    std::vector<GpuDevice> RankedAddressableGpus(const HardwareInventory& hardware)
    {
        std::vector<GpuDevice> devices;
        std::copy_if(hardware.gpus.begin(), hardware.gpus.end(),
            std::back_inserter(devices), [](const GpuDevice& device)
            {
                return !device.backendId.empty();
            });
        std::stable_sort(devices.begin(), devices.end(),
            [](const GpuDevice& left, const GpuDevice& right)
            {
                if (left.totalMemoryMiB != right.totalMemoryMiB)
                {
                    return left.totalMemoryMiB > right.totalMemoryMiB;
                }
                return left.freeMemoryMiB > right.freeMemoryMiB;
            });
        return devices;
    }

    std::string JoinDeviceIds(const std::vector<GpuDevice>& devices)
    {
        std::ostringstream stream;
        for (std::size_t index = 0; index < devices.size(); ++index)
        {
            if (index > 0)
            {
                stream << ',';
            }
            stream << devices[index].backendId;
        }
        return stream.str();
    }

    std::string JoinValues(const std::vector<std::uint64_t>& values)
    {
        std::ostringstream stream;
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            if (index > 0)
            {
                stream << ',';
            }
            stream << values[index];
        }
        return stream.str();
    }

    int AutoRamCacheMiB(
        const HardwareInventory& hardware,
        const resourceSettings& policy)
    {
        const std::uint64_t reserve = static_cast<std::uint64_t>(
            std::max(0, policy.minimumFreeRamMiB));
        const std::uint64_t headroom = hardware.availableSystemMemoryMiB > reserve
            ? hardware.availableSystemMemoryMiB - reserve
            : 0;
        const std::uint64_t totalShare = hardware.totalSystemMemoryMiB / 16;
        const std::uint64_t availableShare = headroom / 2;
        const std::uint64_t chosen = std::min<std::uint64_t>(
            8192, std::min(totalShare, availableShare));
        return chosen < 256 ? 0 : static_cast<int>(chosen);
    }

    int AutoSqliteCacheMiB(const HardwareInventory& hardware)
    {
        if (hardware.totalSystemMemoryMiB == 0)
        {
            return 64;
        }
        return static_cast<int>(std::clamp<std::uint64_t>(
            hardware.totalSystemMemoryMiB / 256, 32, 512));
    }

    std::uint64_t UsableGpuMiB(const GpuDevice& device, const std::uint64_t reserve)
    {
        const std::uint64_t totalAfterReserve = device.totalMemoryMiB > reserve
            ? device.totalMemoryMiB - reserve
            : 0;
        // Exact llama.cpp inventory reports current free memory. A zero value means the
        // backend did not expose it, so total capacity remains the fallback ceiling.
        const std::uint64_t freeBasis = device.freeMemoryMiB > 0
            ? device.freeMemoryMiB
            : device.totalMemoryMiB;
        const std::uint64_t freeAfterReserve = freeBasis > reserve
            ? freeBasis - reserve
            : 0;
        return std::min(totalAfterReserve, freeAfterReserve);
    }

    const GpuDevice* FindDevice(
        const std::vector<GpuDevice>& ranked,
        const std::string& requested,
        const std::size_t automaticIndex)
    {
        if (ranked.empty())
        {
            return nullptr;
        }
        const std::string wanted = ToUpper(requested);
        if (wanted == "AUTO" || wanted == "AUTO-PRIMARY")
        {
            return &ranked[std::min(automaticIndex, ranked.size() - 1)];
        }
        if (wanted == "AUTO-SECONDARY")
        {
            return &ranked[std::min(automaticIndex, ranked.size() - 1)];
        }
        const auto found = std::find_if(ranked.begin(), ranked.end(),
            [&wanted](const GpuDevice& device)
            {
                return ToUpper(device.backendId) == wanted ||
                    ToUpper(device.QwenDevice()) == wanted;
            });
        return found == ranked.end() ? nullptr : &*found;
    }
}

bool GpuDevice::IsCuda() const
{
    return ToUpper(backendId).starts_with("CUDA") && ordinal >= 0;
}

std::string GpuDevice::QwenDevice() const
{
    return IsCuda() ? "cuda:" + std::to_string(ordinal) : "cpu";
}

std::string ResourcePlan::ChatLabel() const
{
    if (chatGpus.empty())
    {
        return chatDevice == "none" ? "CPU" : chatDevice;
    }
    std::ostringstream stream;
    for (std::size_t index = 0; index < chatGpus.size(); ++index)
    {
        if (index > 0)
        {
            stream << " + ";
        }
        stream << chatGpus[index].backendId << " (" << chatGpus[index].name << ')';
    }
    return stream.str();
}

std::string ResourcePlan::VoiceLabel() const
{
    return voiceDevice == "cpu" ? "CPU" : voiceDevice;
}

std::string ResourcePlan::Summary() const
{
    std::ostringstream stream;
    stream << (automatic ? "Automatic" : "Manual") << " resource plan: chat="
        << ChatLabel() << ", voice=" << VoiceLabel() << ", embeddings="
        << (embeddingDevice == "none" ? "CPU" : embeddingDevice) << ", STT="
        << speechRecognitionDevice << ", CPU thread caps chat/background/STT/voice="
        << chatCpuThreads << '/' << embeddingCpuThreads << '/'
        << speechRecognitionThreads << '/' << voiceCpuThreads
        << ", llama RAM cache="
        << llamaPromptCacheMiB << " MiB, SQLite/mmap cache=" << sqliteCacheMiB
        << " MiB, OS reserve=" << reservedSystemMemoryMiB
        << " MiB.";
    if (chatSplitMode != "none")
    {
        stream << " Chat uses " << chatSplitMode << " model splitting.";
    }
    return stream.str();
}

HardwareInventory DetectHardwareInventory(const std::string& llamaServerExecutable)
{
    HardwareInventory inventory;
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        inventory.totalSystemMemoryMiB = memory.ullTotalPhys / MiB;
        inventory.availableSystemMemoryMiB = memory.ullAvailPhys / MiB;
    }
    inventory.logicalProcessors = std::max<DWORD>(
        1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

    const std::filesystem::path executable = ResolveRuntimePath(llamaServerExecutable);
    inventory.gpus = ParseBackendDevices(CaptureDeviceList(executable));
    inventory.exactBackendDevices = !inventory.gpus.empty();
    if (!inventory.exactBackendDevices)
    {
        inventory.gpus = DetectDxgiAdapters();
        inventory.detail = inventory.gpus.empty()
            ? "No addressable accelerator was reported; services will use their safe defaults."
            : "DXGI reported display memory, but llama.cpp device IDs were unavailable; "
              "backend placement remains automatic.";
    }
    else
    {
        inventory.detail = "llama.cpp reported " + std::to_string(inventory.gpus.size()) +
            (inventory.gpus.size() == 1 ? " addressable accelerator." :
                                         " addressable accelerators.");
    }
#else
    (void)llamaServerExecutable;
    inventory.detail = "Hardware inventory is currently implemented for Windows.";
#endif
    return inventory;
}

ResourceRequirements EstimateResourceRequirements(
    const appSettings& settings,
    const bool voiceExpected)
{
    ResourceRequirements requirements;
    // GGUF size is the most reliable portable floor. The extra GiB covers graph/KV and
    // projector transients; llama.cpp --fit still performs the final exact calculation.
    requirements.chatWorkingSetMiB = FileMiB(settings.llm.modelPath) + 1024;
    if (settings.llm.bVisionEnabled)
    {
        requirements.chatWorkingSetMiB += FileMiB(settings.llm.multimodalProjectorPath);
    }
    if (requirements.chatWorkingSetMiB <= 1024)
    {
        // Planning must remain useful before models are installed and in synthetic runs.
        requirements.chatWorkingSetMiB = 7000;
    }
    requirements.voiceExpected = voiceExpected;
    requirements.speechRecognitionGpuEnabled = settings.speechRecognition.bUseGpu;
    requirements.voiceMinimumVramMiB = settings.speech.qwenMinimumFreeVramMiB;
    requirements.baseGpuReserveMiB = std::max(
        settings.resources.gpuReserveMiB,
        settings.llm.autoFitTargetMiB);
    return requirements;
}

ResourcePlan PlanResources(
    const HardwareInventory& hardware,
    const resourceSettings& policy,
    const ResourceRequirements& requirements)
{
    ResourcePlan plan;
    plan.hardware = hardware;
    plan.automatic = policy.bAutoPlan;
    plan.reservedSystemMemoryMiB = policy.minimumFreeRamMiB;
    plan.llamaPromptCacheMiB = policy.llamaPromptCacheMiB > 0
        ? policy.llamaPromptCacheMiB
        : AutoRamCacheMiB(hardware, policy);
    plan.sqliteCacheMiB = policy.sqliteCacheMiB > 0
        ? policy.sqliteCacheMiB
        : AutoSqliteCacheMiB(hardware);

    const unsigned int reserveCores = static_cast<unsigned int>(
        std::max(0, policy.reserveLogicalCores));
    const unsigned int usableCores = hardware.logicalProcessors > reserveCores
        ? hardware.logicalProcessors - reserveCores
        : 1;
    if (usableCores >= 4)
    {
        // Independent processes can overlap, so their caps must fit within the common
        // host budget rather than each assuming it owns the whole CPU.
        unsigned int extras = usableCores - 4;
        const unsigned int chatThreads = 1 + (extras * 3) / 5;
        extras -= chatThreads - 1;
        const unsigned int embeddingThreads = 1 + extras / 3;
        extras -= embeddingThreads - 1;
        const unsigned int recognitionThreads = 1 + extras / 2;
        extras -= recognitionThreads - 1;
        plan.chatCpuThreads = static_cast<int>(chatThreads);
        plan.chatBatchThreads = static_cast<int>(chatThreads);
        plan.embeddingCpuThreads = static_cast<int>(embeddingThreads);
        plan.speechRecognitionThreads = static_cast<int>(recognitionThreads);
        plan.voiceCpuThreads = static_cast<int>(1 + extras);
    }
    else
    {
        // Every process needs one thread to remain live. On hosts this small the values
        // are minimum caps rather than four simultaneously exclusive lanes.
        plan.chatCpuThreads = 1;
        plan.chatBatchThreads = 1;
        plan.embeddingCpuThreads = 1;
        plan.speechRecognitionThreads = 1;
        plan.voiceCpuThreads = 1;
    }

    const std::vector<GpuDevice> ranked = RankedAddressableGpus(hardware);
    if (!policy.bAutoPlan)
    {
        // Manual mode disables capacity-based reassignment, but symbolic selectors still
        // need to become service-specific device names. This makes turning autoPlan off
        // safe even when the default auto-primary/auto-secondary values are unchanged.
        const GpuDevice* chat = FindDevice(ranked, policy.chat, 0);
        if (chat != nullptr)
        {
            plan.chatGpus.push_back(*chat);
            plan.chatDevice = chat->backendId;
        }
        else
        {
            plan.chatDevice = policy.chat == "auto-primary" ? "auto" : policy.chat;
            if (policy.chat == "cpu")
            {
                plan.chatDevice = "none";
            }
        }

        const GpuDevice* voice = FindDevice(ranked, policy.voice, 1);
        plan.voiceDevice = voice != nullptr
            ? voice->QwenDevice()
            : (policy.voice == "auto-secondary" ? "auto" : policy.voice);

        const GpuDevice* embedding = FindDevice(ranked, policy.embeddings, 1);
        plan.embeddingDevice = policy.embeddings == "cpu"
            ? "none"
            : (embedding != nullptr ? embedding->backendId : policy.embeddings);

        const GpuDevice* recognition = FindDevice(ranked, policy.speechRecognition, 1);
        plan.speechRecognitionDevice = recognition != nullptr
            ? recognition->QwenDevice()
            : (policy.speechRecognition == "auto-secondary"
                ? "auto"
                : policy.speechRecognition);
        plan.notes.push_back(
            "Manual placement was preserved; symbolic device selectors were resolved only.");
        return plan;
    }

    const bool chatForcedToCpu = policy.chat == "cpu" || policy.chat == "none";
    const GpuDevice* primary = chatForcedToCpu ? nullptr : FindDevice(ranked, policy.chat, 0);
    if (chatForcedToCpu)
    {
        plan.chatDevice = "none";
        plan.notes.push_back("Chat was assigned to CPU by resource policy.");
    }
    else if (primary == nullptr)
    {
        // Parser drift or a transient --list-devices failure must not convert a working
        // accelerator installation into forced CPU inference.
        plan.chatDevice = "auto";
        plan.notes.push_back("No exact chat accelerator was available; llama.cpp will use CPU/auto offload.");
    }
    else
    {
        plan.chatGpus.push_back(*primary);
        const std::uint64_t reserve = static_cast<std::uint64_t>(
            std::max(0, requirements.baseGpuReserveMiB));
        const std::uint64_t primaryUsable = UsableGpuMiB(*primary, reserve);
        if (policy.bAllowChatModelSplit && policy.chat == "auto-primary" &&
            requirements.chatWorkingSetMiB > primaryUsable && ranked.size() > 1)
        {
            std::uint64_t combinedUsable = 0;
            std::vector<std::uint64_t> proportions;
            for (const GpuDevice& device : ranked)
            {
                const std::uint64_t usable = UsableGpuMiB(device, reserve);
                if (usable > 0)
                {
                    combinedUsable += usable;
                    proportions.push_back(usable);
                }
            }
            if (combinedUsable >= requirements.chatWorkingSetMiB &&
                proportions.size() == ranked.size())
            {
                plan.chatGpus = ranked;
                plan.chatSplitMode = "layer";
                plan.chatTensorSplit = JoinValues(proportions);
                plan.notes.push_back(
                    "The chat working set did not fit the primary GPU, so layer splitting was enabled.");
            }
            else
            {
                plan.notes.push_back(
                    "The chat working set exceeds the primary GPU, but splitting would not fit safely; "
                    "llama.cpp may offload remaining layers to CPU RAM.");
            }
        }
        plan.chatDevice = JoinDeviceIds(plan.chatGpus);
    }

    const bool chatUsesAll = plan.chatSplitMode != "none" && !ranked.empty() &&
        plan.chatGpus.size() == ranked.size();
    const GpuDevice* secondary = FindDevice(ranked, policy.voice, 1);
    if (policy.voice == "cpu")
    {
        plan.voiceDevice = "cpu";
    }
    else if (secondary != nullptr && secondary->IsCuda() && !chatUsesAll)
    {
        // On a one-GPU machine an active voice shares only when the combined VRAM budget
        // fits. On two GPUs the secondary remains independent from chat.
        const bool sameAsChat = !plan.chatGpus.empty() &&
            secondary->backendId == plan.chatGpus.front().backendId;
        const std::uint64_t combinedNeed = requirements.chatWorkingSetMiB +
            static_cast<std::uint64_t>(std::max(0, requirements.voiceMinimumVramMiB)) +
            static_cast<std::uint64_t>(std::max(0, requirements.baseGpuReserveMiB));
        const std::uint64_t secondaryUsable = UsableGpuMiB(
            *secondary,
            static_cast<std::uint64_t>(std::max(0, requirements.baseGpuReserveMiB)));
        const bool independentFits = !sameAsChat && secondaryUsable >=
            static_cast<std::uint64_t>(std::max(0, requirements.voiceMinimumVramMiB));
        const bool sharedFits = sameAsChat && (!requirements.voiceExpected ||
            UsableGpuMiB(*secondary, 0) >= combinedNeed);
        if (independentFits || sharedFits)
        {
            plan.voiceDevice = secondary->QwenDevice();
        }
        else
        {
            plan.voiceDevice = "cpu";
            plan.notes.push_back(
                sameAsChat
                    ? "The active Qwen voice cannot share the chat GPU safely, so it will use CPU."
                    : "The secondary GPU lacks the free VRAM budget for Qwen voice, so it will use CPU.");
        }
    }
    else
    {
        plan.voiceDevice = "cpu";
        if (requirements.voiceExpected)
        {
            plan.notes.push_back(
                "No independent CUDA device remained for Qwen voice; CPU fallback was selected.");
        }
    }

    const GpuDevice* embedding = FindDevice(ranked, policy.embeddings, 1);
    plan.embeddingDevice = policy.embeddings == "cpu" || embedding == nullptr
        ? "none"
        : embedding->backendId;

    const GpuDevice* stt = FindDevice(ranked, policy.speechRecognition, 1);
    constexpr std::uint64_t SpeechRecognitionWorkingSetMiB = 1024;
    const bool sttFits = stt != nullptr && UsableGpuMiB(
        *stt,
        static_cast<std::uint64_t>(std::max(0, requirements.baseGpuReserveMiB))) >=
        SpeechRecognitionWorkingSetMiB;
    plan.speechRecognitionDevice = chatUsesAll ||
        !requirements.speechRecognitionGpuEnabled ||
        policy.speechRecognition == "cpu" ||
        stt == nullptr || !stt->IsCuda() || !sttFits
        ? "cpu"
        : stt->QwenDevice();
    if (chatUsesAll && requirements.speechRecognitionGpuEnabled)
    {
        plan.notes.push_back(
            "Chat consumes every GPU for capacity, so whisper speech recognition will use CPU.");
    }

    if (!plan.chatGpus.empty())
    {
        std::vector<std::uint64_t> fitTargets;
        fitTargets.reserve(plan.chatGpus.size());
        for (const GpuDevice& device : plan.chatGpus)
        {
            std::uint64_t target = static_cast<std::uint64_t>(
                std::max(0, requirements.baseGpuReserveMiB));
            if (requirements.voiceExpected && device.QwenDevice() == plan.voiceDevice)
            {
                target += static_cast<std::uint64_t>(
                    std::max(0, requirements.voiceMinimumVramMiB));
            }
            fitTargets.push_back(target);
        }
        plan.chatFitTargets = JoinValues(fitTargets);
    }
    return plan;
}

void ApplyResourcePlan(const ResourcePlan& plan, appSettings& settings)
{
    settings.resources.llamaPromptCacheMiB = plan.llamaPromptCacheMiB;
    settings.resources.sqliteCacheMiB = plan.sqliteCacheMiB;
    settings.llm.device = plan.chatDevice;
    settings.llm.splitMode = plan.chatSplitMode;
    settings.llm.tensorSplit = plan.chatTensorSplit;
    settings.llm.fitTargetMiB = plan.chatFitTargets;
    settings.llm.cpuThreads = plan.chatCpuThreads;
    settings.llm.cpuBatchThreads = plan.chatBatchThreads;
    settings.llm.ramCacheMiB = plan.llamaPromptCacheMiB;
    settings.llm.modelLoadMode = "mmap";
    settings.llm.reservedVramMiB = 0;

    settings.speech.qwenDevice = plan.voiceDevice;
    settings.speech.qwenCpuThreads = plan.voiceCpuThreads;
    settings.embedding.device = plan.embeddingDevice;
    settings.embedding.cpuThreads = plan.embeddingCpuThreads;
    settings.embedding.cpuBatchThreads = plan.embeddingCpuThreads;
    settings.embedding.ramCacheMiB = 0;
    settings.embedding.modelLoadMode = "mmap";
    settings.speechRecognition.device = plan.speechRecognitionDevice;
    settings.speechRecognition.bUseGpu = plan.speechRecognitionDevice != "cpu";
    settings.speechRecognition.threads = plan.speechRecognitionThreads;
}

} // namespace revia::resources
