#include "Core/memoryManager.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "Library/enumLibrary.h"

using namespace std;

memoryManager::memoryManager()
= default;

memoryManager::~memoryManager()
= default;

bool memoryManager::SaveMemory(memoryType type, const string &speaker, const string &message) const
{
    string path;

    switch (type)
    {
        case memoryType::ChatLog:
            path = chatLogPath;
            break;

        case memoryType::ImportantMemory:
            path = memoryPath;
            break;

        case memoryType::Debug:
        case memoryType::System:
            path = debugPath;
            break;
    }

    filesystem::create_directories(filesystem::path(path).parent_path());

    ofstream file(path, std::ios::app);

    if (!file.is_open())
    {
        return false;
    }

    file << speaker << ": " << message << "\n";

    return true;
}

bool memoryManager::ShouldRemember(const std::string &message) {
    if (message.empty())
    {
        return false;
    }

    if (message.find("remember") != std::string::npos)
    {
        return true;
    }

    if (message.find("my name is") != std::string::npos)
    {
        return true;
    }

    if (message.find("I like") != std::string::npos)
    {
        return true;
    }

    if (message.find("I don't like") != std::string::npos)
    {
        return true;
    }

    return false;
}
