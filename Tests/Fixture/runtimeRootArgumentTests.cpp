#include "../../Desktop/runtimeRootArgument.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main(int argc, char** argv)
{
    try
    {
        Check(argc == 2, "The runtime-root fixture needs its dedicated evidence directory.");
        const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
        const auto valid = root / "valid-install";
        std::filesystem::create_directories(valid / "Config");
        const auto noConfig = root / "without-config";
        std::filesystem::create_directories(noConfig);
        const auto executable = root / "build";
        revia::desktop::RuntimeRootArgument unchanged;
        Check(unchanged.Directory(executable) == executable,
            "Absent runtime-root changed the existing executable-directory launch default.");
        revia::desktop::RuntimeRootArgument selected;
        Check(selected.Accept(valid.string()) && selected.Directory(executable) == valid,
            "The explicit valid runtime root was not selected.");
        Check(!selected.Accept(valid.string()), "Duplicate runtime-root arguments were accepted.");
        const auto unicode = root / std::filesystem::path(u8"r\u00e9via-\u685c");
        std::filesystem::create_directories(unicode / "Config");
        const auto unicodeBytes = unicode.u8string();
        revia::desktop::RuntimeRootArgument unicodeSelected;
        Check(unicodeSelected.Accept(std::string(unicodeBytes.begin(), unicodeBytes.end())) &&
                  unicodeSelected.Directory(executable) == unicode,
            "The explicit runtime root did not preserve its UTF-8 path.");
        for (const auto& invalid : {std::string{}, std::string("relative"), std::string("--ui-smoke-test"),
                 noConfig.string(), (root / "missing").string()})
        {
            revia::desktop::RuntimeRootArgument rejected;
            Check(!rejected.Accept(invalid), "An invalid runtime-root value was accepted.");
        }
        std::cout << "Explicit runtime root validation passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
