#include "testSupport.h"
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
    TestGrowthAfterMetadataCheck();
    std::cout << "Bounded file read tests passed.\n";
}