#include "Resources/resourceUsage.h"

#include <algorithm>
#include <cstdio>
#include <deque>
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

// "luid_0x00000000_0x0000b8b4_phys_0" -> "luid_0x00000000_0x0000b8b4". The physical
// engine suffix splits one adapter across several instances; memory is reported per
// adapter, so the suffix is noise here.
std::string AdapterKeyFromInstance(const std::string& instance)
{
    std::string lowered = instance;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    const std::size_t physical = lowered.find("_phys");
    return physical == std::string::npos ? lowered : lowered.substr(0, physical);
}
#endif
} // namespace

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

GpuMemorySampler::GpuMemorySampler()
{
#ifdef _WIN32
    PDH_HQUERY openedQuery = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &openedQuery) != ERROR_SUCCESS)
    {
        return;
    }
    PDH_HCOUNTER openedCounter = nullptr;
    // The English counter name is used deliberately: the localized path differs per
    // Windows display language, and a monitor that works only on English installs is a
    // portability bug waiting to be reported as a missing feature.
    if (PdhAddEnglishCounterW(
            openedQuery,
            L"\\GPU Adapter Memory(*)\\Dedicated Usage",
            0,
            &openedCounter) != ERROR_SUCCESS)
    {
        PdhCloseQuery(openedQuery);
        return;
    }
    // Some counter types format only after a prior collection, so one is taken here and
    // its result discarded rather than making the first sample report nothing.
    PdhCollectQueryData(openedQuery);
    query = openedQuery;
    counter = openedCounter;
#endif
}

GpuMemorySampler::~GpuMemorySampler()
{
#ifdef _WIN32
    if (query != nullptr)
    {
        PdhCloseQuery(static_cast<PDH_HQUERY>(query));
    }
#endif
    query = nullptr;
    counter = nullptr;
}

bool GpuMemorySampler::IsAvailable() const
{
    return query != nullptr && counter != nullptr;
}

std::vector<GpuMemoryReading> GpuMemorySampler::Sample()
{
    std::vector<GpuMemoryReading> readings;
#ifdef _WIN32
    if (!IsAvailable())
    {
        return readings;
    }
    if (PdhCollectQueryData(static_cast<PDH_HQUERY>(query)) != ERROR_SUCCESS)
    {
        return readings;
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(
        static_cast<PDH_HCOUNTER>(counter),
        PDH_FMT_LARGE,
        &bufferSize,
        &itemCount,
        nullptr);
    if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bufferSize == 0)
    {
        return readings;
    }

    std::vector<unsigned char> buffer(bufferSize);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(
        static_cast<PDH_HCOUNTER>(counter),
        PDH_FMT_LARGE,
        &bufferSize,
        &itemCount,
        items);
    if (status != ERROR_SUCCESS)
    {
        return readings;
    }

    // One adapter appears once per physical engine. Their memory figures are the same
    // adapter total repeated, so the largest is taken rather than the sum.
    std::unordered_map<std::string, std::uint64_t> byAdapter;
    for (DWORD index = 0; index < itemCount; ++index)
    {
        if (items[index].FmtValue.CStatus != ERROR_SUCCESS &&
            items[index].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA &&
            items[index].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA)
        {
            continue;
        }
        const std::string key = AdapterKeyFromInstance(WideToUtf8(items[index].szName));
        if (key.empty())
        {
            continue;
        }
        const auto bytes = static_cast<std::uint64_t>(
            std::max<LONGLONG>(0, items[index].FmtValue.largeValue));
        std::uint64_t& stored = byAdapter[key];
        stored = std::max(stored, bytes / MiB);
    }

    readings.reserve(byAdapter.size());
    for (const auto& [adapter, used] : byAdapter)
    {
        readings.push_back({adapter, used});
    }
    std::sort(readings.begin(), readings.end(),
        [](const GpuMemoryReading& left, const GpuMemoryReading& right)
        {
            return left.adapterLuid < right.adapterLuid;
        });
#endif
    return readings;
}

} // namespace revia::resources
