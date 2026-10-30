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
        revia::visual::ImageArtifactExpectation expected;
        expected.path = path;
        expected.jobId = receipt.jobId;
        expected.model = receipt.model;
        expected.width = expected.height = 1;
        expected.pipeline = "StableDiffusionImg2ImgPipeline";
        expected.mode = revia::visual::ImageRequestMode::ReferenceToImage;
        expected.seed = 42;
        expected.requestedSteps = expected.totalInferenceSteps = 4;
        expected.denoisingSteps = 2;
        expected.requestedStrength = expected.effectiveStrength = 0.5;
        expected.sourceId = "source-id";
        expected.sourceDigest = revia::audit::ContentDigest(bytes);
        expected.sourcePreparation = "contain-pad-rgb-v1";
        receipt.mode = expected.mode;
        receipt.provider = expected.provider;
        receipt.pipeline = expected.pipeline;
        receipt.seed = expected.seed;
        receipt.requestedSteps = receipt.totalInferenceSteps = 4;
        receipt.denoisingSteps = 2;
        receipt.requestedStrength = receipt.effectiveStrength = 0.5;
        receipt.sourceId = expected.sourceId;
        receipt.sourceDigest = expected.sourceDigest;
        receipt.preparedSourceDigest = revia::audit::ContentDigest("prepared RGB pixels");
        receipt.sourcePreparation = expected.sourcePreparation;
        const auto strictVerify = [&]() { return revia::visual::VerifyImageArtifact(expected, receipt, error); };
        Check(strictVerify(), "A complete reference receipt was refused: " + error);
        const auto correct = receipt;
        receipt.mode = revia::visual::ImageRequestMode::TextToImage;
        Check(!strictVerify(), "A text-to-image receipt satisfied a reference request.");
        receipt = correct;
        receipt.sourceDigest = revia::audit::ContentDigest("different source");
        Check(!strictVerify(), "A different reference digest was accepted.");
        receipt = correct;
        receipt.seed = 43;
        Check(!strictVerify(), "Another seed was accepted.");
        receipt = correct;
        receipt.denoisingSteps = 0;
        Check(!strictVerify(), "A zero-denoising result was accepted.");
        receipt = correct;
        receipt.effectiveStrength = 0.75;
        Check(!strictVerify(), "A silently changed strength was accepted.");
        receipt = correct;
        receipt.preparedSourceDigest.clear();
        Check(!strictVerify(), "Missing prepared-source provenance was accepted.");
        receipt = correct;
        receipt.provider.clear();
        Check(!strictVerify(), "Missing provider provenance was accepted.");
        receipt = correct;
        receipt.pipeline = "StableDiffusionPipeline";
        Check(!strictVerify(), "The text-to-image pipeline satisfied a reference receipt.");
        receipt = correct;
        receipt.variant = "another-weight-variant";
        Check(!strictVerify(), "A different weight variant was accepted.");
        receipt = correct;
        receipt.sourcePreparation = "crop";
        Check(!strictVerify(), "A silent source preparation change was accepted.");
        receipt = correct;
        revia::visual::ImageSource source{
            "source-id", revia::audit::ContentDigest(bytes), "image/png", std::make_shared<const std::string>(bytes), 1, 1};
        Check(revia::visual::VerifyImageSource(source, error), "Valid immutable source bytes were refused: " + error);
        source.width = 2;
        Check(!revia::visual::VerifyImageSource(source, error), "Source dimensions were trusted without decode.");
        source.width = 1;
        source.digest = std::string(64, '0');
        Check(!revia::visual::VerifyImageSource(source, error), "Source digest was trusted without hashing bytes.");
        source.digest = revia::audit::ContentDigest("invalid PNG");
        source.bytes = std::make_shared<const std::string>("invalid PNG");
        Check(!revia::visual::VerifyImageSource(source, error), "Invalid encoded source bytes were accepted.");
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
