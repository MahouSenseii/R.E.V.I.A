#pragma once

//
// Logger for tracking errors and warnings
//
#include <string>
#include <source_location>

enum class logSeverity;

class logger
{
public:
    logger();
    ~logger();

    // Log normal activity
    void Log(const std::string& message);

    //Warning but but won't cause a failure
    void Warning(const std::string& reason,const std::source_location& location = std::source_location::current());

    // Errors will result in failure
    void Error(const std::string& reason,const std::source_location& location = std::source_location::current());

    // Check condition and log error if false
    bool Check(bool bCondition,  logSeverity severity, const std::string& reason,const std::source_location& location = std::source_location::current());

private:

};
