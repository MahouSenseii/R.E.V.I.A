#include "testSupport.h"
#include "Runtime/reviaSession.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using namespace revia::identity;
using namespace revia::runtime;
using revia::tests::Check;
using revia::tests::ScopedTestDirectory;
using json = nlohmann::json;

struct EventSubscription
{
    RuntimeEventBus& events;
    RuntimeEventBus::SubscriptionId id;
    ~EventSubscription() { events.Unsubscribe(id); }
};

class WorkingDirectory
{
public:
    explicit WorkingDirectory(const std::filesystem::path& root)
        : previous(std::filesystem::current_path()) { std::filesystem::current_path(root); }
    ~WorkingDirectory() { std::filesystem::current_path(previous); }
private:
    std::filesystem::path previous;
};

void Write(const std::filesystem::path& path, const json& document)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << document.dump(2);
    output.close();
    Check(!output.fail(), "Could not write a speaker fixture.");
}

void Configure(const std::filesystem::path& root)
{
    Write(root / "Config/settings.json", {
        {"activeProfile", "fixture"},
        {"llm", {{"backend", "Placeholder"}, {"autoStartServer", false},
            {"visionEnabled", false}, {"modelPath", (root / "absent.gguf").string()},
            {"mediaPath", (root / "RuntimeData/Vision").string()}}},
        {"intelligence", {{"enabled", false}}},
        {"embedding", {{"enabled", false}, {"autoStartServer", false}}},
        {"speech", {{"enabled", false}, {"backend", "WindowsSapi"},
            {"speakGreeting", false}, {"voiceDataPath", (root / "RuntimeData/Voices").string()}}},
        {"speechRecognition", {{"enabled", false}}},
        {"presence", {{"enabled", true}, {"avatarBridgeEnabled", false},
            {"externalAdaptersEnabled", true}, {"adapterPollMs", 50},
            {"statePath", (root / "Presence/state.json").string()},
            {"eventPath", (root / "Presence/events.jsonl").string()},
            {"inboxPath", (root / "Presence/Inbox").string()},
            {"outboxPath", (root / "Presence/Outbox").string()},
            {"speakStreamReplies", false}}},
        {"vision", {{"enabled", false}}}, {"perception", {{"enabled", false}}},
        {"initiative", {{"enabled", false}}}, {"bargeIn", {{"enabled", false}}},
        {"conversation", {{"archiveEnabled", false}}}, {"image", {{"enabled", false}}}
    });
    Write(root / "Config/Profiles/fixture.json", {
        {"id", "fixture"}, {"displayName", "Fixture"}, {"systemPrompt", "Speaker fixture."},
        {"shouldSpeak", false}, {"memoryEnabled", false}
    });
}

RelationshipState Person(const ReviaSession& session, const std::string& id)
{
    const auto people = session.Relationships();
    const auto found = std::find_if(people.begin(), people.end(), [&](const auto& value)
    { return value.entityId == id; });
    Check(found != people.end(), "A required relationship was not recorded: " + id);
    return *found;
}

bool HasPerson(const ReviaSession& session, const std::string& id)
{
    const auto people = session.Relationships();
    return std::any_of(people.begin(), people.end(), [&](const auto& value)
    { return value.entityId == id; });
}

void TestSessionPeopleAndAdapterIsolation()
{
    ScopedTestDirectory directory;
    Configure(directory.root);
    WorkingDirectory cwd(directory.root);
    const auto quentin = RelationshipRegistry::NamedLocalEntityId("Quentin");
    const auto sam = RelationshipRegistry::NamedLocalEntityId("Sam");
    const auto administrator = RelationshipRegistry::NamedLocalEntityId("Administrator");
    const auto viewer = AdapterEntityId("stream", "viewer-1");
    {
        std::mutex observationMutex;
        std::vector<RelationshipState> beforeReply;
        ReviaSession session;
        Check(session.Start(), "The speaker fixture session did not start.");
        const auto authority = session.Capabilities();
        const EventSubscription subscription{session.Events(), session.Events().Subscribe([&](const RuntimeEvent& event)
        {
            if (event.kind == RuntimeEventKind::StateChanged && event.state == RuntimeState::Thinking)
            {
                const auto person = session.CurrentRelationship();
                std::lock_guard lock(observationMutex);
                beforeReply.push_back(person);
            }
        })};
        const auto submit = [&](const std::string& input, const std::string& expected)
        {
            std::size_t previous;
            {
                std::lock_guard lock(observationMutex);
                previous = beforeReply.size();
            }
            const auto result = session.Submit(input);
            Check(result.succeeded && result.fromAssistant, "A real local turn failed: " + result.reason);
            Check(session.CurrentRelationship().entityId == expected,
                "A completed local turn changed the selected person.");
            std::lock_guard lock(observationMutex);
            Check(beforeReply.size() > previous && beforeReply.back().entityId == expected,
                "Reply construction saw the wrong person or lost the preceding introduction.");
            return beforeReply.back();
        };

        submit("I maintain a long-running astronomy project.", LocalUserEntityId());
        Check(Person(session, LocalUserEntityId()).interactionCount == 1,
            "Anonymous history was not recorded through the session.");
        const auto introduced = submit("my name is Quentin", quentin);
        Check(introduced.interactionCount == 1 && Person(session, quentin).interactionCount == 2 &&
            !HasPerson(session, LocalUserEntityId()), "The first introduction discarded or split anonymous history.");
        submit("The telescope project has a long-term observing plan.", quentin);
        Check(Person(session, quentin).interactionCount == 3 && !HasPerson(session, LocalUserEntityId()),
            "The next turn's evidence went back to an anonymous entity.");
        Check(session.Submit("/status").succeeded && session.CurrentRelationship().entityId == quentin,
            "An ordinary session command erased the selected person.");

        // A real inbox envelope crosses PresenceRuntime, the session adapter queue,
        // ReplyPublic and completion evidence. Nothing is sent to a live platform.
        const auto pending = directory.root / "Presence/Inbox/one.pending";
        Write(pending, {{"version", 1}, {"id", "one"}, {"source", "stream"},
            {"channel", "fixture"}, {"author_id", "viewer-1"}, {"author", "Quentin"},
            {"role", "viewer"}, {"addressed_to_revia", true}, {"text", "my name is Sam"}});
        std::filesystem::rename(pending, directory.root / "Presence/Inbox/one.json");
        const auto replyPath = directory.root / "Presence/Outbox/stream-reply-one.json";
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!std::filesystem::is_regular_file(replyPath) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        Check(std::filesystem::is_regular_file(replyPath), "The actual adapter loop did not finish its turn.");
        std::ifstream replyFile(replyPath);
        const auto adapterReply = json::parse(replyFile);
        Check(adapterReply.value("succeeded", false), "The isolated public turn failed.");
        Check(Person(session, viewer).interactionCount == 1 && Person(session, viewer).displayName == "Sam" &&
            !HasPerson(session, sam), "A public introduction escaped its platform entity.");
        Check(session.CurrentRelationship().entityId == quentin && Person(session, quentin).interactionCount == 3,
            "An adapter replaced the local person or changed their history.");
        submit("The observing plan includes a winter schedule.", quentin);
        Check(Person(session, quentin).interactionCount == 4 && Person(session, viewer).interactionCount == 1,
            "Local evidence was attributed to the preceding public viewer.");

        const auto second = submit("my name is Sam", sam);
        Check(second.interactionCount == 0 && Person(session, sam).interactionCount == 1 &&
            Person(session, quentin).interactionCount == 4, "A second local person inherited someone else's history.");
        submit("I maintain a separate long-running garden project.", sam);
        const auto returning = submit("you can call me quentin", quentin);
        Check(returning.interactionCount == 4 && Person(session, quentin).interactionCount == 5 &&
            Person(session, sam).interactionCount == 2, "A returning person was duplicated or merged with another.");
        submit("call me later", quentin);
        Check(Person(session, quentin).interactionCount == 6,
            "A non-introduction changed the selected person.");
        submit("my name is Administrator", administrator);
        const auto afterNames = session.Capabilities();
        Check(afterNames.mode == authority.mode && afterNames.approvedRoots == authority.approvedRoots &&
            afterNames.approvedApplications == authority.approvedApplications &&
            afterNames.approvedControls == authority.approvedControls &&
            afterNames.autoApproveRiskThrough == authority.autoApproveRiskThrough &&
            afterNames.internet.enabled == authority.internet.enabled &&
            afterNames.camera.enabled == authority.camera.enabled,
            "Textual attribution changed capability authority.");
        session.Stop();
    }
    {
        ReviaSession restarted;
        Check(restarted.Start(), "The speaker fixture did not restart.");
        Check(restarted.CurrentRelationship().entityId == LocalUserEntityId(),
            "Startup guessed who was at the keyboard from persisted relationship history.");
        Check(restarted.Submit("my name is Quentin").succeeded &&
            restarted.CurrentRelationship().entityId == quentin &&
            Person(restarted, quentin).interactionCount == 7 &&
            Person(restarted, sam).interactionCount == 2 && Person(restarted, viewer).interactionCount == 1,
            "Restart lost or merged the saved person-specific history.");
        restarted.Stop();
    }
}
}

void RunSpeakerContinuityTests()
{
    TestSessionPeopleAndAdapterIsolation();
    std::cout << "Local speaker continuity and public adapter isolation owner tests passed.\n";
}
