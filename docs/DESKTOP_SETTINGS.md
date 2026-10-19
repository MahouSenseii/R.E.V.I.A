# Desktop settings

Revia has one Settings window. It opens from the gear at the bottom of the sidebar, from the
command palette and with Ctrl+, and it stays open beside the chat until you close it. Its
sections sit in a list on the left: General, Appearance, Chat, Voice, Vision, Internet, Memory,
Models, Permissions, Integrations and Advanced.

Every user-facing true/false setting is a labelled switch with a one-line explanation. A switch
either applies at once or carries the badge "Applies after restart" or "Partly applies now". Risky
switches carry a "Risky" badge and a warning, and ask before they change. This page lists every
switch, where its value is kept, and the booleans that deliberately have no switch.

The list is generated from `Desktop/settingsCatalog.cpp`, the single source the dialog, the
CapabilityPanel, the command palette and this page share. `Revia.SettingsDialog`
(`ReviaDesktopStopTests --suite=settings`) checks that each of the 84 user-facing booleans found in
the settings inventory appears exactly once, alongside the nine switches the redesign added, and
that every boolean preference and every owner-settings key has a row.

## How a switch behaves

- **Badges.** "Applies after restart" (it changes to "Restart to apply" once you have changed it),
  "Partly applies now" for a startup setting that part of Revia already follows while it runs
  (the rest takes effect at the next start, and the restart banner does not count it), "All
  companions" for values shared by every companion, "Privacy", "Risky" and "Autonomy". Badges
  always pair a word with their colour.
- **Confirmation.** Turning on a risky switch, or turning off one whose off state is the risky one
  (marked "asks when turned off" below), opens a question titled "Turn on ...?" or "Turn off ...?"
  with the warning. Cancel is the default and changes nothing.
- **Saving.** Shell settings are written to QSettings at once. Everything else is written in the
  background: the switch shows "Saving..." and is disabled until the write finishes. If the write
  is refused, the switch goes back and the reason is shown under it. After a successful write the
  row shows the message Revia returned. The temperature and reply limit beside the two profile
  override switches are saved when you leave them with a changed value; a refused value goes back
  to the saved one.
- **Restart banner.** "Restart Revia to apply N changes" counts the switches whose saved value
  differs from the one in effect. "Restart now" restarts Revia; "Later" hides the banner until the
  count changes.
- **Dependencies.** A switch whose requirement is off is disabled, and its tooltip says which
  switch to turn on first. Requirements are judged on saved values, so a restart-only parent still
  counts once you have changed it. "Works best with ..." is only a hint.
- **Inverted rows.** "Let picture generation use the internet" shows the opposite of the stored
  `image.offline` value, so the switch can ask the positive question.
- **Reset.** Right-click a Preference or owner-overlay row for "Reset to default", which removes
  your saved choice so the shipped value applies again. A switch that asks before it turns on is
  reset only while it is on (one that asks when turned off, only while it is off): the shipped
  value may be the risky one, and a reset never makes that change without the question.
- **Search.** The search box filters rows by label, description and keywords across every
  section, shows each section's hit count, and says so when nothing matches. Esc clears the search
  first and closes Settings second. Ctrl+F focuses it and Down moves into the section list.
  Enter does nothing by itself in Settings; Space turns a focused switch or presses a focused
  button.
- **Scrolling.** The mouse wheel changes a number box or a drop-down list only after you have
  clicked or tabbed into it; over one you have not, it scrolls the page. This includes the
  "Requested tasks" mode in Permissions, where one notch would otherwise switch to Free mode.
- **Names.** "{name}" in this page is the active companion's display name. "Revia" in a label
  names the application.

## Where values are kept

| Store | Location | Shared by |
| --- | --- | --- |
| QSettings | The desktop's QSettings: `window/alwaysOnTop`, `window/closeToTray`, `sidebar/expanded`, `chat/sidebarVisible`, `panels/restoreOpen`, `chat/showWorkSummaries`, `activity/follow`, `input/autoSendVoice`, `appearance/theme`, `appearance/chatTextSize` | This PC |
| Preference | `<companion>/RuntimeData/Preferences/preferences.json` | One companion |
| Owner overlay | `InstallRoot/RuntimeData/Settings/owner-settings.json`, laid over `Config/settings.json` when it is read. `Config/settings.json` itself is never written, because builds and updates replace it. | All companions |
| Capabilities | `InstallRoot/RuntimeData/Capabilities/capabilities.json` | All companions |
| Active profile | The profile's file, saved through the same path as the Profiles window. Refused while that window holds unsaved edits. | One profile |
| Ubuntu workspace file | `<companion>/RuntimeData/VirtualMachine/settings.json` | One companion |
| Session only | Not saved; back to its default at the next start | This run |

## Every switch

Tool-server switches repeat for each server in `Config/settings.json`; *server* stands for the
server's `id`, and the objectName uses that id with every character outside `[A-Za-z0-9_]`
replaced by `_`. *profile* is the active profile's name. Switches marked "CapabilityPanel switch"
live in the permission cards hosted in the Internet and Permissions sections; the rest are
Settings rows whose row frame is named after the switch plus `Row`.

### General

| Group | Switch | What it does | Key | Stored in | Applies | Risk | Needs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Window | Keep Revia on top | Keep the Revia window above your other windows. | `window/alwaysOnTop` (`alwaysOnTopCheck`), default off | QSettings | Live |  |  |
| Window | Keep running in the tray when closed | Closing the window leaves Revia running in the notification area. Quit Revia from the tray menu. | `window/closeToTray` (`sw_window_closeToTray`), default on | QSettings | Live |  |  |
| Activity window | Follow new activity | Scroll the activity log to the newest entry by itself. | `activity/follow` (`sw_activity_follow`), default on | QSettings | Live |  |  |

### Appearance

| Group | Switch | What it does | Key | Stored in | Applies | Risk | Needs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Layout | Keep the sidebar open | Show the sidebar with your chats. Off keeps it folded to a strip of icons. | `sidebar/expanded` (`sw_sidebar_expanded`), default on | QSettings | Live |  |  |
| Layout | Show recent chats in the sidebar | List your past chats in the sidebar so you can reopen them. | `chat/sidebarVisible` (`sw_chat_sidebarVisible`), default on | QSettings | Live |  |  |
| Layout | Reopen panel windows at startup | Open the panel windows that were open when Revia last closed. | `panels/restoreOpen` (`sw_panels_restoreOpen`), default off | QSettings | After restart |  |  |
| Transcript | Show *{name}*'s thinking steps | Show short rows for thinking, checks and memory updates between messages. | `chat/showWorkSummaries` (`sw_chat_showWorkSummaries`), default on | QSettings | Live |  |  |

### Chat

| Group | Switch | What it does | Key | Stored in | Applies | Risk | Needs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Starting conversations | Let *{name}* start conversations | *{name}* may speak up when there is something worth saying. | `initiative.enabled` (`initiativeCheck`), default on | Preference | Live |  |  |
| Starting conversations | Allow curiosity | *{name}* privately comes up with topics from your recent talks and feelings. | `initiative.curiosityEnabled` (`curiosityCheck`), default on | Preference | Live |  | Let *{name}* start conversations |
| Starting conversations | Say curious thoughts out loud | A curious thought can turn into something *{name}* says out loud. | `initiative.spontaneousSpeechEnabled` (`spontaneousSpeechCheck`), default on | Preference | Live |  | Let *{name}* start conversations, Allow curiosity |
| Starting conversations | Speak while I'm away | *{name}* may still speak up when Windows says you are away from the PC. | `initiative.speakWhenUserAway` (`speakWhenAwayCheck`), default on | Preference | Live |  | Let *{name}* start conversations, Allow curiosity |
| Starting conversations | Stay quiet during full-screen apps | No unprompted talking while a game, video or presentation fills the screen. | `initiative.suppressWhenFullScreen` (`sw_initiative_suppressWhenFullScreen`), default on | Preference | Live |  | Let *{name}* start conversations |
| Thinking | Think before hard answers | On hard questions *{name}* takes a moment to think first. Slower but more careful. | `conversation.selfInquiryEnabled` (`sw_conversation_selfInquiryEnabled`), default on | Preference | Live |  |  |
| Thinking | Double-check open questions | When thinking leaves something unsettled, *{name}* runs a few extra checks. Slower. | `conversation.iterativeInvestigationEnabled` (`sw_conversation_iterativeInvestigationEnabled`), default off | Preference | Live |  | Think before hard answers |
| Reply checks | Extra AI review of replies | A second AI pass checks each reply before you see it. The basic safety filter is always on. Adds a little delay. | `responseFilter.aiReviewEnabled` (`aiFilterCheck`), default off | Preference | Live |  |  |
| This profile (*profile*) | Use its own creativity | This profile uses its own temperature instead of the global one. | `profile.hasTemperatureOverride` (`sw_profile_temperatureOverride`), default off | Active profile | Live |  |  |
| This profile (*profile*) | Use its own reply limit | This profile uses its own maximum reply length. It also turns off automatic reply length. | `profile.hasMaxTokensOverride` (`sw_profile_maxTokensOverride`), default off | Active profile | Live |  |  |

### Voice

| Group | Switch | What it does | Key | Stored in | Applies | Risk | Needs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Spoken replies | Speak replies | *{name}* reads replies out loud. | `speech.enabled` (`speechCheck`), default on | Preference | Live |  |  |
| Spoken replies | Say hello at startup | *{name}* greets you out loud on coming online. | `speech.speakGreeting` (`sw_speech_speakGreeting`), default off | Preference | After restart |  | Speak replies |
| Spoken replies | Let me interrupt *{name}* | Talking over *{name}* stops the speech. | `bargeIn.enabled` (`bargeInCheck`), default on | Preference | Live |  | Speak replies |
| Listening | Send what I say automatically | Send your spoken message when you stop talking instead of leaving it in the box to edit. | `input/autoSendVoice` (`autoSendVoiceCheck`), default on | QSettings | Live |  | off while Hands-free listening is on |
| Listening | Use the microphone | Let *{name}* hear you through your microphone. Your speech is turned into text on this PC. | `speechRecognition.enabled` (`sw_speechRecognition_enabled`), default on | Preference | Partly now, fully after restart | Privacy |  |
| Listening | Hands-free listening | Listen all the time and send what you say when you pause. No button needed. | `speechRecognition.handsFree` (`handsFreeCheck`), default off | Preference | Live | Privacy | Use the microphone |
| Listening | Require the wake word | In hands-free mode *{name}* answers only when you say the wake word or are already talking together. **Warning:** With hands-free listening on, *{name}* will answer anything the microphone hears. | `speechRecognition.requireWakeWord` (`sw_speechRecognition_requireWakeWord`), default on | Preference | Live | Privacy, asks when turned off | Hands-free listening |
| Singing | Singing (karaoke) | Let *{name}* sing songs from your song library. | `performance.enabled` (`sw_performance_enabled`), default on | Preference | Partly now, fully after restart |  |  |
| Singing | Stop singing to answer | If you talk during a song, *{name}* stops to reply. Off: the music keeps playing and the reply shows as text. | `performance.interruptSongToSpeak` (`sw_performance_interruptSongToSpeak`), default on | Preference | Partly now, fully after restart |  | Singing (karaoke) |

### Vision

| Group | Switch | What it does | Key | Stored in | Applies | Risk | Needs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Screen | Let *{name}* see my screen | When you ask, *{name}* can take a screenshot and look at it on this PC. | `vision.enabled` (`sw_vision_enabled`), default off | Owner overlay (all companions) | Live | Privacy |  |
| Screen | Keep track of my screen | *{name}* keeps a short private summary of what is on screen, updated when windows change. Nothing is clicked or typed from it. | `vision.continuousAwareness` (`sw_vision_continuousAwareness`), default off | Owner overlay (all companions) | Live | Privacy | Let *{name}* see my screen, Notice which app I'm using |
| App awareness | Notice which app I'm using | *{name}* notes which app and window title you are using. Kept in memory only, with exclusions. | `perception.enabled` (`sw_perception_enabled`), default on | Owner overlay (all companions) | After restart | Privacy |  |
| App awareness | Pause app awareness for now | Stop noticing apps and the screen until you resume it or Revia restarts. | `perception.paused` (`sw_perception_paused`), default off | Session only | Live | Privacy | Notice which app I'm using |
| Pictures | Understand pictures I attach | Load the main model's vision part so *{name}* can look at images and screenshots. Uses more graphics memory. | `llm.visionEnabled` (`sw_llm_visionEnabled`), default off | Owner overlay (all companions) | After restart |  |  |

### Internet

| Group | Switch | What it does | Key | Stored in | Applies | Risk | Needs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Internet & research | Internet lookups | Let *{name}* search the web, read-only and rate-limited, through a fixed set of public sources. **Warning:** Your questions are sent to the configured search providers. | `capabilities.internet.enabled` (`internetCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Privacy, asks when turned on |  |
| Internet & research | Look things up automatically | Search for facts without being asked. Your questions are sent to the search provider. | `capabilities.internet.automaticLookup` (`automaticLookupCheck`), default on | Capabilities (all companions); CapabilityPanel switch | Live | Privacy | Internet lookups |
| Internet & research | Search in a visible browser | Run searches in a separate Edge or Chrome window you can watch. **Warning:** A dedicated browser profile opens public pages; your own cookies and passwords are never used. | `capabilities.internet.visibleBrowser` (`visibleBrowserCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | asks when turned on | Internet lookups |
| Internet & research | Research without being asked | Curiosity may start a bounded web search with no request from you. **Warning:** Searches can start from recent conversation or feelings, without a new message from you. | `capabilities.internet.autonomousResearch` (`autonomousResearchCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Privacy, asks when turned on, Autonomy | Internet lookups, Search in a visible browser |
| Interactive browser | Interactive browser | Let *{name}* use a dedicated browser on the sites you approve below. | `capabilities.browser.enabled` (`browserEnabled`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Privacy |  |
| Interactive browser | Open pages | Open pages on approved sites. | `capabilities.browser.navigate` (`browserNavigate`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Privacy | Interactive browser |
| Interactive browser | Click and fill in forms | Click buttons and fill in or submit forms on approved sites. **Warning:** Forms can be submitted for you: whatever an approved site lets you do, *{name}* can do there. | `capabilities.browser.interact` (`browserInteract`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on | Interactive browser |
| Interactive browser | Let tasks use the browser | Tasks you hand off may use the browser too, with the same sites and permissions. **Warning:** Delegated tasks use the browser without asking you about each step. | `capabilities.browser.allowTaskInteraction` (`browserTaskInteraction`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on | Interactive browser |
| Interactive browser | Allow local test sites | Allow approved sites running on this PC (localhost), including plain HTTP. **Warning:** Services running on this PC become reachable, including ones with no sign-in of their own. | `capabilities.browser.allowLoopback` (`browserLoopback`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on | Interactive browser |

### Memory

| Group | Switch | What it does | Key | Stored in | Applies | Risk | Needs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Chat history | Keep chat history | Save everything said so you can reopen and search past chats. The chat list needs this. | `conversation.archiveEnabled` (`sw_conversation_archiveEnabled`), default on | Preference | Off live, on after restart | Privacy |  |
| Long-term memory | Remember things (this profile) | Save what you tell this profile to long-term memory. | `profile.memoryEnabled` (`sw_profile_memoryEnabled`), default on | Active profile | Live | Privacy |  |
| Long-term memory | Remember what *{name}* researches | Keep short, sourced notes from research done without being asked. Only matters when that research is allowed. | `initiative.autonomousLearningEnabled` (`sw_initiative_autonomousLearningEnabled`), default on | Preference | Live |  | works best with Research without being asked |
| Search | Search memories by meaning | Find memories and documents by meaning, not only by exact words. Uses a small extra model. | `embedding.enabled` (`sw_embedding_enabled`), default on | Owner overlay (all companions) | After restart |  |  |

### Models

| Group | Switch | What it does | Key | Stored in | Applies | Risk | Needs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Helper models | Use helper models | Use the extra Fast and Expert models alongside the main one. | `intelligence.enabled` (`sw_intelligence_enabled`), default on | Owner overlay (all companions) | After restart |  |  |
| Helper models | Fast helper model | A small model for quick turns. It also keeps chat working if the main model is down. | `intelligence.fast.enabled` (`sw_intelligence_fast_enabled`), default on | Owner overlay (all companions) | After restart |  | Use helper models |
| Helper models | Warm up the Fast model at startup | Send the Fast model one warm-up request once it is running, so its first reply comes sooner. | `intelligence.fast.warmAtStartup` (`sw_intelligence_fast_warmAtStartup`), default off | Owner overlay (all companions) | After restart |  | Fast helper model |
| Helper models | Expert model for hard questions | A larger model for difficult turns. Needs a second graphics card. | `intelligence.expert.enabled` (`sw_intelligence_expert_enabled`), default off | Owner overlay (all companions) | After restart |  | Use helper models |
| Helper models | Load Expert only when needed | Saves graphics memory. The first hard question takes longer. | `intelligence.expert.onDemand` (`sw_intelligence_expert_onDemand`), default on | Owner overlay (all companions) | After restart |  | Expert model for hard questions |
| Helper models | Warm up the Expert model at startup | Send the Expert model one warm-up request once it is running, so its first hard answer comes sooner. | `intelligence.expert.warmAtStartup` (`sw_intelligence_expert_warmAtStartup`), default off | Owner overlay (all companions) | After restart |  | Expert model for hard questions, off while Load Expert only when needed is on |
| Cloud | Allow the cloud model | Let some turns go to an online AI service you set up (its key comes from an environment variable). Off keeps everything on this PC. **Warning:** Messages sent to the cloud, and the context that goes with them, leave this PC. | `llm.cloud.enabled` (`sw_llm_cloud_enabled`), default off | Owner overlay (all companions) | Live | Privacy, asks when turned on |  |
| Cloud | Use the cloud this session | Pause or resume the cloud model until Revia restarts. It cannot turn the cloud on while Allow the cloud model is off. | `cloud.session` (`sw_cloud_session`), default on | Session only | Live | Privacy | Allow the cloud model |
| Pictures | Picture generation | Let *{name}* draw pictures on this PC (/imagine). Needs the image add-on installed. | `image.enabled` (`sw_image_enabled`), default on | Owner overlay (all companions) | After restart |  |  |
| Pictures | Keep the picture model loaded | Faster pictures, but graphics memory stays in use between them. | `image.keepLoaded` (`sw_image_keepLoaded`), default off | Owner overlay (all companions) | After restart |  | Picture generation |
| Pictures | Let picture generation use the internet | Allow the picture add-on to download models from the internet. Off keeps it offline. **Warning:** The picture add-on may download model files from the internet. | `image.offline (shown inverted)` (`sw_image_allowInternet`), default on | Owner overlay (all companions) | After restart | Privacy, asks when turned on | Picture generation |

### Permissions

| Group | Switch | What it does | Key | Stored in | Applies | Risk | Needs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Camera | Camera snapshots | Take a single still photo from your camera when you ask. No video, no recording. **Warning:** Frames are kept in RuntimeData/Camera on this PC and are never uploaded. | `capabilities.camera.enabled` (`cameraCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Privacy, asks when turned on |  |
| Camera | Let *{name}* decide when to look | *{name}* may take a camera photo without being asked. **Warning:** Photos can be taken without a request from you, within the same rate limit and audit trail. | `capabilities.camera.autonomousCapture` (`autonomousCameraCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Privacy, asks when turned on, Autonomy | Camera snapshots |
| Pointer & keyboard | Mouse control | Move and click the pointer, only inside apps you approve. **Warning:** Clicks inside approved apps are indistinguishable from yours. | `capabilities.desktopControl.pointer` (`pointerCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on |  |
| Pointer & keyboard | Keyboard control | Type and press keys, only inside apps you approve. **Warning:** Typing inside approved apps is indistinguishable from yours. | `capabilities.desktopControl.keyboard` (`keyboardCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on |  |
| Pointer & keyboard | Open approved apps | Start apps from your approved list. **Warning:** Approved apps can be started without you opening them. | `capabilities.desktopControl.applicationLaunch` (`launchCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on |  |
| Pointer & keyboard | Click what *{name}* sees | Click things *{name}* can see in games and custom screens that have no normal buttons. Needs Mouse control. **Warning:** Clicks are aimed from a screenshot instead of a control Windows can check. | `capabilities.desktopControl.visualTargeting` (`visualTargetCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on | Mouse control |
| Pointer & keyboard | Click chosen screen positions | Click screen positions *{name}* picks instead of checked buttons. Needs Mouse control. **Warning:** A click can land on whatever is at that point, with no control behind it to check. | `capabilities.desktopControl.rawCoordinates` (`rawCoordinateCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on | Mouse control |
| Gated controls | Allow the whole desktop | Control any window, not only approved apps. Needs Mouse control and Click chosen screen positions. **Warning:** Input is no longer confined to approved apps: the pointer goes anywhere and keys go to whatever has focus. | `capabilities.desktopControl.wholeDesktop` (`wholeDesktopCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on | Mouse control, Click chosen screen positions |
| Gated controls | Type into terminals | Allow typing into command prompts and script windows, and the shortcuts that open them. **Warning:** Text typed into a terminal runs as a command on this PC. | `capabilities.desktopControl.allowCommandSurfaces` (`commandSurfaceCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on |  |
| Gated controls | Allow unprompted desktop work | Operate the desktop without being asked. Still refused while you are actively working. **Warning:** Pointer, keyboard and app actions can start without a request from you. | `capabilities.desktopControl.autonomous` (`autonomousDesktopCheck`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on, Autonomy |  |
| Commands | Run approved programs | Run programs from your approved list, with time and output limits. **Warning:** Approved programs run with your Windows account's access. | `capabilities.process.enabled` (`processEnabled`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on |  |
| Commands | Allow shells and scripts | Allow command shells and script hosts that are on your approved list. **Warning:** A shell runs whatever command it is given, so model-written text can reach your system. | `capabilities.process.allowCommandInterpreters` (`processCommandInterpreters`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on |  |
| Commands | Let tasks run programs | Tasks you hand off may run your approved programs, within the same limits. **Warning:** Delegated tasks run approved programs without asking you about each step. | `capabilities.process.allowTaskExecution` (`processTaskExecution`), default off | Capabilities (all companions); CapabilityPanel switch | Live | Risky, asks when turned on |  |
| Ubuntu workspace | Start the Ubuntu workspace | Start the private Ubuntu virtual machine (VMware) with Revia. It grants no permissions by itself. | `virtualMachine.enabled` (`sw_vm_enabled`), default off | Ubuntu workspace file | After restart |  |  |
| Ubuntu workspace | Allow actions in the Ubuntu workspace | Let *{name}* work inside the private Ubuntu virtual machine. | `capabilities.guest.enabled` (`sw_guest_enabled`), default off | Capabilities (all companions) | Live |  |  |
| Ubuntu workspace | Write files in the workspace | Write text files inside the Ubuntu workspace. | `capabilities.guest.write` (`sw_guest_write`), default off | Capabilities (all companions) | Live |  | Allow actions in the Ubuntu workspace |
| Ubuntu workspace | Read files in the workspace | Read files and folders inside the Ubuntu workspace. Sharing a project into it needs read and write. | `capabilities.guest.read` (`sw_guest_read`), default off | Capabilities (all companions) | Live | Privacy | Allow actions in the Ubuntu workspace |
| Ubuntu workspace | Run programs in the workspace | Run approved programs inside the Ubuntu workspace. **Warning:** Programs run inside the virtual machine and can change anything shared into it. | `capabilities.guest.execute` (`sw_guest_execute`), default off | Capabilities (all companions) | Live | Risky, asks when turned on | Allow actions in the Ubuntu workspace |
| Ubuntu workspace | Let tasks use the workspace | Tasks you hand off may run programs in the Ubuntu workspace. **Warning:** Delegated tasks run workspace programs without asking you about each step. | `capabilities.guest.allowTaskExecution` (`sw_guest_allowTaskExecution`), default off | Capabilities (all companions) | Live | Risky, asks when turned on | Allow actions in the Ubuntu workspace, Run programs in the workspace |

### Integrations

| Group | Switch | What it does | Key | Stored in | Applies | Risk | Needs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Avatar and streaming | Avatar and streaming features | Master switch for avatar output and the local Discord, stream and game connections. | `presence.enabled` (`sw_presence_enabled`), default on | Preference | Live |  |  |
| Avatar and streaming | Publish avatar animation | Share *{name}*'s expressions and animation events with your avatar app. | `presence.avatarBridgeEnabled` (`avatarBridgeCheck`), default on | Preference | Live |  | Avatar and streaming features |
| Avatar and streaming | Discord, stream and game connections | Accept chat messages from local Discord, stream and game connectors. Replies there are public. | `presence.externalAdaptersEnabled` (`externalAdaptersCheck`), default off | Preference | Live | Privacy | Avatar and streaming features |
| Avatar and streaming | Only reply to viewers who address *{name}* | Stream viewers get a reply only when they address *{name}* by name. | `presence.requireAddressedStreamMessages` (`sw_presence_requireAddressedStreamMessages`), default on | Preference | Live |  | Discord, stream and game connections |
| Avatar and streaming | Speak stream replies | Read answers to stream chat out loud. | `presence.speakStreamReplies` (`sw_presence_speakStreamReplies`), default off | Preference | Live |  | Discord, stream and game connections |
| Local API | Local API for other apps | Let other programs on this PC talk to Revia with the install token. They are treated as a shared guest by default. | `localApi.enabled` (`sw_localApi_enabled`), default off | Owner overlay (all companions) | After restart | Privacy |  |
| Local API | Trust the local API as me | Treat every API request as if you typed it, with your memories, tools and approvals. **Warning:** Any program on this PC with the install token speaks as you. | `localApi.trustedPrivate` (`sw_localApi_trustedPrivate`), default off | Owner overlay (all companions) | After restart | Risky, asks when turned on | Local API for other apps |
| Tool server: *server* | Connect *server* | Start this tool server and offer its allowed tools to *{name}*. **Warning:** The server's allowed tools become available to *{name}*, and the server runs with your account's access. | `mcp.servers[*].enabled` (`sw_mcp_*_enabled`), default off | Owner overlay (all companions) | Live | Risky, asks when turned on |  |
| Tool server: *server* | Ask before each tool call | *{name}* asks you before every call to this server. Off lets its allowed tools run on their own. **Warning:** This server's allowed tools will run without asking you first. | `mcp.servers[*].requireConfirmation` (`sw_mcp_*_requireConfirmation`), default on | Owner overlay (all companions) | Live | Risky, asks when turned off | Connect *server* |

### Advanced

| Group | Switch | What it does | Key | Stored in | Applies | Risk | Needs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Self-improvement reviews | Self-improvement reviews | While you are away, *{name}* reviews Revia's code and writes suggested fixes. Your installed code is never changed. | `improvement.enabled` (`sw_improvement_enabled`), default on | Owner overlay (all companions) | After restart |  |  |
| Self-improvement reviews | Look for improvements without being asked | Review parts of Revia every couple of hours, not only after a problem was recorded. | `improvement.explore` (`sw_improvement_explore`), default on | Owner overlay (all companions) | After restart |  | Self-improvement reviews |
| Self-improvement reviews | Build and test suggested improvements | Compile and test each suggestion in a separate copy. Uses a lot of CPU. **Warning:** Model-written code is compiled and run on this PC. | `improvement.verify` (`sw_improvement_verify`), default on | Owner overlay (all companions) | After restart | Risky, asks when turned on | Self-improvement reviews |
| Performance and hardware | Automatic model tuning | Fit the main model's memory use to your hardware automatically. Off uses the manual numbers in settings.json. | `llm.autoTune` (`sw_llm_autoTune`), default on | Owner overlay (all companions) | After restart |  |  |
| Performance and hardware | Automatic reply length | Choose a reply length for each turn, up to the maximum. Ignored while a profile sets its own reply limit. | `llm.autoMaxTokens` (`sw_llm_autoMaxTokens`), default on | Owner overlay (all companions) | After restart |  |  |
| Performance and hardware | Start the chat model server automatically | Revia starts and stops its own chat model server. Off connects to a server you run yourself, and chat stops working if none is running. | `llm.autoStartServer` (`sw_llm_autoStartServer`), default on | Owner overlay (all companions) | After restart |  |  |
| Performance and hardware | Start the memory search server automatically | Revia starts its own memory search server. Off connects to one you run yourself. | `embedding.autoStartServer` (`sw_embedding_autoStartServer`), default on | Owner overlay (all companions) | After restart |  | Search memories by meaning |
| Performance and hardware | Use the GPU for speech recognition | Run speech recognition on a graphics card. Automatic hardware planning decides this while it is on. | `speechRecognition.useGpu` (`sw_speechRecognition_useGpu`), default off | Owner overlay (all companions) | After restart |  |  |
| Performance and hardware | Automatic hardware planning | At startup, decide which graphics card or the CPU runs each model. Off uses the assignments in settings.json. | `resources.autoPlan` (`sw_resources_autoPlan`), default on | Owner overlay (all companions) | After restart |  |  |
| Performance and hardware | Split the chat model across GPUs | Let the main model use two graphics cards when it does not fit on one. | `resources.allowChatModelSplit` (`sw_resources_allowChatModelSplit`), default off | Owner overlay (all companions) | After restart |  |  |

## Settings rows that are not switches

| Section | Row | Kept in |
| --- | --- | --- |
| Appearance | Theme: System, Light or Dark. System follows the Windows app mode setting. | QSettings `appearance/theme` |
| Appearance | Chat text size: Small, Default or Large | QSettings `appearance/chatTextSize` |
| Chat | At most N unprompted openings per hour (0 = never), enabled only with "Let {name} start conversations" | Preference `initiative.maxPerHour` |
| Chat | Basic safety filter: always on (information only) | none |
| Chat | Temperature and reply limit values beside the two profile override switches | Active profile |
| Voice | Microphone: device, refresh and test | QSettings `input/microphoneDevice` |
| Voice | Voice generation device: opens the Resources window | Preference `resources.voiceDevice` |
| Vision | Camera snapshots: opens Settings, Permissions | Capabilities |
| Models | Use the cloud for: Never, Only when I ask, Hard questions | Owner overlay `llm.cloud.useFor` |
| Permissions | Requested tasks (approval mode), approved applications and controls, approved executables, approved site origins (in Internet) | Capabilities |
| Advanced | Sample resource usage every N seconds (0 = off) | Preference `resources.usageSampleSeconds` |

The two list editors, approved site origins and approved executables, are the only places with an
Apply button. While either holds unsaved edits a footer reads "Unsaved changes" with Apply and
Discard, and closing Settings discards them. Every switch beside them commits as soon as it
changes.

## Booleans without a switch

These are true/false values in Revia's configuration or code that Settings deliberately leaves
out.

| Value | Why it has no switch |
| --- | --- |
| `llm.shutdownServerOnExit`, `embedding.shutdownServerOnExit`, `image.shutdownOnExit` | Process housekeeping, read only when Revia stops. Nothing you can see changes. |
| `llm.stablePromptPrefix` | An internal prompt-cache optimisation with no visible effect on behaviour. |
| `llm.bVerifyServerKey`, `embedding.bVerifyServerKey` | Derived at startup from the matching "Start ... server automatically" switch; never read from a file. |
| `llm.bAllowPromptCache` | Derived for each companion (always off for non-legacy companions); never read from a file. |
| `intelligence.fast.visionEnabled`, `intelligence.expert.visionEnabled` | Must match the model file chosen for that tier; the validator rejects them without a projector path. |
| `intelligence.fast.onDemand` | Read by configManager but never used: only the Expert tier honours on-demand loading (`settings.intelligence.expert.bOnDemand` is the only reader, in `Private/Runtime/reviaSession.cpp`). |
| `vision.requireConfirmation` | Read by configManager but never used. Screen actions always go through policy and confirmation, so a switch would do nothing. |
| `computerControl.recordingEnabled` | Read by configManager but never used. Recording is chosen per session and per app with `/controller record`. |
| `speech.qwenParallelLongReplies`, `qwenLowLatencyPhrase`, `qwenCudaGraph`, `qwenTalkerGraph`, `qwenBatchReplyPhrases`, `qwenDirectPcm`, `qwenPrecomputeVoicePrompt` | Voice engine and GPU internals with measured trade-offs (`qwenBatchReplyPhrases` ships off on purpose because it causes long silences). They stay in `Config/settings.json`. |
| `speechRecognition.useServer` | Engine plumbing that needs a valid server executable and port. |
| `capabilities.createMissingApprovedRoots` | Setup plumbing: creates sandbox folders at startup and grants nothing. Goal sandboxes force it off. |
| Image generation capability `enabled` and `autonomous` | Derived by the host from "Picture generation" and from initiative at startup; never read from a file. |
| `capabilities.mode` | A choice of four approval modes, shown as "Requested tasks" in Permissions. |
| `capabilities.autoApproveRiskThrough` | A risk level, not a true/false value, and it has no setter. |
| Profile `shouldSpeak` | Present in profile files but never read; spoken replies follow "Speak replies". |
| `REVIA_WEB_ENABLED` and the other web demo environment variables | A developer deployment switch read from the environment, not a setting. |
| The desktop-control stop | An emergency control, not a setting: "Stop desktop control" in Permissions, the chat header while it is active, and Ctrl+Alt+Shift. |
| Audience recognition consent | A consent action recorded per enrolled person, kept in the Audience window as `recognitionConsent`. |
| Memory window "important only" filter, `activity/filter`, `vision/cameraLink`, `palette/recent` | View state with no effect on behaviour. |
| `input/microphoneDevice` | Chosen in the Microphone row rather than with a switch. |
| `settingsDialog/geometry`, `panels/<key>/geometry`, `panels/openSet`, `window/trayHintShown` | Window placement and one-time hints the shell keeps for itself. |
| Agent worker scope bools, goal-store scope snapshots, web guest control state, skill manifest review flags | Internal files and records the runtime writes for each workflow, goal, guest or skill; not owner settings. |
| `VocalizationLimits::bForbidImmediateRepeat` | A code constant, not read from any file. |
| Runtime copies such as the llama.cpp process flags and `UserPreferenceSnapshot` | Internal copies of the settings above. |

Five values the settings inventory first excluded are switches after all, because every
user-facing boolean gets one: "Start the chat model server automatically" (`llm.autoStartServer`),
"Start the memory search server automatically" (`embedding.autoStartServer`), the two "Warm up ...
at startup" switches (which send one warm-up request once the model runs; they do not preload it),
and "Use the GPU for speech recognition" (`speechRecognition.useGpu`, which automatic hardware
planning overrides while it is on). They sit in Models and Advanced, and their descriptions say
what turning them off costs.
