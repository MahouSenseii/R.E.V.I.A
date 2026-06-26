#include "Core/memoryManager.h"

#include <algorithm>
#include <cctype>
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

        default:

            return false;
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

bool memoryManager::ShouldRemember(const string &message) {
    if (message.empty())
    {
        return false;
    }

    string lowered = message;
    transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char c) { return static_cast<char>(tolower(c)); });

    static const string triggers[] = {
        "remember that",
        "please remember",
        "don't forget",
        "my name is",
        "i like",
        "i don't like",
        "i prefer",
        "i want you to know"
    };

    for (const string& trigger : triggers)
    {
        if (lowered.find(trigger) != string::npos)
        {
            return true;
        }
    }

    return false;
}
