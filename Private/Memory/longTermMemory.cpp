#include "Memory/longTermMemory.h"

#include <fstream>
#include <sstream>

longTermMemory::longTermMemory() = default;

longTermMemory::~longTermMemory() = default;

std::vector<std::string> longTermMemory::Load() const
{
    std::vector<std::string> lines;

    std::ifstream file(memoryPath);

    if (!file.is_open())
    {
        return lines;
    }

    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty())
        {
            lines.push_back(line);
        }
    }

    return lines;
}

bool longTermMemory::HasMemories() const
{
    return !Load().empty();
}

std::string longTermMemory::BuildPromptBlock() const
{
    const std::vector<std::string> lines = Load();

    if (lines.empty())
    {
        return "";
    }

    std::ostringstream stream;
    stream << "Known facts the user has asked you to remember:\n";

    for (const std::string& line : lines)
    {
        stream << "- " << line << "\n";
    }

    return stream.str();
}
