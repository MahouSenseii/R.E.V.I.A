#pragma once

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace revia::skills::storage
{
inline void CheckPath(const std::filesystem::path& path)
{
    if (!path.is_absolute())
        throw std::runtime_error("Storage path must be captured and absolute.");
    std::filesystem::path current;
    for (const auto& part : path)
    {
        current /= part;
        std::error_code error;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(current, error)))
            throw std::runtime_error("Storage path contains a link.");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            throw std::runtime_error("Storage path contains a reparse point.");
#endif
    }
}

inline std::string Read(const std::filesystem::path& path, const std::size_t maximum = 262144)
{
    CheckPath(path);
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > maximum)
        throw std::runtime_error("Storage artifact is absent or oversized.");
    std::ifstream input(path, std::ios::binary);
    std::string result(std::istreambuf_iterator<char>(input), {});
    if (input.bad() || result.size() > maximum)
        throw std::runtime_error("Storage artifact could not be read completely.");
    return result;
}

inline void AtomicWrite(const std::filesystem::path& path, const std::string& bytes)
{
    CheckPath(path);
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    CheckPath(temporary);
#ifdef _WIN32
    FILE* output = _wfopen(temporary.c_str(), L"wb");
#else
    FILE* output = std::fopen(temporary.c_str(), "wb");
#endif
    if (!output)
        throw std::runtime_error("Storage temporary artifact could not be opened.");
    bool success = std::fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size() && std::fflush(output) == 0;
#ifdef _WIN32
    if (success)
        success = _commit(_fileno(output)) == 0;
#else
    if (success)
        success = fsync(fileno(output)) == 0;
#endif
    if (std::fclose(output) != 0)
        success = false;
    if (!success)
        throw std::runtime_error("Storage artifact could not be flushed completely.");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Storage artifact could not be published.");
#else
    std::filesystem::rename(temporary, path);
#endif
}

inline nlohmann::json ReadJson(const std::filesystem::path& path, const std::size_t maximum = 262144)
{
    return nlohmann::json::parse(Read(path, maximum));
}
}
