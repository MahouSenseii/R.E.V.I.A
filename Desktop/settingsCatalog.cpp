#include "settingsCatalog.h"

#include <algorithm>
#include <cctype>

namespace revia::desktop
{
namespace
{
using S = SettingsSection;
using Store = SettingStore;
using Apply = SettingApply;
using Risk = SettingRisk;
using Confirm = ConfirmWhen;
using Host = SettingHost;

// Rows are written section by section in the order the dialog shows them. Within a group the
// dialog still sorts normal rows before privacy rows before risky ones, and never puts a row ahead
// of a row it requires, so the order here only has to be sensible, not final.
//
// Descriptions are the settings inventory's one-line texts, made companion-neutral. A risky row's
// warning says what can go wrong in plain words; the confirmation dialog and the warning line under
// a switch that is on both show it.
std::vector<SettingDescriptor> BuildCatalog()
{
    std::vector<SettingDescriptor> rows;

    // ------------------------------------------------------------------ General
    rows.push_back({.key = "window/alwaysOnTop",
        .objectName = "alwaysOnTopCheck",
        .section = S::General,
        .group = "Window",
        .label = "Keep Revia on top",
        .description = "Keep the Revia window above your other windows.",
        .keywords = {"always on top", "topmost", "pin", "float"}});
    rows.push_back({.key = "window/closeToTray",
        .objectName = "sw_window_closeToTray",
        .section = S::General,
        .group = "Window",
        .label = "Keep running in the tray when closed",
        .description = "Closing the window leaves Revia running in the notification area. Quit Revia from the tray menu.",
        .defaultValue = true,
        .keywords = {"tray", "notification area", "minimize", "close", "quit", "background"}});
    rows.push_back({.key = "activity/follow",
        .objectName = "sw_activity_follow",
        .section = S::General,
        .group = "Activity window",
        .label = "Follow new activity",
        .description = "Scroll the activity log to the newest entry by itself.",
        .defaultValue = true,
        .keywords = {"auto scroll", "log", "activity"}});

    // ------------------------------------------------------------------ Appearance
    // The theme choice and chat text size are the dialog's own rows, not switches.
    rows.push_back({.key = "sidebar/expanded",
        .objectName = "sw_sidebar_expanded",
        .section = S::Appearance,
        .group = "Layout",
        .label = "Keep the sidebar open",
        .description = "Show the sidebar with your chats. Off keeps it folded to a strip of icons.",
        .defaultValue = true,
        .keywords = {"sidebar", "collapse", "rail", "navigation"}});
    rows.push_back({.key = "chat/sidebarVisible",
        .objectName = "sw_chat_sidebarVisible",
        .section = S::Appearance,
        .group = "Layout",
        .label = "Show recent chats in the sidebar",
        .description = "List your past chats in the sidebar so you can reopen them.",
        .defaultValue = true,
        .keywords = {"chat list", "history", "recents", "sidebar"}});
    rows.push_back({.key = "panels/restoreOpen",
        .objectName = "sw_panels_restoreOpen",
        .section = S::Appearance,
        .group = "Layout",
        .label = "Reopen panel windows at startup",
        .description = "Open the panel windows that were open when Revia last closed.",
        .apply = Apply::Restart,
        .keywords = {"windows", "panels", "restore", "startup"}});
    rows.push_back({.key = "chat/showWorkSummaries",
        .objectName = "sw_chat_showWorkSummaries",
        .section = S::Appearance,
        .group = "Transcript",
        .label = "Show {name}'s thinking steps",
        .description = "Show short rows for thinking, checks and memory updates between messages.",
        .defaultValue = true,
        .keywords = {"thinking", "reasoning", "work", "transcript", "chat"}});

    // ------------------------------------------------------------------ Chat
    rows.push_back({.key = "initiative.enabled",
        .objectName = "initiativeCheck",
        .section = S::Chat,
        .group = "Starting conversations",
        .label = "Let {name} start conversations",
        .description = "{name} may speak up when there is something worth saying.",
        .store = Store::Preference,
        .defaultValue = true,
        .keywords = {"initiative", "proactive", "unprompted", "talk first"}});
    rows.push_back({.key = "initiative.curiosityEnabled",
        .objectName = "curiosityCheck",
        .section = S::Chat,
        .group = "Starting conversations",
        .label = "Allow curiosity",
        .description = "{name} privately comes up with topics from your recent talks and feelings.",
        .store = Store::Preference,
        .requiresAll = {"initiative.enabled"},
        .defaultValue = true,
        .keywords = {"curious", "topics", "initiative"}});
    rows.push_back({.key = "initiative.spontaneousSpeechEnabled",
        .objectName = "spontaneousSpeechCheck",
        .section = S::Chat,
        .group = "Starting conversations",
        .label = "Say curious thoughts out loud",
        .description = "A curious thought can turn into something {name} says out loud.",
        .store = Store::Preference,
        .requiresAll = {"initiative.enabled", "initiative.curiosityEnabled"},
        .defaultValue = true,
        .keywords = {"spontaneous", "speak", "curiosity"}});
    rows.push_back({.key = "initiative.speakWhenUserAway",
        .objectName = "speakWhenAwayCheck",
        .section = S::Chat,
        .group = "Starting conversations",
        .label = "Speak while I'm away",
        .description = "{name} may still speak up when Windows says you are away from the PC.",
        .store = Store::Preference,
        .requiresAll = {"initiative.enabled", "initiative.curiosityEnabled"},
        .defaultValue = true,
        .keywords = {"away", "idle", "absent"}});
    rows.push_back({.key = "initiative.suppressWhenFullScreen",
        .objectName = "sw_initiative_suppressWhenFullScreen",
        .section = S::Chat,
        .group = "Starting conversations",
        .label = "Stay quiet during full-screen apps",
        .description = "No unprompted talking while a game, video or presentation fills the screen.",
        .store = Store::Preference,
        .requiresAll = {"initiative.enabled"},
        .defaultValue = true,
        .keywords = {"full screen", "game", "video", "presentation", "do not disturb", "quiet"}});
    rows.push_back({.key = "conversation.selfInquiryEnabled",
        .objectName = "sw_conversation_selfInquiryEnabled",
        .section = S::Chat,
        .group = "Thinking",
        .label = "Think before hard answers",
        .description = "On hard questions {name} takes a moment to think first. Slower but more careful.",
        .store = Store::Preference,
        .defaultValue = true,
        .keywords = {"reasoning", "self inquiry", "deliberation", "careful"}});
    rows.push_back({.key = "conversation.iterativeInvestigationEnabled",
        .objectName = "sw_conversation_iterativeInvestigationEnabled",
        .section = S::Chat,
        .group = "Thinking",
        .label = "Double-check open questions",
        .description = "When thinking leaves something unsettled, {name} runs a few extra checks. Slower.",
        .store = Store::Preference,
        .requiresAll = {"conversation.selfInquiryEnabled"},
        .keywords = {"investigation", "verify", "checks", "reasoning"}});
    rows.push_back({.key = "responseFilter.aiReviewEnabled",
        .objectName = "aiFilterCheck",
        .section = S::Chat,
        .group = "Reply checks",
        .label = "Extra AI review of replies",
        .description = "A second AI pass checks each reply before you see it. The basic safety filter is always on. Adds a "
                       "little delay.",
        .store = Store::Preference,
        .keywords = {"filter", "safety", "moderation", "review"}});
    rows.push_back({.key = "profile.hasTemperatureOverride",
        .objectName = "sw_profile_temperatureOverride",
        .section = S::Chat,
        .group = "This profile ({profile})",
        .label = "Use its own creativity",
        .description = "This profile uses its own temperature instead of the global one.",
        .store = Store::Profile,
        .keywords = {"temperature", "creativity", "randomness", "profile", "sampling"}});
    rows.push_back({.key = "profile.hasMaxTokensOverride",
        .objectName = "sw_profile_maxTokensOverride",
        .section = S::Chat,
        .group = "This profile ({profile})",
        .label = "Use its own reply limit",
        .description = "This profile uses its own maximum reply length. It also turns off automatic reply length.",
        .store = Store::Profile,
        .keywords = {"max tokens", "reply length", "limit", "profile"}});

    // ------------------------------------------------------------------ Voice
    // The microphone, singing and stop-to-answer switches are startup settings that something also
    // reads while Revia runs. The session commits them for that reader at once, so they are Partial
    // ("Partly applies now", design spec 5.7.5): the value in effect always matches the saved one
    // and the restart banner never counts them.
    rows.push_back({.key = "speech.enabled",
        .objectName = "speechCheck",
        .section = S::Voice,
        .group = "Spoken replies",
        .label = "Speak replies",
        .description = "{name} reads replies out loud.",
        .store = Store::Preference,
        .defaultValue = true,
        .keywords = {"voice", "speech", "tts", "text to speech", "talk", "audio"}});
    rows.push_back({.key = "speech.speakGreeting",
        .objectName = "sw_speech_speakGreeting",
        .section = S::Voice,
        .group = "Spoken replies",
        .label = "Say hello at startup",
        .description = "{name} greets you out loud on coming online.",
        .store = Store::Preference,
        .apply = Apply::Restart,
        .requiresAll = {"speech.enabled"},
        .keywords = {"greeting", "hello", "startup"}});
    rows.push_back({.key = "bargeIn.enabled",
        .objectName = "bargeInCheck",
        .section = S::Voice,
        .group = "Spoken replies",
        .label = "Let me interrupt {name}",
        .description = "Talking over {name} stops the speech.",
        .store = Store::Preference,
        .requiresAll = {"speech.enabled"},
        .defaultValue = true,
        .keywords = {"barge in", "interrupt", "stop speaking"}});
    rows.push_back({.key = "input/autoSendVoice",
        .objectName = "autoSendVoiceCheck",
        .section = S::Voice,
        .group = "Listening",
        .label = "Send what I say automatically",
        .description = "Send your spoken message when you stop talking instead of leaving it in the box to edit.",
        .disabledWhenOn = "speechRecognition.handsFree",
        .defaultValue = true,
        .keywords = {"auto send", "dictation", "voice input"}});
    rows.push_back({.key = "speechRecognition.enabled",
        .objectName = "sw_speechRecognition_enabled",
        .section = S::Voice,
        .group = "Listening",
        .label = "Use the microphone",
        .description = "Let {name} hear you through your microphone. Your speech is turned into text on this PC.",
        .store = Store::Preference,
        .apply = Apply::LiveWhenOffRestartWhenOn,
        .risk = Risk::Privacy,
        .defaultValue = true,
        .keywords = {"microphone", "mic", "speech recognition", "listening", "whisper", "dictation"}});
    rows.push_back({.key = "speechRecognition.handsFree",
        .objectName = "handsFreeCheck",
        .section = S::Voice,
        .group = "Listening",
        .label = "Hands-free listening",
        .description = "Listen all the time and send what you say when you pause. No button needed.",
        .store = Store::Preference,
        .risk = Risk::Privacy,
        .requiresAll = {"speechRecognition.enabled"},
        .keywords = {"hands free", "always listening", "open mic", "microphone"}});
    rows.push_back({.key = "speechRecognition.requireWakeWord",
        .objectName = "sw_speechRecognition_requireWakeWord",
        .section = S::Voice,
        .group = "Listening",
        .label = "Require the wake word",
        .description = "In hands-free mode {name} answers only when you say the wake word or are already talking together.",
        .warning = "With hands-free listening on, {name} will answer anything the microphone hears.",
        .store = Store::Preference,
        .risk = Risk::Privacy,
        .confirm = Confirm::TurningOff,
        .requiresAll = {"speechRecognition.handsFree"},
        .defaultValue = true,
        .keywords = {"wake word", "name", "hotword", "addressed"}});
    rows.push_back({.key = "performance.enabled",
        .objectName = "sw_performance_enabled",
        .section = S::Voice,
        .group = "Singing",
        .label = "Singing (karaoke)",
        .description = "Let {name} sing songs from your song library.",
        .store = Store::Preference,
        .apply = Apply::Partial,
        .defaultValue = true,
        .keywords = {"sing", "karaoke", "songs", "music", "performance"}});
    rows.push_back({.key = "performance.interruptSongToSpeak",
        .objectName = "sw_performance_interruptSongToSpeak",
        .section = S::Voice,
        .group = "Singing",
        .label = "Stop singing to answer",
        .description = "If you talk during a song, {name} stops to reply. Off: the music keeps playing and the reply shows as "
                       "text.",
        .store = Store::Preference,
        .apply = Apply::Partial,
        .requiresAll = {"performance.enabled"},
        .defaultValue = true,
        .keywords = {"interrupt", "song", "music"}});

    // ------------------------------------------------------------------ Vision
    rows.push_back({.key = "vision.enabled",
        .objectName = "sw_vision_enabled",
        .section = S::Vision,
        .group = "Screen",
        .label = "Let {name} see my screen",
        .description = "When you ask, {name} can take a screenshot and look at it on this PC.",
        .store = Store::SettingsFile,
        .risk = Risk::Privacy,
        .allCompanions = true,
        .keywords = {"screen", "screenshot", "vision", "look"}});
    rows.push_back({.key = "vision.continuousAwareness",
        .objectName = "sw_vision_continuousAwareness",
        .section = S::Vision,
        .group = "Screen",
        .label = "Keep track of my screen",
        .description = "{name} keeps a short private summary of what is on screen, updated when windows change. Nothing is "
                       "clicked or typed from it.",
        .store = Store::SettingsFile,
        .risk = Risk::Privacy,
        .requiresAll = {"vision.enabled", "perception.enabled"},
        .allCompanions = true,
        .keywords = {"screen awareness", "continuous", "watch", "summary"}});
    rows.push_back({.key = "perception.enabled",
        .objectName = "sw_perception_enabled",
        .section = S::Vision,
        .group = "App awareness",
        .label = "Notice which app I'm using",
        .description = "{name} notes which app and window title you are using. Kept in memory only, with exclusions.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .risk = Risk::Privacy,
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"perception", "foreground", "window title", "watching", "app"}});
    rows.push_back({.key = "perception.paused",
        .objectName = "sw_perception_paused",
        .section = S::Vision,
        .group = "App awareness",
        .label = "Pause app awareness for now",
        .description = "Stop noticing apps and the screen until you resume it or Revia restarts.",
        .store = Store::Session,
        .risk = Risk::Privacy,
        .requiresAll = {"perception.enabled"},
        .keywords = {"pause", "privacy", "watching", "perception"}});
    rows.push_back({.key = "llm.visionEnabled",
        .objectName = "sw_llm_visionEnabled",
        .section = S::Vision,
        .group = "Pictures",
        .label = "Understand pictures I attach",
        .description = "Load the main model's vision part so {name} can look at images and screenshots. Uses more graphics "
                       "memory.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .allCompanions = true,
        .keywords = {"images", "pictures", "attachments", "projector", "multimodal"}});

    // ------------------------------------------------------------------ Internet (CapabilityPanel)
    rows.push_back({.key = "capabilities.internet.enabled",
        .objectName = "internetCheck",
        .section = S::Internet,
        .group = "Internet & research",
        .label = "Internet lookups",
        .description = "Let {name} search the web, read-only and rate-limited, through a fixed set of public sources.",
        .warning = "Your questions are sent to the configured search providers.",
        .store = Store::Capability,
        .risk = Risk::Privacy,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .allCompanions = true,
        .keywords = {"web", "search", "internet", "online", "lookup"}});
    rows.push_back({.key = "capabilities.internet.automaticLookup",
        .objectName = "automaticLookupCheck",
        .section = S::Internet,
        .group = "Internet & research",
        .label = "Look things up automatically",
        .description = "Search for facts without being asked. Your questions are sent to the search provider.",
        .store = Store::Capability,
        .risk = Risk::Privacy,
        .host = Host::CapabilityPanel,
        .requiresAll = {"capabilities.internet.enabled"},
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"automatic", "facts", "search"}});
    rows.push_back({.key = "capabilities.internet.visibleBrowser",
        .objectName = "visibleBrowserCheck",
        .section = S::Internet,
        .group = "Internet & research",
        .label = "Search in a visible browser",
        .description = "Run searches in a separate Edge or Chrome window you can watch.",
        .warning = "A dedicated browser profile opens public pages; your own cookies and passwords are never used.",
        .store = Store::Capability,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .requiresAll = {"capabilities.internet.enabled"},
        .allCompanions = true,
        .keywords = {"browser", "edge", "chrome", "visible"}});
    rows.push_back({.key = "capabilities.internet.autonomousResearch",
        .objectName = "autonomousResearchCheck",
        .section = S::Internet,
        .group = "Internet & research",
        .label = "Research without being asked",
        .description = "Curiosity may start a bounded web search with no request from you.",
        .warning = "Searches can start from recent conversation or feelings, without a new message from you.",
        .store = Store::Capability,
        .risk = Risk::Privacy,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .requiresAll = {"capabilities.internet.enabled", "capabilities.internet.visibleBrowser"},
        .allCompanions = true,
        .autonomy = true,
        .keywords = {"autonomous", "research", "curiosity", "unprompted"}});
    rows.push_back({.key = "capabilities.browser.enabled",
        .objectName = "browserEnabled",
        .section = S::Internet,
        .group = "Interactive browser",
        .label = "Interactive browser",
        .description = "Let {name} use a dedicated browser on the sites you approve below.",
        .store = Store::Capability,
        .risk = Risk::Privacy,
        .host = Host::CapabilityPanel,
        .allCompanions = true,
        .keywords = {"browser", "automation", "sites", "web"}});
    rows.push_back({.key = "capabilities.browser.navigate",
        .objectName = "browserNavigate",
        .section = S::Internet,
        .group = "Interactive browser",
        .label = "Open pages",
        .description = "Open pages on approved sites.",
        .store = Store::Capability,
        .risk = Risk::Privacy,
        .host = Host::CapabilityPanel,
        .requiresAll = {"capabilities.browser.enabled"},
        .allCompanions = true,
        .keywords = {"navigate", "pages", "browser"}});
    rows.push_back({.key = "capabilities.browser.interact",
        .objectName = "browserInteract",
        .section = S::Internet,
        .group = "Interactive browser",
        .label = "Click and fill in forms",
        .description = "Click buttons and fill in or submit forms on approved sites.",
        .warning = "Forms can be submitted for you: whatever an approved site lets you do, {name} can do there.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .requiresAll = {"capabilities.browser.enabled"},
        .allCompanions = true,
        .keywords = {"click", "forms", "submit", "interact", "browser"}});
    rows.push_back({.key = "capabilities.browser.allowTaskInteraction",
        .objectName = "browserTaskInteraction",
        .section = S::Internet,
        .group = "Interactive browser",
        .label = "Let tasks use the browser",
        .description = "Tasks you hand off may use the browser too, with the same sites and permissions.",
        .warning = "Delegated tasks use the browser without asking you about each step.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .requiresAll = {"capabilities.browser.enabled"},
        .allCompanions = true,
        .keywords = {"tasks", "delegated", "agents", "browser"}});
    rows.push_back({.key = "capabilities.browser.allowLoopback",
        .objectName = "browserLoopback",
        .section = S::Internet,
        .group = "Interactive browser",
        .label = "Allow local test sites",
        .description = "Allow approved sites running on this PC (localhost), including plain HTTP.",
        .warning = "Services running on this PC become reachable, including ones with no sign-in of their own.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .requiresAll = {"capabilities.browser.enabled"},
        .allCompanions = true,
        .keywords = {"localhost", "loopback", "http", "local"}});

    // ------------------------------------------------------------------ Memory
    rows.push_back({.key = "conversation.archiveEnabled",
        .objectName = "sw_conversation_archiveEnabled",
        .section = S::Memory,
        .group = "Chat history",
        .label = "Keep chat history",
        .description = "Save everything said so you can reopen and search past chats. The chat list needs this.",
        .store = Store::Preference,
        .apply = Apply::LiveWhenOffRestartWhenOn,
        .risk = Risk::Privacy,
        .defaultValue = true,
        .keywords = {"archive", "history", "transcripts", "save chats"}});
    rows.push_back({.key = "profile.memoryEnabled",
        .objectName = "sw_profile_memoryEnabled",
        .section = S::Memory,
        .group = "Long-term memory",
        .label = "Remember things (this profile)",
        .description = "Save what you tell this profile to long-term memory.",
        .store = Store::Profile,
        .risk = Risk::Privacy,
        .defaultValue = true,
        .keywords = {"memory", "remember", "profile", "long-term"}});
    rows.push_back({.key = "initiative.autonomousLearningEnabled",
        .objectName = "sw_initiative_autonomousLearningEnabled",
        .section = S::Memory,
        .group = "Long-term memory",
        .label = "Remember what {name} researches",
        .description = "Keep short, sourced notes from research done without being asked. Only matters when that research "
                       "is allowed.",
        .store = Store::Preference,
        .softRequires = "capabilities.internet.autonomousResearch",
        .defaultValue = true,
        .keywords = {"learning", "notes", "research", "memory"}});
    rows.push_back({.key = "embedding.enabled",
        .objectName = "sw_embedding_enabled",
        .section = S::Memory,
        .group = "Search",
        .label = "Search memories by meaning",
        .description = "Find memories and documents by meaning, not only by exact words. Uses a small extra model.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"embedding", "semantic", "vector", "search", "rag"}});

    // ------------------------------------------------------------------ Models
    rows.push_back({.key = "intelligence.enabled",
        .objectName = "sw_intelligence_enabled",
        .section = S::Models,
        .group = "Helper models",
        .label = "Use helper models",
        .description = "Use the extra Fast and Expert models alongside the main one.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"intelligence", "tiers", "models", "fast", "expert"}});
    rows.push_back({.key = "intelligence.fast.enabled",
        .objectName = "sw_intelligence_fast_enabled",
        .section = S::Models,
        .group = "Helper models",
        .label = "Fast helper model",
        .description = "A small model for quick turns. It also keeps chat working if the main model is down.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .requiresAll = {"intelligence.enabled"},
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"fast", "small model", "fallback"}});
    rows.push_back({.key = "intelligence.fast.warmAtStartup",
        .objectName = "sw_intelligence_fast_warmAtStartup",
        .section = S::Models,
        .group = "Helper models",
        .label = "Warm up the Fast model at startup",
        .description = "Send the Fast model one warm-up request once it is running, so its first reply comes sooner.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .requiresAll = {"intelligence.fast.enabled"},
        .allCompanions = true,
        .keywords = {"warm up", "preload", "latency", "fast"}});
    rows.push_back({.key = "intelligence.expert.enabled",
        .objectName = "sw_intelligence_expert_enabled",
        .section = S::Models,
        .group = "Helper models",
        .label = "Expert model for hard questions",
        .description = "A larger model for difficult turns. Needs a second graphics card.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .requiresAll = {"intelligence.enabled"},
        .allCompanions = true,
        .keywords = {"expert", "large model", "gpu", "hard questions"}});
    rows.push_back({.key = "intelligence.expert.onDemand",
        .objectName = "sw_intelligence_expert_onDemand",
        .section = S::Models,
        .group = "Helper models",
        .label = "Load Expert only when needed",
        .description = "Saves graphics memory. The first hard question takes longer.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .requiresAll = {"intelligence.expert.enabled"},
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"on demand", "lazy", "vram", "expert"}});
    rows.push_back({.key = "intelligence.expert.warmAtStartup",
        .objectName = "sw_intelligence_expert_warmAtStartup",
        .section = S::Models,
        .group = "Helper models",
        .label = "Warm up the Expert model at startup",
        .description = "Send the Expert model one warm-up request once it is running, so its first hard answer comes sooner.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .requiresAll = {"intelligence.expert.enabled"},
        .disabledWhenOn = "intelligence.expert.onDemand",
        .allCompanions = true,
        .keywords = {"warm up", "preload", "latency", "expert"}});
    rows.push_back({.key = "llm.cloud.enabled",
        .objectName = "sw_llm_cloud_enabled",
        .section = S::Models,
        .group = "Cloud",
        .label = "Allow the cloud model",
        .description = "Let some turns go to an online AI service you set up (its key comes from an environment variable). Off "
                       "keeps everything on this PC.",
        .warning = "Messages sent to the cloud, and the context that goes with them, leave this PC.",
        .store = Store::SettingsFile,
        .risk = Risk::Privacy,
        .confirm = Confirm::TurningOn,
        .allCompanions = true,
        .keywords = {"cloud", "online", "api", "openai", "remote model"}});
    rows.push_back({.key = "cloud.session",
        .objectName = "sw_cloud_session",
        .section = S::Models,
        .group = "Cloud",
        .label = "Use the cloud this session",
        .description = "Pause or resume the cloud model until Revia restarts. It cannot turn the cloud on while Allow the cloud "
                       "model is off.",
        .store = Store::Session,
        .risk = Risk::Privacy,
        .requiresAll = {"llm.cloud.enabled"},
        .defaultValue = true,
        .keywords = {"cloud", "session", "pause"}});
    rows.push_back({.key = "image.enabled",
        .objectName = "sw_image_enabled",
        .section = S::Models,
        .group = "Pictures",
        .label = "Picture generation",
        .description = "Let {name} draw pictures on this PC (/imagine). Needs the image add-on installed.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"image", "draw", "imagine", "stable diffusion", "art"}});
    rows.push_back({.key = "image.keepLoaded",
        .objectName = "sw_image_keepLoaded",
        .section = S::Models,
        .group = "Pictures",
        .label = "Keep the picture model loaded",
        .description = "Faster pictures, but graphics memory stays in use between them.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .requiresAll = {"image.enabled"},
        .allCompanions = true,
        .keywords = {"keep loaded", "vram", "image"}});
    // Stored as image.offline; the switch asks the positive question.
    rows.push_back({.key = "image.offline",
        .objectName = "sw_image_allowInternet",
        .section = S::Models,
        .group = "Pictures",
        .label = "Let picture generation use the internet",
        .description = "Allow the picture add-on to download models from the internet. Off keeps it offline.",
        .warning = "The picture add-on may download model files from the internet.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .risk = Risk::Privacy,
        .confirm = Confirm::TurningOn,
        .requiresAll = {"image.enabled"},
        .allCompanions = true,
        .inverted = true,
        .defaultValue = true,
        .keywords = {"offline", "download", "internet", "image"}});

    // ------------------------------------------------------------------ Permissions (CapabilityPanel)
    rows.push_back({.key = "capabilities.camera.enabled",
        .objectName = "cameraCheck",
        .section = S::Permissions,
        .group = "Camera",
        .label = "Camera snapshots",
        .description = "Take a single still photo from your camera when you ask. No video, no recording.",
        .warning = "Frames are kept in RuntimeData/Camera on this PC and are never uploaded.",
        .store = Store::Capability,
        .risk = Risk::Privacy,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .allCompanions = true,
        .keywords = {"camera", "webcam", "photo", "snapshot"}});
    rows.push_back({.key = "capabilities.camera.autonomousCapture",
        .objectName = "autonomousCameraCheck",
        .section = S::Permissions,
        .group = "Camera",
        .label = "Let {name} decide when to look",
        .description = "{name} may take a camera photo without being asked.",
        .warning = "Photos can be taken without a request from you, within the same rate limit and audit trail.",
        .store = Store::Capability,
        .risk = Risk::Privacy,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .requiresAll = {"capabilities.camera.enabled"},
        .allCompanions = true,
        .autonomy = true,
        .keywords = {"autonomous", "camera", "look"}});
    rows.push_back({.key = "capabilities.desktopControl.pointer",
        .objectName = "pointerCheck",
        .section = S::Permissions,
        .group = "Pointer & keyboard",
        .label = "Mouse control",
        .description = "Move and click the pointer, only inside apps you approve.",
        .warning = "Clicks inside approved apps are indistinguishable from yours.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .allCompanions = true,
        .keywords = {"mouse", "pointer", "click", "desktop control", "automation"}});
    rows.push_back({.key = "capabilities.desktopControl.keyboard",
        .objectName = "keyboardCheck",
        .section = S::Permissions,
        .group = "Pointer & keyboard",
        .label = "Keyboard control",
        .description = "Type and press keys, only inside apps you approve.",
        .warning = "Typing inside approved apps is indistinguishable from yours.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .allCompanions = true,
        .keywords = {"keyboard", "typing", "keys", "desktop control"}});
    rows.push_back({.key = "capabilities.desktopControl.applicationLaunch",
        .objectName = "launchCheck",
        .section = S::Permissions,
        .group = "Pointer & keyboard",
        .label = "Open approved apps",
        .description = "Start apps from your approved list.",
        .warning = "Approved apps can be started without you opening them.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .allCompanions = true,
        .keywords = {"launch", "start", "apps", "programs"}});
    rows.push_back({.key = "capabilities.desktopControl.visualTargeting",
        .objectName = "visualTargetCheck",
        .section = S::Permissions,
        .group = "Pointer & keyboard",
        .label = "Click what {name} sees",
        .description = "Click things {name} can see in games and custom screens that have no normal buttons. Needs Mouse "
                       "control.",
        .warning = "Clicks are aimed from a screenshot instead of a control Windows can check.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .requiresAll = {"capabilities.desktopControl.pointer"},
        .allCompanions = true,
        .keywords = {"visual", "games", "screen", "click"}});
    rows.push_back({.key = "capabilities.desktopControl.rawCoordinates",
        .objectName = "rawCoordinateCheck",
        .section = S::Permissions,
        .group = "Pointer & keyboard",
        .label = "Click chosen screen positions",
        .description = "Click screen positions {name} picks instead of checked buttons. Needs Mouse control.",
        .warning = "A click can land on whatever is at that point, with no control behind it to check.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .requiresAll = {"capabilities.desktopControl.pointer"},
        .allCompanions = true,
        .keywords = {"coordinates", "position", "click", "raw"}});
    rows.push_back({.key = "capabilities.desktopControl.wholeDesktop",
        .objectName = "wholeDesktopCheck",
        .section = S::Permissions,
        .group = "Gated controls",
        .label = "Allow the whole desktop",
        .description = "Control any window, not only approved apps. Needs Mouse control and Click chosen screen positions.",
        .warning = "Input is no longer confined to approved apps: the pointer goes anywhere and keys go to whatever has focus.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .requiresAll = {"capabilities.desktopControl.pointer", "capabilities.desktopControl.rawCoordinates"},
        .allCompanions = true,
        .keywords = {"whole desktop", "any window", "scope", "confinement"}});
    rows.push_back({.key = "capabilities.desktopControl.allowCommandSurfaces",
        .objectName = "commandSurfaceCheck",
        .section = S::Permissions,
        .group = "Gated controls",
        .label = "Type into terminals",
        .description = "Allow typing into command prompts and script windows, and the shortcuts that open them.",
        .warning = "Text typed into a terminal runs as a command on this PC.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .allCompanions = true,
        .keywords = {"terminal", "command prompt", "powershell", "shell", "script"}});
    rows.push_back({.key = "capabilities.desktopControl.autonomous",
        .objectName = "autonomousDesktopCheck",
        .section = S::Permissions,
        .group = "Gated controls",
        .label = "Allow unprompted desktop work",
        .description = "Operate the desktop without being asked. Still refused while you are actively working.",
        .warning = "Pointer, keyboard and app actions can start without a request from you.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .allCompanions = true,
        .autonomy = true,
        .keywords = {"autonomous", "unprompted", "desktop", "on its own"}});
    rows.push_back({.key = "capabilities.process.enabled",
        .objectName = "processEnabled",
        .section = S::Permissions,
        .group = "Commands",
        .label = "Run approved programs",
        .description = "Run programs from your approved list, with time and output limits.",
        .warning = "Approved programs run with your Windows account's access.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .allCompanions = true,
        .keywords = {"programs", "process", "commands", "executables", "run"}});
    rows.push_back({.key = "capabilities.process.allowCommandInterpreters",
        .objectName = "processCommandInterpreters",
        .section = S::Permissions,
        .group = "Commands",
        .label = "Allow shells and scripts",
        .description = "Allow command shells and script hosts that are on your approved list.",
        .warning = "A shell runs whatever command it is given, so model-written text can reach your system.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .allCompanions = true,
        .keywords = {"shell", "powershell", "cmd", "scripts", "interpreters"}});
    rows.push_back({.key = "capabilities.process.allowTaskExecution",
        .objectName = "processTaskExecution",
        .section = S::Permissions,
        .group = "Commands",
        .label = "Let tasks run programs",
        .description = "Tasks you hand off may run your approved programs, within the same limits.",
        .warning = "Delegated tasks run approved programs without asking you about each step.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .host = Host::CapabilityPanel,
        .allCompanions = true,
        .keywords = {"tasks", "delegated", "agents", "programs"}});
    rows.push_back({.key = "virtualMachine.enabled",
        .objectName = "sw_vm_enabled",
        .section = S::Permissions,
        .group = "Ubuntu workspace",
        .label = "Start the Ubuntu workspace",
        .description = "Start the private Ubuntu virtual machine (VMware) with Revia. It grants no permissions by itself.",
        .store = Store::VirtualMachine,
        .apply = Apply::Restart,
        .keywords = {"virtual machine", "vm", "ubuntu", "vmware", "linux", "sandbox"}});
    rows.push_back({.key = "capabilities.guest.enabled",
        .objectName = "sw_guest_enabled",
        .section = S::Permissions,
        .group = "Ubuntu workspace",
        .label = "Allow actions in the Ubuntu workspace",
        .description = "Let {name} work inside the private Ubuntu virtual machine.",
        .store = Store::Capability,
        .allCompanions = true,
        .keywords = {"guest", "virtual machine", "vm", "workspace"}});
    rows.push_back({.key = "capabilities.guest.write",
        .objectName = "sw_guest_write",
        .section = S::Permissions,
        .group = "Ubuntu workspace",
        .label = "Write files in the workspace",
        .description = "Write text files inside the Ubuntu workspace.",
        .store = Store::Capability,
        .requiresAll = {"capabilities.guest.enabled"},
        .allCompanions = true,
        .keywords = {"write", "files", "guest", "vm"}});
    rows.push_back({.key = "capabilities.guest.read",
        .objectName = "sw_guest_read",
        .section = S::Permissions,
        .group = "Ubuntu workspace",
        .label = "Read files in the workspace",
        .description = "Read files and folders inside the Ubuntu workspace. Sharing a project into it needs read and write.",
        .store = Store::Capability,
        .risk = Risk::Privacy,
        .requiresAll = {"capabilities.guest.enabled"},
        .allCompanions = true,
        .keywords = {"read", "files", "guest", "vm"}});
    rows.push_back({.key = "capabilities.guest.execute",
        .objectName = "sw_guest_execute",
        .section = S::Permissions,
        .group = "Ubuntu workspace",
        .label = "Run programs in the workspace",
        .description = "Run approved programs inside the Ubuntu workspace.",
        .warning = "Programs run inside the virtual machine and can change anything shared into it.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .requiresAll = {"capabilities.guest.enabled"},
        .allCompanions = true,
        .keywords = {"execute", "programs", "guest", "vm"}});
    rows.push_back({.key = "capabilities.guest.allowTaskExecution",
        .objectName = "sw_guest_allowTaskExecution",
        .section = S::Permissions,
        .group = "Ubuntu workspace",
        .label = "Let tasks use the workspace",
        .description = "Tasks you hand off may run programs in the Ubuntu workspace.",
        .warning = "Delegated tasks run workspace programs without asking you about each step.",
        .store = Store::Capability,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .requiresAll = {"capabilities.guest.enabled", "capabilities.guest.execute"},
        .allCompanions = true,
        .keywords = {"tasks", "delegated", "guest", "vm"}});

    // ------------------------------------------------------------------ Integrations
    rows.push_back({.key = "presence.enabled",
        .objectName = "sw_presence_enabled",
        .section = S::Integrations,
        .group = "Avatar and streaming",
        .label = "Avatar and streaming features",
        .description = "Master switch for avatar output and the local Discord, stream and game connections.",
        .store = Store::Preference,
        .defaultValue = true,
        .keywords = {"presence", "avatar", "streaming", "vtuber"}});
    rows.push_back({.key = "presence.avatarBridgeEnabled",
        .objectName = "avatarBridgeCheck",
        .section = S::Integrations,
        .group = "Avatar and streaming",
        .label = "Publish avatar animation",
        .description = "Share {name}'s expressions and animation events with your avatar app.",
        .store = Store::Preference,
        .requiresAll = {"presence.enabled"},
        .defaultValue = true,
        .keywords = {"avatar", "animation", "expressions", "vtube studio"}});
    rows.push_back({.key = "presence.externalAdaptersEnabled",
        .objectName = "externalAdaptersCheck",
        .section = S::Integrations,
        .group = "Avatar and streaming",
        .label = "Discord, stream and game connections",
        .description = "Accept chat messages from local Discord, stream and game connectors. Replies there are public.",
        .store = Store::Preference,
        .risk = Risk::Privacy,
        .requiresAll = {"presence.enabled"},
        .keywords = {"discord", "twitch", "stream chat", "game", "adapters"}});
    rows.push_back({.key = "presence.requireAddressedStreamMessages",
        .objectName = "sw_presence_requireAddressedStreamMessages",
        .section = S::Integrations,
        .group = "Avatar and streaming",
        .label = "Only reply to viewers who address {name}",
        .description = "Stream viewers get a reply only when they address {name} by name.",
        .store = Store::Preference,
        .requiresAll = {"presence.externalAdaptersEnabled"},
        .defaultValue = true,
        .keywords = {"viewers", "stream", "addressed", "name"}});
    rows.push_back({.key = "presence.speakStreamReplies",
        .objectName = "sw_presence_speakStreamReplies",
        .section = S::Integrations,
        .group = "Avatar and streaming",
        .label = "Speak stream replies",
        .description = "Read answers to stream chat out loud.",
        .store = Store::Preference,
        .requiresAll = {"presence.externalAdaptersEnabled"},
        .keywords = {"stream", "speak", "voice", "chat"}});
    rows.push_back({.key = "localApi.enabled",
        .objectName = "sw_localApi_enabled",
        .section = S::Integrations,
        .group = "Local API",
        .label = "Local API for other apps",
        .description = "Let other programs on this PC talk to Revia with the install token. They are treated as a shared "
                       "guest by default.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .risk = Risk::Privacy,
        .allCompanions = true,
        .keywords = {"api", "http", "token", "integration", "local server"}});
    rows.push_back({.key = "localApi.trustedPrivate",
        .objectName = "sw_localApi_trustedPrivate",
        .section = S::Integrations,
        .group = "Local API",
        .label = "Trust the local API as me",
        .description = "Treat every API request as if you typed it, with your memories, tools and approvals.",
        .warning = "Any program on this PC with the install token speaks as you.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .requiresAll = {"localApi.enabled"},
        .allCompanions = true,
        .keywords = {"trusted", "api", "owner", "private"}});
    // One block per configured server; "*" is replaced by the server's id.
    rows.push_back({.key = "mcp.servers[*].enabled",
        .objectName = "sw_mcp_*_enabled",
        .section = S::Integrations,
        .group = "Tool server: *",
        .label = "Connect *",
        .description = "Start this tool server and offer its allowed tools to {name}.",
        .warning = "The server's allowed tools become available to {name}, and the server runs with your account's access.",
        .store = Store::SettingsFile,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .allCompanions = true,
        .keywords = {"mcp", "tools", "tool server", "plugins"}});
    rows.push_back({.key = "mcp.servers[*].requireConfirmation",
        .objectName = "sw_mcp_*_requireConfirmation",
        .section = S::Integrations,
        .group = "Tool server: *",
        .label = "Ask before each tool call",
        .description = "{name} asks you before every call to this server. Off lets its allowed tools run on their own.",
        .warning = "This server's allowed tools will run without asking you first.",
        .store = Store::SettingsFile,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOff,
        .requiresAll = {"mcp.servers[*].enabled"},
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"mcp", "confirmation", "approve", "ask"}});

    // ------------------------------------------------------------------ Advanced
    rows.push_back({.key = "improvement.enabled",
        .objectName = "sw_improvement_enabled",
        .section = S::Advanced,
        .group = "Self-improvement reviews",
        .label = "Self-improvement reviews",
        .description = "While you are away, {name} reviews Revia's code and writes suggested fixes. Your installed code is "
                       "never changed.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"improvement", "self review", "code", "suggestions"}});
    rows.push_back({.key = "improvement.explore",
        .objectName = "sw_improvement_explore",
        .section = S::Advanced,
        .group = "Self-improvement reviews",
        .label = "Look for improvements without being asked",
        .description = "Review parts of Revia every couple of hours, not only after a problem was recorded.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .requiresAll = {"improvement.enabled"},
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"explore", "improvement", "review"}});
    rows.push_back({.key = "improvement.verify",
        .objectName = "sw_improvement_verify",
        .section = S::Advanced,
        .group = "Self-improvement reviews",
        .label = "Build and test suggested improvements",
        .description = "Compile and test each suggestion in a separate copy. Uses a lot of CPU.",
        .warning = "Model-written code is compiled and run on this PC.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .risk = Risk::Dangerous,
        .confirm = Confirm::TurningOn,
        .requiresAll = {"improvement.enabled"},
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"verify", "build", "test", "compile"}});
    rows.push_back({.key = "llm.autoTune",
        .objectName = "sw_llm_autoTune",
        .section = S::Advanced,
        .group = "Performance and hardware",
        .label = "Automatic model tuning",
        .description = "Fit the main model's memory use to your hardware automatically. Off uses the manual numbers in "
                       "settings.json.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"tuning", "context", "layers", "gpu", "vram"}});
    rows.push_back({.key = "llm.autoMaxTokens",
        .objectName = "sw_llm_autoMaxTokens",
        .section = S::Advanced,
        .group = "Performance and hardware",
        .label = "Automatic reply length",
        .description = "Choose a reply length for each turn, up to the maximum. Ignored while a profile sets its own reply "
                       "limit.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"max tokens", "reply length", "automatic"}});
    rows.push_back({.key = "llm.autoStartServer",
        .objectName = "sw_llm_autoStartServer",
        .section = S::Advanced,
        .group = "Performance and hardware",
        .label = "Start the chat model server automatically",
        .description = "Revia starts and stops its own chat model server. Off connects to a server you run yourself, and chat "
                       "stops working if none is running.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"llama", "server", "llama.cpp", "external server"}});
    rows.push_back({.key = "embedding.autoStartServer",
        .objectName = "sw_embedding_autoStartServer",
        .section = S::Advanced,
        .group = "Performance and hardware",
        .label = "Start the memory search server automatically",
        .description = "Revia starts its own memory search server. Off connects to one you run yourself.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .requiresAll = {"embedding.enabled"},
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"embedding", "server", "memory search"}});
    rows.push_back({.key = "speechRecognition.useGpu",
        .objectName = "sw_speechRecognition_useGpu",
        .section = S::Advanced,
        .group = "Performance and hardware",
        .label = "Use the GPU for speech recognition",
        .description = "Run speech recognition on a graphics card. Automatic hardware planning decides this while it is on.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .allCompanions = true,
        .keywords = {"gpu", "cuda", "whisper", "speech recognition"}});
    rows.push_back({.key = "resources.autoPlan",
        .objectName = "sw_resources_autoPlan",
        .section = S::Advanced,
        .group = "Performance and hardware",
        .label = "Automatic hardware planning",
        .description = "At startup, decide which graphics card or the CPU runs each model. Off uses the assignments in "
                       "settings.json.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .allCompanions = true,
        .defaultValue = true,
        .keywords = {"gpu", "planner", "devices", "assignments", "hardware"}});
    rows.push_back({.key = "resources.allowChatModelSplit",
        .objectName = "sw_resources_allowChatModelSplit",
        .section = S::Advanced,
        .group = "Performance and hardware",
        .label = "Split the chat model across GPUs",
        .description = "Let the main model use two graphics cards when it does not fit on one.",
        .store = Store::SettingsFile,
        .apply = Apply::Restart,
        .allCompanions = true,
        .keywords = {"multi gpu", "split", "tensor split", "graphics cards"}});
    return rows;
}

bool Replace(std::string& text, const std::string& from, const std::string& to)
{
    const auto at = text.find(from);
    if (at == std::string::npos)
        return false;
    text.replace(at, from.size(), to);
    return true;
}
}

const std::vector<SettingDescriptor>& SettingCatalog()
{
    static const std::vector<SettingDescriptor> catalog = BuildCatalog();
    return catalog;
}

const SettingDescriptor* FindSetting(const std::string& keyOrObjectName)
{
    const auto& catalog = SettingCatalog();
    const auto found = std::find_if(catalog.begin(), catalog.end(),
        [&](const SettingDescriptor& row) { return row.key == keyOrObjectName || row.objectName == keyOrObjectName; });
    return found == catalog.end() ? nullptr : &*found;
}

const std::vector<SettingsSection>& SettingsSections()
{
    static const std::vector<SettingsSection> sections = {S::General, S::Appearance, S::Chat, S::Voice, S::Vision, S::Internet,
        S::Memory, S::Models, S::Permissions, S::Integrations, S::Advanced};
    return sections;
}

QString SectionName(const SettingsSection section)
{
    switch (section)
    {
        case S::General: return QStringLiteral("General");
        case S::Appearance: return QStringLiteral("Appearance");
        case S::Chat: return QStringLiteral("Chat");
        case S::Voice: return QStringLiteral("Voice");
        case S::Vision: return QStringLiteral("Vision");
        case S::Internet: return QStringLiteral("Internet");
        case S::Memory: return QStringLiteral("Memory");
        case S::Models: return QStringLiteral("Models");
        case S::Permissions: return QStringLiteral("Permissions");
        case S::Integrations: return QStringLiteral("Integrations");
        case S::Advanced: return QStringLiteral("Advanced");
    }
    return {};
}

const char* SectionIcon(const SettingsSection section)
{
    switch (section)
    {
        case S::General: return "settings";
        case S::Appearance: return "sun";
        case S::Chat: return "message";
        case S::Voice: return "mic";
        case S::Vision: return "eye";
        case S::Internet: return "globe";
        case S::Memory: return "database";
        case S::Models: return "cpu";
        case S::Permissions: return "shield";
        case S::Integrations: return "git-branch";
        case S::Advanced: return "code";
    }
    return "settings";
}

const char* SectionId(const SettingsSection section)
{
    switch (section)
    {
        case S::General: return "general";
        case S::Appearance: return "appearance";
        case S::Chat: return "chat";
        case S::Voice: return "voice";
        case S::Vision: return "vision";
        case S::Internet: return "internet";
        case S::Memory: return "memory";
        case S::Models: return "models";
        case S::Permissions: return "permissions";
        case S::Integrations: return "integrations";
        case S::Advanced: return "advanced";
    }
    return "general";
}

bool IsServerTemplate(const SettingDescriptor& descriptor)
{
    return descriptor.key.find("[*]") != std::string::npos;
}

std::string ObjectNamePart(const std::string& text)
{
    std::string part = text;
    for (char& character : part)
    {
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_')
            character = '_';
    }
    return part;
}

SettingDescriptor ServerRow(const SettingDescriptor& serverTemplate, const std::string& serverId)
{
    SettingDescriptor row = serverTemplate;
    Replace(row.key, "[*]", "[" + serverId + "]");
    Replace(row.objectName, "*", ObjectNamePart(serverId));
    const QString id = QString::fromStdString(serverId);
    row.group.replace(QStringLiteral("*"), id);
    row.label.replace(QStringLiteral("*"), id);
    for (std::string& required : row.requiresAll)
        Replace(required, "[*]", "[" + serverId + "]");
    row.keywords.push_back(id);
    return row;
}

QString WithCompanionName(QString text, const QString& companionName)
{
    return text.replace(QStringLiteral("{name}"), companionName.isEmpty() ? QStringLiteral("Revia") : companionName);
}

QString DescriptionWhenOff(const SettingDescriptor& descriptor)
{
    if (descriptor.key == "window/closeToTray")
        return QStringLiteral("Closing the window quits Revia.");
    return {};
}
}
