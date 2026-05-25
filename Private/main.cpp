/**
 * Main file for the project
**/

#include <exception>
#include <filesystem>
#include <iostream>

#include "Core/reviaApp.h"

int main(int argc, char** argv)
{

    try
    {
        if (argc > 0)
        {
            const std::filesystem::path exePath = std::filesystem::absolute(argv[0]);
            std::filesystem::current_path(exePath.parent_path());
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "[Warning] Could not set working directory: " << error.what() << "\n";
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
