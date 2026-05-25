#pragma once

#include <string>
#include <vector>


class longTermMemory
{
public:
    longTermMemory();
    ~longTermMemory();

    std::vector<std::string> Load() const;
    bool HasMemories() const;
    std::string BuildPromptBlock() const;

private:

    std::string memoryPath = "Memory/revia_memory.log";
};
