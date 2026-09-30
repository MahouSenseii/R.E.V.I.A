#include "Core/appSettings.h"
#include "testSupport.h"
#include "Core/preferenceStore.h"
#include "Memory/conversationArchive.h"

#include <barrier>
#include <cmath>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <thread>

namespace
{
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using revia::core::PreferenceStore;
using revia::memory::ConversationArchive;

void TestPreferenceNumbersRejectNonFiniteAndFractionalIntegers()
{
    ScopedTestDirectory directory;
    PreferenceStore store(directory.root / "preferences.json");
    Check(store.Set("llm.temperature", "0.75").succeeded &&
        store.Set("speech.volume", "45").succeeded, "Preference fixture could not be saved.");
    const auto before = store.Load();
    for (const std::string value : {"nan", "NaN", "inf", "-inf"})
    {
        Check(!store.Set("llm.temperature", value).succeeded,
            "A non-finite temperature preference was accepted: " + value);
        Check(!store.Set("speech.volume", value).succeeded,
            "A non-finite integer preference was accepted: " + value);
    }
    Check(!store.Set("speech.volume", "24.75").succeeded,
        "A fractional integer preference was silently truncated.");
    Check(store.Load() == before, "A refused number changed the saved preferences.");
    Check(store.Set("speech.volume", "2e1").succeeded &&
        store.Load().at("speech.volume") == "20", "A valid whole number in scientific notation was refused.");
}

void TestPersistedPreferencesValidateBeforeApplication()
{
    ScopedTestDirectory directory;
    const auto path = directory.root / "preferences.json";
    {
        std::ofstream file(path);
        file << nlohmann::json{
            {"speech.volume", "999"}, {"speech.rate", "2.5"},
            {"initiative.maxPerHour", "nan"}, {"resources.usageSampleSeconds", "-1"},
            {"llm.temperature", "nan"}, {"resources.voiceDevice", "invalid-device"},
            {"speech.enabled", "off"}, {"bargeIn.enabled", "YES"},
            {"activeProfile", "fixture"}, {"approvedRoots", "forbidden"}}.dump();
    }
    PreferenceStore store(path);
    appSettings settings;
    settings.speech.volume = 37;
    settings.speech.rate = -2;
    settings.initiative.maxUtterancesPerHour = 4;
    settings.resources.usageSampleSeconds = 15;
    settings.resources.voice = "cpu";
    settings.llm.temperature = 0.65F;
    settings.speech.bEnabled = true;
    settings.bargeIn.bEnabled = false;
    store.Apply(settings);
    Check(settings.speech.volume == 37 && settings.speech.rate == -2 &&
        settings.initiative.maxUtterancesPerHour == 4 &&
        settings.resources.usageSampleSeconds == 15 && settings.resources.voice == "cpu" &&
        std::isfinite(settings.llm.temperature) && std::abs(settings.llm.temperature - 0.65F) < 0.0001F,
        "An invalid saved preference bypassed its range or type while loading.");
    Check(!settings.speech.bEnabled && settings.bargeIn.bEnabled && settings.activeProfile == "fixture",
        "Valid legacy boolean or text preferences stopped applying.");
    Check(!store.Load().contains("approvedRoots"), "A persisted authority key was accepted.");
    {
        std::ofstream file(path);
        file << R"({"resources.voiceDevice":"auto","speech.volume":"100","llm.temperature":"2"})";
    }
    store.Apply(settings);
    Check(settings.resources.voice == "auto-secondary" && settings.speech.volume == 100 &&
        settings.llm.temperature == 2.0F, "Valid boundary values or the legacy voice alias stopped applying.");
}

void TestConcurrentPreferenceWritesRetainAcknowledgedValues()
{
    ScopedTestDirectory directory;
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const auto path = directory.root / ("preferences-" + std::to_string(attempt) + ".json");
        PreferenceStore store(path);
        std::barrier start(3);
        bool speechSaved = false, bargeInSaved = false;
        std::jthread speech([&] {
            start.arrive_and_wait(); speechSaved = store.Set("speech.enabled", "off").succeeded;
        });
        std::jthread bargeIn([&] {
            start.arrive_and_wait(); bargeInSaved = store.Set("bargeIn.enabled", "off").succeeded;
        });
        start.arrive_and_wait();
        speech.join(); bargeIn.join();
        const auto values = PreferenceStore(path).Load();
        Check(speechSaved && bargeInSaved && values.contains("speech.enabled") &&
            values.contains("bargeIn.enabled") && values.at("speech.enabled") == "false" &&
            values.at("bargeIn.enabled") == "false", "Concurrent preference writes lost an acknowledged value.");

        Check(store.Set("speech.volume", "45").succeeded, "Clear race fixture could not be saved.");
        std::barrier clearStart(3);
        bool volumeCleared = false, temperatureSaved = false;
        std::jthread clear([&] {
            clearStart.arrive_and_wait(); volumeCleared = store.Clear("speech.volume").succeeded;
        });
        std::jthread temperature([&] {
            clearStart.arrive_and_wait(); temperatureSaved = store.Set("llm.temperature", "0.9").succeeded;
        });
        clearStart.arrive_and_wait();
        clear.join(); temperature.join();
        const auto afterClear = PreferenceStore(path).Load();
        Check(volumeCleared && temperatureSaved && !afterClear.contains("speech.volume") &&
            afterClear.contains("llm.temperature") && afterClear.contains("speech.enabled") &&
            afterClear.contains("bargeIn.enabled"), "Concurrent save and clear lost or restored an acknowledged value.");
    }
}

void ExecuteSql(const std::string& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    const int opened = sqlite3_open(path.c_str(), &database);
    char* error = nullptr;
    const int result = opened == SQLITE_OK
        ? sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error) : opened;
    const std::string detail = error ? error : "";
    sqlite3_free(error);
    if (database) sqlite3_close(database);
    Check(result == SQLITE_OK, "Archive fault fixture could not be configured: " + detail);
}

void Seed(ConversationArchive& archive)
{
    std::string error;
    Check(archive.BeginSession("fixture", error) &&
        archive.Record("fixture", "user", "synthetic pottery discussion", error) &&
        archive.Record("fixture", "assistant", "synthetic pottery reply", error),
        "Archive fixture could not be written: " + error);
}

void TestSessionForgetRollsBackEitherDeleteFailure()
{
    for (const std::string table : {"conversation_turns", "conversation_sessions"})
    {
        ScopedTestDirectory directory;
        const auto path = (directory.root / "archive.sqlite").string();
        ConversationArchive archive(path);
        Seed(archive);
        ExecuteSql(path, "CREATE TRIGGER block_delete BEFORE DELETE ON " + table +
            " BEGIN SELECT RAISE(ABORT, 'synthetic delete failure'); END;");
        const auto removed = archive.ForgetSession("fixture");
        Check(removed == 0 && archive.TotalTurns() == 2 && archive.RecentSessions().size() == 1 &&
            archive.LoadSession("fixture").size() == 2 && archive.Search("pottery").size() == 2,
            "A failed session deletion reported success or partially removed durable history: " + table);
        ExecuteSql(path, "DROP TRIGGER block_delete;");
        Check(archive.ForgetSession("fixture") == 2 && archive.TotalTurns() == 0 &&
            archive.RecentSessions().empty(), "A session could not be forgotten after the write failure cleared.");
        std::string error;
        Check(archive.BeginSession("empty-session", error) &&
            archive.ForgetSession("empty-session") == 0 && archive.RecentSessions().empty(),
            "Forgetting an empty session counted its metadata as a removed conversation turn.");
    }
}

void TestCompleteForgetRollsBackEitherDeleteFailure()
{
    for (const std::string table : {"conversation_turns", "conversation_sessions"})
    {
        ScopedTestDirectory directory;
        const auto path = (directory.root / "archive.sqlite").string();
        ConversationArchive archive(path);
        Seed(archive);
        ExecuteSql(path, "CREATE TRIGGER block_delete BEFORE DELETE ON " + table +
            " BEGIN SELECT RAISE(ABORT, 'synthetic delete failure'); END;");
        Check(archive.Forget() == 0 && archive.TotalTurns() == 2 && archive.RecentSessions().size() == 1 &&
            archive.Search("pottery").size() == 2,
            "A failed full deletion partially removed durable conversation history: " + table);
        ExecuteSql(path, "DROP TRIGGER block_delete;");
        Check(archive.Forget() == 2 && archive.TotalTurns() == 0 && archive.RecentSessions().empty(),
            "The archive could not be forgotten after the write failure cleared.");
    }
}

void TestRetentionDoesNotCountFailedDeletion()
{
    ScopedTestDirectory directory;
    const auto path = (directory.root / "archive.sqlite").string();
    revia::memory::ArchiveLimits limits;
    limits.maxSessions = 1;
    ConversationArchive archive(path, limits);
    Seed(archive);
    ExecuteSql(path, "UPDATE conversation_sessions SET started_at='1';"
        "CREATE TRIGGER block_delete BEFORE DELETE ON conversation_sessions "
        "BEGIN SELECT RAISE(ABORT, 'synthetic delete failure'); END;");
    std::string error;
    Check(!archive.BeginSession("new-session", error) && !error.empty() &&
        archive.Counters().prunedSessions == 0 && archive.LoadSession("fixture").size() == 2,
        "Retention reported an old session pruned even though its deletion failed.");
}
}

void RunPersistenceRecoveryTests()
{
    const std::pair<const char*, void(*)()> cases[] = {
        {"numeric preferences", TestPreferenceNumbersRejectNonFiniteAndFractionalIntegers},
        {"persisted preference validation", TestPersistedPreferencesValidateBeforeApplication},
        {"concurrent preference writes", TestConcurrentPreferenceWritesRetainAcknowledgedValues},
        {"session deletion rollback", TestSessionForgetRollsBackEitherDeleteFailure},
        {"complete deletion rollback", TestCompleteForgetRollsBackEitherDeleteFailure},
        {"retention deletion failure", TestRetentionDoesNotCountFailedDeletion}
    };
    std::string failures;
    for (const auto& [name, test] : cases)
    {
        try { test(); std::cout << "Persistence PASS: " << name << '\n'; }
        catch (const std::exception& error)
        {
            const std::string failure = std::string(name) + ": " + error.what();
            std::cout << "Persistence FAIL: " << failure << '\n';
            failures += failure + '\n';
        }
    }
    Check(failures.empty(), failures);
}
