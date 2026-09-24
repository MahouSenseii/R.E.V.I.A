#include "testSupport.h"
#include "Core/utf8.h"
#include "Filesystem/fileSystemExecutor.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

namespace revia::filesystem
{
struct FileSystemExecutorTestAccess
{
    static actions::ActionResult Read(const FileSystemExecutor& executor, std::istream& file)
    { return executor.ReadTextStream(file); }
};
}

namespace
{
using revia::tests::Check;
using revia::filesystem::FileSystemExecutor;
using Access = revia::filesystem::FileSystemExecutorTestAccess;

void TestReadPrimitiveEnforcesLimit()
{
    for (const std::size_t limit : {0, 1, 32, 4095, 4096, 4097})
    {
        FileSystemExecutor executor(limit, 20, 20);
        std::istringstream exact(std::string(limit, 'x'));
        const auto accepted = Access::Read(executor, exact);
        Check(accepted.succeeded && accepted.content.size() == limit,
            "Text exactly at the read limit was rejected or truncated.");
        std::istringstream oversized(std::string(limit + 8192, 'x'));
        const auto rejected = Access::Read(executor, oversized);
        Check(!rejected.succeeded && rejected.attempted && rejected.content.empty() &&
            rejected.message.find("read limit") != std::string::npos,
            "Actual stream read returned oversized content instead of rejecting it.");
        Check(oversized.tellg() == static_cast<std::streamoff>(limit + 1),
            "Oversize detection read more than one byte beyond the limit.");
    }
    FileSystemExecutor executor(std::numeric_limits<std::uintmax_t>::max(), 20, 20);
    std::istringstream small("small text");
    Check(Access::Read(executor, small).content == "small text", "Maximum read limit overflowed.");
    std::istringstream binary(std::string("a\0b", 3));
    const auto result = Access::Read(executor, binary);
    Check(!result.succeeded && result.content.empty() && result.message.find("binary") != std::string::npos,
        "Bounded reader lost null-byte detection.");
    std::istringstream broken("unreadable");
    broken.setstate(std::ios::badbit);
    Check(!Access::Read(executor, broken).succeeded, "Stream failure was reported as success.");
}

void TestTextIsAlwaysReturnedAsUtf8()
{
    FileSystemExecutor executor(4096, 20, 20);
    const auto read = [&executor](const std::string& bytes)
    {
        std::istringstream file(bytes);
        return Access::Read(executor, file);
    };
    // UTF-16 with a byte-order mark, which Notepad called "Unicode", both byte orders.
    // Its zero bytes used to get an ordinary text file refused as binary.
    const auto little = read(std::string("\xFF\xFEh\0i\0 \0\xE9\0=\xD8\x00\xDE", 14));
    Check(little.succeeded && little.content == "hi \xC3\xA9\xF0\x9F\x98\x80",
        "A UTF-16 little-endian text file was not read as its text: " + little.message);
    const auto big = read(std::string("\xFE\xFF\0h\0i", 6));
    Check(big.succeeded && big.content == "hi",
        "A UTF-16 big-endian text file was not read as its text: " + big.message);

    // A file in a legacy code page is not UTF-8, and JSON -- which every prompt and
    // record is -- refuses it. What comes back is always valid, whatever the platform
    // decodes it as.
    const auto legacy = read("caf\xE9 au lait");
    Check(legacy.succeeded && revia::utf8::IsValid(legacy.content) &&
            legacy.content.rfind("caf", 0) == 0 &&
            legacy.content.find(" au lait") != std::string::npos,
        "Text in a legacy code page was returned as malformed UTF-8.");
    const auto plain = read("already UTF-8: caf\xC3\xA9");
    Check(plain.succeeded && plain.content == "already UTF-8: caf\xC3\xA9",
        "Valid UTF-8 was altered on the way through.");
    Check(revia::utf8::Sanitize("a\xFF\xC3") == "a\xEF\xBF\xBD\xEF\xBF\xBD",
        "Malformed bytes were not each replaced by U+FFFD.");
}

void TestGrowthAfterMetadataCheck()
{
    revia::tests::ScopedTestDirectory directory;
    const auto path = directory.root / "growing.txt";
    { std::ofstream file(path, std::ios::binary); file << "small"; }
    Check(std::filesystem::file_size(path) <= 32, "Growth fixture started oversized.");
    // Replace the race with a deterministic change between inspection and reading.
    { std::ofstream file(path, std::ios::binary | std::ios::app); file << std::string(64, 'x'); }
    std::ifstream file(path, std::ios::binary);
    const auto result = Access::Read(FileSystemExecutor(32, 20, 20), file);
    Check(!result.succeeded && result.content.empty(), "Growth after metadata inspection escaped the reader.");
}
}

void RunBoundedFileReadTests()
{
    TestReadPrimitiveEnforcesLimit();
    TestTextIsAlwaysReturnedAsUtf8();
    TestGrowthAfterMetadataCheck();
    std::cout << "Bounded file read tests passed.\n";
}