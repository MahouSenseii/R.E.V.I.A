#include "Audit/contentDigest.h"
#include "Visual/imageArtifact.h"

#include <filesystem>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void Check(const bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("revia-artifact-verification-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto path = root / "owned.png";
    try
    {
        // A complete, one-pixel PNG fixture; decoding must inspect actual pixel data.
        const unsigned char pixel[] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0, 0, 1, 8, 4, 0, 0, 0,
            181, 28, 12, 2, 0, 0, 0, 11, 73, 68, 65, 84, 120, 218, 99, 100, 248, 15, 0, 1, 5, 1, 1, 39, 24, 227, 102, 0, 0, 0, 0, 73, 69,
            78, 68, 174, 66, 96, 130};
        const std::string bytes(reinterpret_cast<const char*>(pixel), sizeof(pixel));
        std::ofstream(path, std::ios::binary).write(bytes.data(), bytes.size());
        revia::visual::ImageArtifactReceipt receipt;
        receipt.path = path;
        receipt.sha256 = revia::audit::ContentDigest(bytes);
        receipt.jobId = "owned-job";
        receipt.model = "owned-model";
        receipt.width = 1;
        receipt.height = 1;
        std::string error;
        const auto verify = [&]() { return revia::visual::VerifyImageArtifact(path, "owned-job", "owned-model", 1, 1, receipt, error); };
        Check(verify(), "A valid raster was refused: " + error);
        receipt.path = root / "other.png";
        Check(!verify(), "A foreign returned file was accepted.");
        receipt.path = path;
        receipt.jobId = "stale-job";
        Check(!verify(), "Stale generation provenance was accepted.");
        receipt.jobId = "owned-job";
        receipt.model = "different-model";
        Check(!verify(), "Different model provenance was accepted.");
        receipt.model = "owned-model";
        receipt.width = 2;
        Check(!verify(), "Wrong dimensions were accepted.");
        receipt.width = 1;
        receipt.sha256 = std::string(64, '0');
        Check(!verify(), "Different artifact bytes were accepted.");
        const std::string corrupt = "this is not a PNG";
        std::ofstream(path, std::ios::binary).write(corrupt.data(), corrupt.size());
        receipt.sha256 = revia::audit::ContentDigest(corrupt);
        Check(!verify(), "Undecodable bytes were accepted with a matching digest.");
        std::filesystem::remove(path);
        Check(!verify(), "A missing artifact was accepted.");
        std::filesystem::remove(root);
        std::cout << "Image artifact ownership, provenance, digest and decoding tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::filesystem::remove(path);
        std::filesystem::remove(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
