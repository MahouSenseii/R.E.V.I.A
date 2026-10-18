#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace revia::attachments
{

struct ZipEntry
{
    bool found = false;
    bool ok = false;
    std::string data;
    std::string error;
};

// Reads one named member of a ZIP archive held in memory, for DOCX. Only what an
// Office document uses is supported: stored or deflated members, no encryption, no
// ZIP64, no spanning. Offsets and sizes are checked against the buffer before every
// read, and the member is never inflated past `maxBytes`.
[[nodiscard]] ZipEntry ReadZipEntry(std::string_view archive, std::string_view name, std::size_t maxBytes);

} // namespace revia::attachments
