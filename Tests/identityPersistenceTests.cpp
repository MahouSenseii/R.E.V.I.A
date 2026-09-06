#include "reviaSessionTestAccess.h"

#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace
{
using namespace std::chrono_literals;
using namespace revia::runtime;
using namespace revia::identity;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using Access = ReviaSessionTestAccess;

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path& root)
        : previous(std::filesystem::current_path()) { std::filesystem::current_path(root); }
    ~WorkingDirectory() { std::filesystem::current_path(previous); }
private:
    std::filesystem::path previous;
};

void Write(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
    output.close();
    Check(!output.fail(), "Could not write identity fixture.");
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::filesystem::path IdentityPath(const std::filesystem::path& root)
{
    return root / "RuntimeData/Identity/identity.json";
}

void Configure(const std::filesystem::path& root, const bool placeholderTurn = false)
{
    // None is deliberately unavailable and performs no HTTP request or model load.
    // Absolute writable service paths and a fixture Config directory keep bootstrap,
    // logs, preferences, SQLite and identity away from the executable's runtime data.
    nlohmann::json settings = {
        {"activeProfile", "fixture"},
        {"llm", {{"backend", placeholderTurn ? "Placeholder" : "None"}, {"autoStartServer", false},
            {"visionEnabled", false}, {"modelPath", (root / "absent.gguf").string()},
            {"mediaPath", (root / "RuntimeData/Vision").string()}}},
        {"intelligence", {{"enabled", false}}},
        {"embedding", {{"enabled", false}, {"autoStartServer", false}}},
        {"speech", {{"enabled", false}, {"backend", "WindowsSapi"},
            {"speakGreeting", false}, {"voiceDataPath", (root / "RuntimeData/Voices").string()}}},
        {"speechRecognition", {{"enabled", false}}},
        {"presence", {{"enabled", false}, {"externalAdaptersEnabled", false}}},
        {"vision", {{"enabled", false}}}, {"perception", {{"enabled", false}}},
        {"initiative", {{"enabled", false}}}, {"bargeIn", {{"enabled", false}}},
        {"conversation", {{"archiveEnabled", false}}}, {"image", {{"enabled", false}}}
    };
    Write(root / "Config/settings.json", settings.dump(2));
    Write(root / "Config/Profiles/fixture.json", R"({"id":"fixture",
        "displayName":"Fixture", "systemPrompt":"Persistence fixture.",
        "shouldSpeak":false,"memoryEnabled":false})");
}

template<class Predicate>
bool Until(Predicate predicate, std::chrono::milliseconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void VerifySnapshot(const std::filesystem::path& path, const std::string& name)
{
    IdentitySnapshot snapshot;
    std::string error;
    Check(IdentityStore(path).Load(snapshot, error), error);
    Check(snapshot.relationships.contains(LocalUserEntityId()) &&
        snapshot.relationships.at(LocalUserEntityId()).displayName == name,
        "The current relationship was not persisted: " + name);
    Check(!snapshot.preferences.empty() && snapshot.preferences.front().subject == "fixture astronomy",
        "Earned preferences were lost.");
    Check(std::abs(snapshot.development.delta[Trait::Patience] - 0.07F) < 0.0001F &&
        std::abs(snapshot.mood.valence - 0.31F) < 0.0001F,
        "Earned development or mood was lost.");
}

void TestAutosaveAndLateShutdown(const bool finalOnly = false)
{
    ScopedTestDirectory temporary;
    Configure(temporary.root);
    WorkingDirectory cwd(temporary.root);
    {
        ReviaSession session;
        Access::IdentitySaveEvery(session, finalOnly ? 1h : 40ms);
        Check(session.Start(), "Isolated production session did not start.");
        if (!finalOnly)
        {
            Access::RememberIdentity(session, "First periodic save");
            Check(Until([&] { return Read(IdentityPath(temporary.root)).find("First periodic save") != std::string::npos; }),
                "Session.Start did not connect periodic identity persistence.");
        }
        Access::RememberIdentity(session, "Second periodic save");
        if (!finalOnly)
        {
            Check(Until([&] { return Read(IdentityPath(temporary.root)).find("Second periodic save") != std::string::npos; }),
                "Autosave did not replace an existing identity.");
        }
        session.Stop();
        VerifySnapshot(IdentityPath(temporary.root), "Second periodic save");
    }
    {
        ReviaSession session;
        Access::IdentitySaveEvery(session, 1h);
        Check(session.Start(), "Restart did not load the saved identity.");
        Check(session.Relationships().front().displayName == "Second periodic save" &&
            !session.CurrentPreferences().empty() &&
            std::abs(session.CurrentDevelopment().delta[Trait::Patience] - 0.07F) < 0.0001F,
            "Production startup discarded earned identity.");
        std::promise<void> entered;
        std::jthread late([&]
        {
            auto foreground = Access::HoldForeground(session);
            const auto token = Access::OperationToken(session);
            entered.set_value();
            if (Until([&] { return token.stop_requested(); }))
                Access::RememberIdentity(session, "Late shutdown evidence");
        });
        entered.get_future().wait();
        session.Stop();
        late.join();
        VerifySnapshot(IdentityPath(temporary.root), "Late shutdown evidence");
    }
}

void TestFailedLoadAndReplacement()
{
    ScopedTestDirectory temporary;
    Configure(temporary.root);
    WorkingDirectory cwd(temporary.root);
    const auto path = IdentityPath(temporary.root);
    const std::string corrupt = "{ unreadable original identity";
    Write(path, corrupt);
    {
        ReviaSession session;
        Access::IdentitySaveEvery(session, 25ms);
        Check(session.Start(), "Corrupt identity prevented isolated startup.");
        Access::RememberIdentity(session, "Must not overwrite");
        std::this_thread::sleep_for(150ms);
        Check(Read(path) == corrupt, "Autosave overwrote a failed identity load.");
        session.Stop();
        Check(Read(path) == corrupt, "Final save overwrote a failed identity load.");
    }
    // A directory is not a missing first-run file.
    const auto unreadable = temporary.root / "not-a-file";
    std::filesystem::create_directory(unreadable);
    IdentitySnapshot snapshot;
    std::string error;
    Check(!IdentityStore(unreadable).Load(snapshot, error), "Unreadable identity was treated as first run.");

    IdentityStore store(path);
    Check(store.Save(snapshot, error), error); // Explicit fixture repair; production never does this.
    const std::string before = Read(path);
    std::filesystem::create_directory(path.string() + ".tmp");
    Check(!store.Save(snapshot, error) && Read(path) == before,
        "A failed pending write damaged the previous identity.");
    std::filesystem::remove(path.string() + ".tmp");
#ifdef _WIN32
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "Could not hold the replacement fault fixture.");
    snapshot.mood.valence = 0.5F;
    const bool saved = store.Save(snapshot, error);
    CloseHandle(locked);
    Check(!saved && Read(path) == before, "Replacement failure damaged the previous identity.");
    Check(store.Save(snapshot, error), "Replacement did not recover after the lock was released: " + error);
#endif
}

#ifdef _WIN32
void TestOwnedCrashAndRestart()
{
    ScopedTestDirectory temporary;
    wchar_t executable[32768]{};
    Check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Could not locate crash fixture executable.");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --identity-crash-child \"" +
        temporary.root.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, temporary.root.c_str(), &startup, &process), "Could not create the owned crash fixture.");
    CloseHandle(process.hThread);
    const bool saved = Until([&] { return std::filesystem::exists(temporary.root / "saved.ready"); }, 20s);
    // Only the handle returned by our CreateProcess call is terminated. Stop and
    // destructors cannot rescue this process; the periodic production save must.
    const bool terminated = TerminateProcess(process.hProcess, 73) != 0;
    const DWORD waited = WaitForSingleObject(process.hProcess, 5000);
    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    Check(terminated && waited == WAIT_OBJECT_0 && exitCode == 73,
        "Owned crash fixture was not terminated with the expected exit code.");
    Check(saved, "Owned process did not autosave before its crash deadline.");
    VerifySnapshot(IdentityPath(temporary.root), "Survives forced exit");
    IdentitySnapshot recovered;
    std::string error;
    Check(IdentityStore(IdentityPath(temporary.root)).Load(recovered, error), error);
    Check(recovered.relationships.at(LocalUserEntityId()).interactionCount > 0,
        "The completed production turn did not survive forced exit.");
    WorkingDirectory cwd(temporary.root);
    ReviaSession restarted;
    Check(restarted.Start(), "Production restart after forced exit failed.");
    Check(restarted.Relationships().front().displayName == "Survives forced exit" &&
        !restarted.CurrentPreferences().empty(), "Production restart lost the last periodic snapshot.");
    restarted.Stop();
}
#endif
}

void RunIdentityCrashChild(const std::filesystem::path& root)
{
    Configure(root, true);
    WorkingDirectory cwd(root);
    ReviaSession session;
    Access::IdentitySaveEvery(session, 40ms);
    Check(session.Start(), "Crash child startup failed.");
    const auto reply = session.Submit("Explain why an eclipsing binary star changes brightness.");
    Check(reply.succeeded && reply.fromAssistant,
        "The placeholder-backed production conversation did not complete.");
    Access::RememberIdentity(session, "Survives forced exit");
    Check(Until([&] { return Read(IdentityPath(root)).find("Survives forced exit") != std::string::npos; }),
        "Crash child periodic save failed.");
    Write(root / "saved.ready", "ready");
    std::this_thread::sleep_for(60s);
    throw std::runtime_error("Parent did not terminate its crash fixture.");
}

void RunIdentityPersistenceTests()
{
    TestAutosaveAndLateShutdown();
    TestFailedLoadAndReplacement();
#ifdef _WIN32
    TestOwnedCrashAndRestart();
#endif
    std::cout << "Identity persistence owner tests passed.\n";
}

void RunIdentityFinalSaveTests()
{
    TestAutosaveAndLateShutdown(true);
    std::cout << "Identity final save owner tests passed.\n";
}
