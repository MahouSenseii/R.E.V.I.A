#include <filesystem>
#include <iostream>

int RunStudioLive(const std::filesystem::path& source, int modelPort, const std::filesystem::path& evidenceDirectory);

int main(int argc, char** argv)
{
    try
    {
        if (argc != 4) { std::cerr << "Usage: ReviaStudioLive <source-root> <model-port> <evidence-directory>\n"; return 2; }
        return RunStudioLive(std::filesystem::absolute(argv[1]), std::stoi(argv[2]), std::filesystem::absolute(argv[3]));
    }
    catch (const std::exception& error) { std::cerr << "Live verification failed: " << error.what() << '\n'; return 1; }
}
