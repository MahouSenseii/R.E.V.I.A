#include "Resources/resourceUsage.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace revia::resources
{

namespace
{
constexpr std::uint64_t MiB = 1024ull * 1024ull;

#ifdef _WIN32
std::uint64_t FileTimeToMilliseconds(const FILETIME& value)
{
    ULARGE_INTEGER converted{};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart / 10000ull;
}

std::string WideToUtf8(const wchar_t* value)
{
    if (value == nullptr || *value == L'\0')
    {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
    return result;
}
#endif

} // namespace

// "luid_0x00000000_0x0000b8b4_phys_0" and
// "pid_9612_luid_0x00000000_0x0000b8b4_phys_0_eng_3_engtype_3d" both reduce to
// ("luid_0x00000000_0x0000b8b4"), the second also yielding "3d".
//
// The leading process id and the trailing physical-adapter and engine segments all split
// one card across many instances. Every one of them has to be dropped, or a single GPU
// arrives as several devices and each gets a fraction of its own reading.
GpuCounterInstance ParseGpuCounterInstance(const std::string& instanceName)
{
    std::string lowered = instanceName;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });

    const std::size_t luid = lowered.find("luid_");
    if (luid == std::string::npos)
    {
        return {};
    }

    GpuCounterInstance parsed;
    parsed.adapterKey = lowered.substr(luid);
    for (const char* suffix : {"_phys", "_eng"})
    {
        const std::size_t found = parsed.adapterKey.find(suffix);
        if (found != std::string::npos)
        {
            parsed.adapterKey = parsed.adapterKey.substr(0, found);
        }
    }

    static constexpr std::string_view EngineTypeMarker = "_engtype_";
    const std::size_t engineType = lowered.find(EngineTypeMarker);
    if (engineType != std::string::npos)
    {
        parsed.engineType = lowered.substr(engineType + EngineTypeMarker.size());
    }
    return parsed;
}

unsigned int LogicalProcessorCount()
{
#ifdef _WIN32
    return std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
#else
    return 1;
#endif
}

SystemMemoryReading SampleSystemMemory()
{
    SystemMemoryReading reading;
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        reading.totalMiB = memory.ullTotalPhys / MiB;
        reading.availableMiB = memory.ullAvailPhys / MiB;
        reading.measured = true;
    }
#endif
    return reading;
}

std::vector<ProcessUsage> SampleOwnedProcesses()
{
    std::vector<ProcessUsage> owned;
#ifdef _WIN32
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return owned;
    }

    struct Entry
    {
        DWORD parent = 0;
        std::string name;
    };
    std::unordered_map<DWORD, Entry> processes;
    std::unordered_map<DWORD, std::vector<DWORD>> children;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            processes.emplace(entry.th32ProcessID,
                Entry{entry.th32ParentProcessID, WideToUtf8(entry.szExeFile)});
            children[entry.th32ParentProcessID].push_back(entry.th32ProcessID);
        }
        while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    const DWORD self = GetCurrentProcessId();
    std::deque<DWORD> pending{self};
    std::unordered_set<DWORD> visited{self};
    while (!pending.empty())
    {
        const DWORD current = pending.front();
        pending.pop_front();

        ProcessUsage usage;
        usage.processId = current;
        const auto found = processes.find(current);
        usage.name = found == processes.end() ? std::string("(unknown)") : found->second.name;

        const HANDLE handle = current == self
            ? GetCurrentProcess()
            : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, current);
        if (handle != nullptr)
        {
            PROCESS_MEMORY_COUNTERS counters{};
            counters.cb = sizeof(counters);
            if (GetProcessMemoryInfo(handle, &counters, sizeof(counters)))
            {
                usage.workingSetMiB = static_cast<std::uint64_t>(counters.WorkingSetSize) / MiB;
            }
            FILETIME created{};
            FILETIME exited{};
            FILETIME kernel{};
            FILETIME user{};
            if (GetProcessTimes(handle, &created, &exited, &kernel, &user))
            {
                usage.cpuTimeMilliseconds =
                    FileTimeToMilliseconds(kernel) + FileTimeToMilliseconds(user);
            }
            if (current != self)
            {
                CloseHandle(handle);
            }
            owned.push_back(std::move(usage));
        }

        const auto descendants = children.find(current);
        if (descendants == children.end())
        {
            continue;
        }
        for (const DWORD child : descendants->second)
        {
            // A process id is reused after the process ends, so a stale parent field can
            // point back into the tree. Visiting each id once keeps that from looping.
            if (child != current && visited.insert(child).second)
            {
                pending.push_back(child);
            }
        }
    }
#endif
    return owned;
}

GpuAdapterSampler::GpuAdapterSampler()
{
#ifdef _WIN32
    PDH_HQUERY openedQuery = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &openedQuery) != ERROR_SUCCESS)
    {
        return;
    }
    PDH_HCOUNTER openedMemory = nullptr;
    // The English counter name is used deliberately: the localized path differs per
    // Windows display language, and a monitor that works only on English installs is a
    // portability bug waiting to be reported as a missing feature.
    if (PdhAddEnglishCounterW(
            openedQuery,
            L"\\GPU Adapter Memory(*)\\Dedicated Usage",
            0,
            &openedMemory) != ERROR_SUCCESS)
    {
        PdhCloseQuery(openedQuery);
        return;
    }
    // Optional. A machine that cannot report engine utilisation still reports memory,
    // and failing the whole sampler over the second counter would trade a working
    // reading for a missing one.
    PDH_HCOUNTER openedUtilization = nullptr;
    if (PdhAddEnglishCounterW(
            openedQuery,
            L"\\GPU Engine(*)\\Utilization Percentage",
            0,
            &openedUtilization) != ERROR_SUCCESS)
    {
        openedUtilization = nullptr;
    }
    // Some counter types format only after a prior collection, so one is taken here and
    // its result discarded rather than making the first sample report nothing.
    PdhCollectQueryData(openedQuery);
    query = openedQuery;
    memoryCounter = openedMemory;
    utilizationCounter = openedUtilization;
#endif
}

GpuAdapterSampler::~GpuAdapterSampler()
{
#ifdef _WIN32
    if (query != nullptr)
    {
        PdhCloseQuery(static_cast<PDH_HQUERY>(query));
    }
#endif
    query = nullptr;
    memoryCounter = nullptr;
    utilizationCounter = nullptr;
}

bool GpuAdapterSampler::IsAvailable() const
{
    return query != nullptr && memoryCounter != nullptr;
}

bool GpuAdapterSampler::IsUtilizationAvailable() const
{
    return query != nullptr && utilizationCounter != nullptr;
}

std::vector<GpuAdapterReading> GpuAdapterSampler::Sample()
{
    std::vector<GpuAdapterReading> readings;
#ifdef _WIN32
    if (!IsAvailable())
    {
        return readings;
    }
    if (PdhCollectQueryData(static_cast<PDH_HQUERY>(query)) != ERROR_SUCCESS)
    {
        return readings;
    }

    // Formats one multi-instance counter and hands each usable item to the caller. The
    // two-call buffer dance and the per-item status filter are identical for both
    // counters; only the union member read from the value differs.
    const auto forEachItem = [](
        void* counterHandle,
        const DWORD format,
        const auto& visit)
    {
        if (counterHandle == nullptr)
        {
            return;
        }
        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PDH_STATUS status = PdhGetFormattedCounterArrayW(
            static_cast<PDH_HCOUNTER>(counterHandle),
            format,
            &bufferSize,
            &itemCount,
            nullptr);
        if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bufferSize == 0)
        {
            return;
        }
        std::vector<unsigned char> buffer(bufferSize);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        status = PdhGetFormattedCounterArrayW(
            static_cast<PDH_HCOUNTER>(counterHandle),
            format,
            &bufferSize,
            &itemCount,
            items);
        if (status != ERROR_SUCCESS)
        {
            return;
        }
        for (DWORD index = 0; index < itemCount; ++index)
        {
            if (items[index].FmtValue.CStatus != ERROR_SUCCESS &&
                items[index].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA &&
                items[index].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA)
            {
                continue;
            }
            visit(WideToUtf8(items[index].szName), items[index].FmtValue);
        }
    };

    // One adapter appears once per physical engine. Their memory figures are the same
    // adapter total repeated, so the largest is taken rather than the sum.
    std::unordered_map<std::string, std::uint64_t> memoryByAdapter;
    forEachItem(memoryCounter, PDH_FMT_LARGE,
        [&memoryByAdapter](const std::string& instance, const PDH_FMT_COUNTERVALUE& value)
        {
            const std::string key = ParseGpuCounterInstance(instance).adapterKey;
            if (key.empty())
            {
                return;
            }
            const auto bytes = static_cast<std::uint64_t>(
                std::max<LONGLONG>(0, value.largeValue));
            std::uint64_t& stored = memoryByAdapter[key];
            stored = std::max(stored, bytes / MiB);
        });

    // Engine utilisation is reported once per process per engine. Work on one engine
    // adds up across processes, but the engines run concurrently, so the adapter's
    // figure is the busiest engine rather than the total -- which is the same rule Task
    // Manager's GPU column uses, and the reason its number never exceeds 100%.
    std::unordered_map<std::string, std::unordered_map<std::string, double>> engineLoad;
    forEachItem(utilizationCounter, PDH_FMT_DOUBLE,
        [&engineLoad](const std::string& instance, const PDH_FMT_COUNTERVALUE& value)
        {
            const GpuCounterInstance parsed = ParseGpuCounterInstance(instance);
            if (parsed.adapterKey.empty())
            {
                return;
            }
            engineLoad[parsed.adapterKey][parsed.engineType] +=
                std::max(0.0, value.doubleValue);
        });

    std::unordered_map<std::string, double> utilizationByAdapter;
    for (const auto& adapter : engineLoad)
    {
        double busiest = 0.0;
        for (const auto& engine : adapter.second)
        {
            busiest = std::max(busiest, engine.second);
        }
        // Concurrent engines sampled independently can add to a shade over full. The
        // counter cannot mean more than a saturated adapter, so it is reported as one.
        utilizationByAdapter[adapter.first] = std::min(100.0, busiest);
    }

    std::unordered_set<std::string> adapters;
    for (const auto& entry : memoryByAdapter) adapters.insert(entry.first);
    for (const auto& entry : utilizationByAdapter) adapters.insert(entry.first);

    readings.reserve(adapters.size());
    for (const std::string& adapter : adapters)
    {
        GpuAdapterReading reading;
        reading.adapterLuid = adapter;
        const auto memory = memoryByAdapter.find(adapter);
        if (memory != memoryByAdapter.end())
        {
            reading.dedicatedUsedMiB = memory->second;
            reading.memoryMeasured = true;
        }
        const auto utilization = utilizationByAdapter.find(adapter);
        if (utilization != utilizationByAdapter.end())
        {
            reading.utilizationPercent = utilization->second;
            reading.utilizationMeasured = true;
        }
        readings.push_back(std::move(reading));
    }
    std::sort(readings.begin(), readings.end(),
        [](const GpuAdapterReading& left, const GpuAdapterReading& right)
        {
            return left.adapterLuid < right.adapterLuid;
        });
#endif
    return readings;
}

} // namespace revia::resources
