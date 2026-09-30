#include "Memory/conversationArchive.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <random>
#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace
{
using namespace std::chrono_literals;

void Check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

class TestDirectory
{
public:
    TestDirectory()
        : parent(std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()))
    {
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt)
        {
            root = parent / ("revia-archive-writer-" + std::to_string(random()) + "-" +
                std::to_string(random()));
            if (std::filesystem::create_directory(root)) return;
        }
        throw std::runtime_error("Could not create a unique archive writer fixture.");
    }
    ~TestDirectory()
    {
        if (root.parent_path() == parent && root.filename().string().starts_with("revia-archive-writer-"))
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    }
    std::filesystem::path root;
private:
    std::filesystem::path parent;
};

struct Hooks
{
    std::mutex mutex;
    std::condition_variable changed;
    bool endEntered = false;
    bool releaseEnd = false;
    bool deleteCompleted = false;
    bool endFinished = false;
    std::atomic<bool> callbackFailed{false};

    void BeforeEnd()
    {
        std::unique_lock lock(mutex);
        endEntered = true;
        changed.notify_all();
        if (!changed.wait_for(lock, 5s, [&] { return releaseEnd; })) callbackFailed = true;
    }
    void AfterDelete()
    {
        std::unique_lock lock(mutex);
        deleteCompleted = true;
        changed.notify_all();
        if (!changed.wait_for(lock, 5s, [&] { return endFinished; })) callbackFailed = true;
    }
};

std::atomic<Hooks*> activeHooks{nullptr};

bool HasPrefix(const char* sql, const char* prefix)
{
    return sql != nullptr && std::strncmp(sql, prefix, std::strlen(prefix)) == 0;
}

void EndingASessionCannotOverwriteTheDeletionCount()
{
    TestDirectory directory;
    revia::memory::ConversationArchive archive((directory.root / "archive.sqlite").string());
    std::string error;
    Check(archive.BeginSession("delete", error) && archive.BeginSession("keep", error), error);
    Check(archive.Record("delete", "user", "Synthetic pottery one", error) &&
        archive.Record("delete", "assistant", "Synthetic pottery two", error), error);

    Hooks hooks;
    activeHooks = &hooks;
    auto ending = std::async(std::launch::async, [&]
    {
        const bool succeeded = archive.EndSession("keep");
        {
            std::lock_guard lock(hooks.mutex);
            hooks.endFinished = true;
        }
        hooks.changed.notify_all();
        return succeeded;
    });
    bool entered = false;
    {
        std::unique_lock lock(hooks.mutex);
        entered = hooks.changed.wait_for(lock, 2s, [&] { return hooks.endEntered; });
    }
    auto deleting = std::async(std::launch::async, [&] { return archive.ForgetSession("delete"); });
    bool overlapped = false;
    {
        std::unique_lock lock(hooks.mutex);
        // The broken implementation deletes while EndSession is paused after Acquire,
        // then EndSession's UPDATE replaces sqlite3_changes64 before it can be read.
        // Serialized writers instead finish EndSession before reaching the DELETE.
        overlapped = hooks.changed.wait_for(lock, 250ms, [&] { return hooks.deleteCompleted; });
        hooks.releaseEnd = true;
    }
    hooks.changed.notify_all();
    const bool ended = ending.get();
    const std::size_t removed = deleting.get();
    activeHooks = nullptr;

    std::cout << "EndSession succeeded=" << ended << "; DELETE overlap=" << overlapped
        << "; reported removed=" << removed << "; expected=2\n";
    Check(entered && !hooks.callbackFailed, "Archive fixture failed to coordinate bounded real writes.");
    Check(ended, "EndSession failed while deletion ran.");
    Check(archive.TotalTurns() == 0 && archive.LoadSession("delete").empty(),
        "Session deletion did not remove the two real archived turns.");
    Check(removed == 2, "EndSession overwrote the committed deletion count: reported " + std::to_string(removed));
    const auto sessions = archive.RecentSessions();
    Check(sessions.size() == 1 && sessions.front().id == "keep",
        "Deleting one session changed another session's metadata.");
}
} // namespace

extern "C" int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
extern "C" int __real_sqlite3_step(sqlite3_stmt*);

extern "C" int __wrap_sqlite3_prepare_v2(sqlite3* database, const char* sql, int length,
    sqlite3_stmt** statement, const char** tail) noexcept
{
    Hooks* hooks = activeHooks.load();
    try
    {
        if (hooks && HasPrefix(sql, "UPDATE conversation_sessions SET ended_at")) hooks->BeforeEnd();
    }
    catch (...) { if (hooks) hooks->callbackFailed = true; }
    return __real_sqlite3_prepare_v2(database, sql, length, statement, tail);
}

extern "C" int __wrap_sqlite3_step(sqlite3_stmt* statement) noexcept
{
    Hooks* hooks = activeHooks.load();
    const bool turnDeletion = hooks && HasPrefix(sqlite3_sql(statement), "DELETE FROM conversation_turns");
    const int result = __real_sqlite3_step(statement);
    try
    {
        if (turnDeletion && result == SQLITE_DONE) hooks->AfterDelete();
    }
    catch (...) { if (hooks) hooks->callbackFailed = true; }
    return result;
}

int main()
{
    try
    {
        EndingASessionCannotOverwriteTheDeletionCount();
        std::cout << "Archive writer tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        activeHooks = nullptr;
        std::cerr << "Archive writer tests failed: " << error.what() << '\n';
        return 1;
    }
}
