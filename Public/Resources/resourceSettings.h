#pragma once

#include <string>

// Placement budgets only; services retain process, thread and work ownership.
struct resourceSettings
{
    bool bAutoPlan = true;
    int reserveLogicalCores = 2;
    int minimumFreeRamMiB = 4096;
    // 0 derives a bounded value from total and currently available RAM.
    int llamaPromptCacheMiB = 0;
    // Combined SQLite page+mmap ceiling per connection. Zero derives 1/256 of system
    // RAM, capped at 512 MiB; this is a ceiling, not a preallocation.
    int sqliteCacheMiB = 0;
    int gpuReserveMiB = 1536;
    // How often live usage is sampled against the plan. Zero turns the monitor off; the
    // plan itself is unaffected either way, because observing never re-places a worker.
    int usageSampleSeconds = 2;
    bool bAllowChatModelSplit = false;
    std::string chat = "auto-primary";
    std::string voice = "auto-secondary";
    std::string speechRecognition = "auto-secondary";
    std::string embeddings = "cpu";
};
