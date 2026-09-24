/**
 * Main file for the project
**/

#include <exception>
#include <filesystem>
#include <iostream>
#include <system_error>

#include "Core/reviaApp.h"
#include "Core/runtimePath.h"

int main(int argc, char** argv)
{

    if (const std::filesystem::path directory =
            revia::core::ProgramDirectory(argc > 0 ? argv[0] : nullptr);
        !directory.empty())
    {
        std::error_code error;
        std::filesystem::current_path(directory, error);
        if (error)
        {
            std::cerr << "[Warning] Could not set working directory: " << error.message()
                << "\n";
        }
    }

    try
    {
        reviaApp app;
        app.Run();
    }
    catch (const std::exception& error)
    {
        std::cerr << "[Fatal] Unhandled exception: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
