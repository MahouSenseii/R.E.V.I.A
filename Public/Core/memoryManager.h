#pragma once

#include "iostream"

enum class memoryType;

class memoryManager
{
public:
    memoryManager();
    ~memoryManager();

    bool SaveMemory(memoryType type,const std::string& speaker,const std::string& message) const;

    bool ShouldRemember(const std::string& message);
private:
    std::string chatLogPath = "Logs/chat.log";
    std::string memoryPath = "Memory/revia_memory.log";
    std::string debugPath = "Logs/debug.log";
};
