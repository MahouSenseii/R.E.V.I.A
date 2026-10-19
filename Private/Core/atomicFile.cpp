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
bool ReplaceOnce(const std::filesystem::path& target, const std::filesystem::path& temporary, std::string& outError)
{
    std::filesystem::path backup = target;
    backup += ".bak";
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
    if (!moved && (!replacing || code == ERROR_FILE_NOT_FOUND || code == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2))
    {
        moved = MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        code = moved ? ERROR_SUCCESS : GetLastError();
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
} // namespace

bool ReplaceFileAtomically(const std::filesystem::path& target, const std::filesystem::path& temporary, std::string& outError)
{
    // Windows refuses to replace a file while another thread or a scanner has it open, and
    // that passes in moments, so a refused swap is retried briefly before it is reported.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (ReplaceOnce(target, temporary, outError))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::error_code error;
    std::filesystem::remove(temporary, error);
    return false;
}

} // namespace revia::core
