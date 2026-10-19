# Desktop navigation

The window follows the Claude and Codex desktop apps: a left sidebar, the chat in a
centred column, and every other page in a window of its own. Settings are one dialog
(see [Desktop settings](DESKTOP_SETTINGS.md)). The pages themselves are the existing
ones; only where they are shown changed.

## The main window

The window uses the native Windows frame (snap layouts, the system shadow, Alt+Space).
Its caption follows the light or dark scheme. Default size 1180 × 780, minimum 760 × 540.

**Sidebar** (264 px, or a 56 px rail of icons):

- **New chat**, **Search chats** and a **Recents** heading that folds the chat list.
- One line per chat, with the current chat in bold. The date and length are in the row's
  tooltip. Right-click a row to rename, export (Markdown or JSON) or delete it. F2 and
  Delete act on the highlighted chat while the list has focus (the current chat when no row is
  highlighted).
- At the bottom, the companion's menu button and Settings. The menu holds the companion
  switcher, every panel grouped as Workspace, Companion, Studio and Runtime, Stop desktop
  control (while it is active), Settings, the command palette, the shortcut sheet, Keep
  Revia on top and Quit Revia.

Below 900 px of window width the sidebar starts as the rail. Expanding it there lasts
until the window widens again. At 900 px and wider, **Keep the sidebar open** (Settings ›
Appearance) applies.

**Chat column:**

- The header shows the chat's title and the live privacy states (below). It also has the
  desktop **Stop**, the status pill and the chat menu (rename, export, delete,
  documents, review the last answer). The status pill reads Ready, Thinking, Replying,
  Listening, Speaking, Needs your OK, Controlling desktop, Problem, and so on. Click it for the
  six live chips (affect, voice, microphone, actions, vision, app awareness), or, while an
  action waits for approval, to bring that question forward. **Details** appears in the
  header only on Problem and opens the Activity window.
- The transcript reads at a 760 px measure. Your turns are bubbles on the right; replies
  are plain text with a speaker line where the speaker changes. Thinking, checking,
  memory and offers are small muted rows. A thinking or checking row opens with a click, also
  while the reply is still being prepared. **Show {name}'s thinking steps** hides them.
  Only the newest finished reply carries Copy, Regenerate and Review. Right-click any
  reply for **Copy message**.
- A turn that could not finish draws one red row with **Retry** and **Details**. A turn
  you stopped reads **Stopped**. Neither becomes a chat entry.
- If you scroll up, new messages do not move the view. **Latest** appears and takes you
  back down.
- An empty chat greets you by time of day. While the runtime starts it says so. When the
  language model is not running it links to Activity and Resources.
- The conversation strip (current status, Details, Review answer) shows above the
  composer only while something is running, while its details are open or while it
  flags the latest reply.

**Composer:** a rounded card with the message box. It grows up to eight lines, 200 px or
30% of the window. Its toolbar holds:

- **+** for Attach files, Documents and Use screen.
- The profile chip, which switches profiles, holds the session's cloud gate while the
  cloud is allowed, and has Manage profiles.
- The voice-health pill, shown only while synthesis is not known to work.
- The hands-free chip.
- Mic, Stop and Send. While a turn, task or studio operation runs, Stop takes Send's
  place. While only speech or the microphone runs, Stop sits beside Send. While Revia starts,
  Send stays in its place, disabled until Revia is ready, beside a Stop that cancels the startup.

Long-lived modes, such as editing your last message or waiting for the previous request,
show as a line above the message box. Short notices show as a toast over the composer.

## Privacy indicators

Each indicator is in the header only while its state is live. All of them come from
existing runtime events; no event was added.

| Indicator | Live while | Click |
| --- | --- | --- |
| Microphone | listening, transcribing or hands-free | stops listening (hands-free: turns it off) |
| App awareness | the Perception status is Watching | pauses app awareness |
| Screen | Vision reports continuous awareness (its Watching and Aware passes, or the vision status naming it) | turns **Keep track of my screen** off |
| Camera | the Camera status is Capturing | none |
| Desktop control | an Automation action is Running and desktop control is not stopped | same as the header Stop |

The header **Stop**, the menu entry and the palette entry all stop desktop control and
the running turn.

## Panel windows

Each panel is one window, made once and only hidden afterwards. It opens beside the main
window, to the right if it fits, else to the left, else at the screen's right edge.
Later windows step down 32 px. Each window remembers its own size and place. Its header
has **Place beside chat**, which narrows the main window and puts the panel next to it,
and, where a Settings section matches, a gear for it. Esc closes a panel unless you are
editing text; Ctrl+W always closes it.

| Group | Panels |
| --- | --- |
| Workspace | Canvas, Vision |
| Companion | Profiles, Mind, Memory (Ctrl+Shift+M), Voice, Presence, Audience |
| Studio | Agents, Learning, Development |
| Runtime | Activity (Ctrl+Shift+A, with its warning count in the title), Pipelines, Internet activity, Resources |

When the window is hidden to the tray, the open panels hide with it and come back with
**Open Revia**. **Reopen panel windows at startup** brings back the panels that were open
at the last close.

Panels are owned windows, so they stay above the main window. Placement keeps them off
the chat. The design called for a Windows experiment: drop each panel's transient
parent so the chat can be raised above it. That experiment needs Windows with Qt 6.8
and has not been run. `PanelHost::SetUnowned(false)`, the owned fallback, is what ships.

## Command palette and shortcuts

Ctrl+K opens a search over actions, panels (open, or place beside the chat), every
setting and your chats. Up and Down move, Enter runs, Esc closes. The palette does not
open while a companion switch or shutdown has locked the window.

| Scope | Keys |
| --- | --- |
| Window | Ctrl+N new chat, Ctrl+B sidebar, Ctrl+F search chats (palette in the rail), Ctrl+Space listen, F6 / Shift+F6 move between sidebar, transcript and message box |
| Message box | Enter send, Shift+Enter new line, Esc cancels an edit, else stops a running reply, task or studio operation (never the startup, speech or the microphone) |
| Chat list | F2 rename, Delete delete (the highlighted chat) |
| Anywhere | Ctrl+K palette, Ctrl+, Settings, Ctrl+/ shortcut sheet, Ctrl+Shift+A Activity, Ctrl+Shift+M Memory |
| Panel windows | Ctrl+W close, Esc close unless editing text |
| Settings | Ctrl+F search, Esc clears the search, then closes (not while you are typing in a box) |

## Closing, the tray and restarts

Closing the window hides it to the tray when a tray is available and **Keep running in the
tray when closed** is on. The tray note shows only the first time. Otherwise closing
shuts Revia down properly, with servers stopped, rather than leaving a windowless process.
A restart from Settings closes everything, then starts a new process. On Windows the new
process waits for the old one to exit.

A companion switch locks the sidebar, the chat, every panel window and Settings, and
says "Switching to {name}…" over the chat. Shutdown locks them for good.

## How it is built

- `Desktop/windowShell.cpp` builds the panel windows, the Settings dialog with its session
  backend, the sidebar, the chat column, menus, palette and shortcuts.
  `Desktop/windowLayout.cpp` holds the size rules.
- `Desktop/tabNavigation.cpp` keeps deep links working. `SelectNavigationPage` selects the
  page in any tab widget or Settings stack on its way up, then shows that window.
  `PolishSectionTabs` gives every remaining tab bar (Mind's views) the section look.
- `Desktop/reviaWindow.ui` is now a page factory. The page parking tab widget is deleted
  once every page has moved into its window.
- Settings writes run one at a time on `Desktop/serialWorker.h`. It is joined before a
  companion's session retires, and a write that finishes after a switch is dropped.

## Checks

`Revia.DesktopStopPolicy` walks all 30 destinations (the chat, 14 panel pages, Mind's
four views and the 11 Settings sections) at 760 × 540, 1040 × 720 and 1600 × 1000. It
checks that each one shows, that its menu entry follows, that nothing scrolls sideways
and that no tab bar needs arrows. It also covers the companion-switch lock, the Activity
count, error Details and Voice Details. Its `RunShellUi` part covers:

- every name in `desktopObjectNames.h` that should exist;
- the palette opening Memory;
- Settings opening on Voice;
- the hands-free dependency;
- the toast;
- the theme reaching the transcript;
- the failed-turn row;
- the Send and Stop slot while Revia starts and while a turn runs, and Esc;
- scroll pinning;
- the desktop indicator and Stop;
- the sidebar's buttons keeping their size through a collapse and expand;
- a fenced code block drawn as one box, the empty chat's greeting a third of the way down,
  the 28px state pill and the sidebar menu's group headings.

`RunSettingsReadBackUi` checks that the two number rows the window adds to Settings follow a
change made elsewhere and never write an untouched value back, that the window's own controls in
Settings ignore a passing mouse wheel, and that choosing Hard questions for the cloud asks first.
`RunShutdownUi` quits with the window showing and expects the event loop to end.

`Revia.Theme`, `Revia.SettingsDialog`, `Revia.ShellWidgets` and `Revia.SettingsBackend` run
the slices' own suites in the same binary.

For a picture of one window, run `ReviaDesktop --ui-screenshot <file.png> --ui-screenshot-tab <target>`,
optionally with `--ui-theme light|dark` and `--ui-size 1280x800`. The target is `chat`, a panel
key or title (`memory`), `settings`, or `settings:<section>` for a section that shares a
panel's name (`settings:voice`).
