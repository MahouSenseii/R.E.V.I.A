#include "Filesystem/fileSystemExecutor.h"

#include "Audit/contentDigest.h"

#include <algorithm>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::filesystem
{

actions::ActionResult FileSystemExecutor::WriteTextFile(
    const actions::ActionRequest& request, const actions::PolicyDecision& decision) const
{
    actions::ActionResult result;
    result.dryRun = request.dryRun;
    result.backend = "guarded_text_write";
    if (request.dryRun)
    {
        result.succeeded = true;
        result.message = "Dry run: the file would be written only if its prior identity still matches.";
        return result;
    }
#ifndef _WIN32
    result.message = "Exclusive guarded text writing is unavailable on this platform.";
    return result;
#else
    const bool create = request.expectedDigest == "missing";
    HANDLE file = CreateFileW(decision.canonicalSource.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
        create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        result.message = "The file could not be opened exclusively with the expected existence state (Windows error " +
                         std::to_string(GetLastError()) + ").";
        return result;
    }
    struct CloseFile
    {
        HANDLE value;
        bool discard = false;
        ~CloseFile()
        {
            if (discard)
            {
                FILE_DISPOSITION_INFO disposition{TRUE};
                SetFileInformationByHandle(value, FileDispositionInfo, &disposition, sizeof(disposition));
            }
            CloseHandle(value);
        }
    } owner{file, create};
    BY_HANDLE_FILE_INFORMATION information{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(file, &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        information.nNumberOfLinks != 1 || !GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > std::min<std::uintmax_t>(maxReadBytes, 1024U * 1024U))
    {
        result.message = "The prior file is linked, unsupported, or exceeds the bounded read limit.";
        return result;
    }
    std::string prior(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if ((!prior.empty() && (!ReadFile(file, prior.data(), static_cast<DWORD>(prior.size()), &read, nullptr) || read != prior.size())) ||
        (!create && audit::ContentDigest(prior) != request.expectedDigest))
    {
        result.message = "The expected prior content digest no longer matches the file.";
        return result;
    }
    if (request.beforeEffect)
    {
        const auto refusal = request.beforeEffect({});
        if (!refusal.empty())
        {
            result.message = refusal;
            return result;
        }
    }
    result.attempted = true;
    LARGE_INTEGER beginning{};
    DWORD written = 0;
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN) ||
        (!request.value.empty() && (!WriteFile(file, request.value.data(), static_cast<DWORD>(request.value.size()), &written, nullptr) ||
                                       written != request.value.size())) ||
        !SetEndOfFile(file) || !FlushFileBuffers(file))
    {
        result.message = "The guarded write failed; an existing file may contain a partial update and requires reobservation.";
        return result;
    }
    owner.discard = false;
    result.succeeded = true;
    result.message = "Wrote the text file after verifying its prior content identity.";
    result.entries.push_back("sha256:" + audit::ContentDigest(request.value));
    return result;
#endif
}

} // namespace revia::filesystem
