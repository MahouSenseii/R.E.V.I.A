#include "Core/atomicFile.h"

#include <chrono>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revia::core
{

namespace
{
std::filesystem::path BackupOf(const std::filesystem::path& target)
{
    std::filesystem::path backup = target;
    backup += ".bak";
    return backup;
}

// Moves the backup back to the target name, never over a file that is already there.
bool RestoreBackup(const std::filesystem::path& target)
{
#ifdef _WIN32
    return MoveFileExW(BackupOf(target).c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    if (std::filesystem::exists(target, error))
        return false;
    std::filesystem::rename(BackupOf(target), target, error);
    return !error;
#endif
}

bool ReplaceOnce(const std::filesystem::path& target, const std::filesystem::path& temporary, std::string& outError)
{
    const std::filesystem::path backup = BackupOf(target);
    std::error_code error;
    const bool replacing = std::filesystem::exists(target, error) && !error;

#ifdef _WIN32
    // ReplaceFileW swaps the new file in and renames the old one to the backup name as one
    // operation, keeping the original's attributes. A first write has nothing to replace, and
    // ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 means the old file already went to the backup name
    // while the new one kept its own, so in both cases the new file is simply moved in.
    bool moved = replacing &&
        ReplaceFileW(target.c_str(), temporary.c_str(), backup.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != 0;
    DWORD code = moved ? ERROR_SUCCESS : GetLastError();
    const bool halfway = !moved && code == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2;
    if (!moved && (!replacing || code == ERROR_FILE_NOT_FOUND || halfway))
    {
        moved = MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        code = moved ? ERROR_SUCCESS : GetLastError();
        if (!moved && halfway)
        {
            // The target name is empty now. Put the old file back before anything else, so
            // a reader in the meantime, or a start after a crash, still finds it.
            static_cast<void>(RestoreBackup(target));
        }
    }
    if (!moved)
    {
        outError = "Windows could not replace " + target.filename().string() + " (error " + std::to_string(code) + ").";
        return false;
    }
#else
    if (replacing)
    {
        std::filesystem::copy_file(target, backup, std::filesystem::copy_options::overwrite_existing, error);
        if (error)
        {
            outError = "Could not keep a backup of " + target.filename().string() + ": " + error.message();
            return false;
        }
    }
    std::filesystem::rename(temporary, target, error);
    if (error)
    {
        outError = "Could not replace " + target.filename().string() + ": " + error.message();
        return false;
    }
#endif
    outError.clear();
    return true;
}

#ifdef _WIN32
// ReplaceFileW has no write-through option, so the new contents are on disk before the swap
// rather than only in the cache, as the repo's other replacers make sure too.
void FlushToDisk(const std::filesystem::path& path)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    static_cast<void>(FlushFileBuffers(file));
    CloseHandle(file);
}
#endif
} // namespace

bool ReplaceFileAtomically(const std::filesystem::path& target, const std::filesystem::path& temporary, std::string& outError)
{
    std::error_code error;
    const bool hadTarget = std::filesystem::exists(target, error) && !error;
#ifdef _WIN32
    FlushToDisk(temporary);
#endif
    // Windows refuses to replace a file while another thread or a scanner has it open, and
    // that passes in moments, so a refused swap is retried briefly before it is reported.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (ReplaceOnce(target, temporary, outError))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (hadTarget && !std::filesystem::exists(target, error) && !RestoreBackup(target))
    {
        // The old file is only under the backup name and could not be moved back, so the
        // new one is not deleted: between them, nothing the owner saved is lost.
        outError += " The previous file is " + BackupOf(target).filename().string() + " and the new one " +
                    temporary.filename().string() + ".";
        return false;
    }
    std::filesystem::remove(temporary, error);
    return false;
}

} // namespace revia::core
