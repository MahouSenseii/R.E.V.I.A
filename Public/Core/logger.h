#pragma once


#include <string>
#include <source_location>

enum class logSeverity;

class logger
{
public:
    logger();
    ~logger();


    void Log(const std::string& message);
    void Warning(const std::string& reason,const std::source_location& location = std::source_location::current());
    void Error(const std::string& reason,const std::source_location& location = std::source_location::current());
    bool Check(bool bCondition,  logSeverity severity, const std::string& reason,const std::source_location& location = std::source_location::current());

private:

};
