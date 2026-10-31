"""Builds docs/CODE_MAP.md: every tracked file, what it does, how big it is, and how domains connect.

Usage, from the repository root:
    python Tools/CodeMap/build_code_map.py            writes docs/CODE_MAP.md
    python Tools/CodeMap/build_code_map.py --check    exits 1 if the map is stale or a file lacks a description

Descriptions are hand-written in descriptions.py; line counts and dependencies are measured here.
Tests are described from their own test names or assertion messages.
"""
import collections, fnmatch, re, runpy, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "docs" / "CODE_MAP.md"
CHECK = "--check" in sys.argv[1:]
scope = runpy.run_path(str(HERE / "descriptions.py"))
D, N = scope["D"], scope["N"]
head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
files = subprocess.run(["git", "-C", str(ROOT), "ls-files"], capture_output=True, text=True).stdout.split()

CODE = (".h", ".hpp", ".cpp", ".c", ".py", ".ps1", ".js", ".mjs", ".ts", ".qml", ".bat", ".cmd", ".sh")
def leading_comment(text, suffix):
    if suffix == ".py":
        m = re.match(r'\s*(?:#[^\n]*\n\s*)*(?:"""|\'\'\')(.+?)(?:"""|\'\'\')', text, re.S)
        return " ".join(m.group(1).split())[:400] if m else ""
    if suffix in (".ps1", ".bat", ".cmd", ".sh"):
        return " ".join(l.strip("#").strip() for l in text.splitlines()[:20] if l.startswith("#") and not l.startswith("#!"))[:400]
    block = []
    for line in text.splitlines()[:400]:
        s = line.strip()
        if s.startswith("//"):
            block.append(s.lstrip("/").strip())
        elif block:
            joined = " ".join(x for x in block if x)
            if len(joined) > 25: return joined[:400]
            block = []
    return ""
F = {}
for f in files:
    p = ROOT / f
    if p.suffix.lower() not in CODE or not p.is_file(): continue
    text = p.read_text(encoding="utf-8", errors="replace")
    F[f] = {"lines": text.count("\n") + 1, "desc": leading_comment(text, p.suffix.lower()),
            "includes": re.findall(r'#include\s+"([^"]+)"', text) if p.suffix in (".h", ".hpp", ".cpp", ".c") else []}

def code_domain(path):
    p = path.split("/")
    return (p[1] if len(p) > 2 else "Core") if p[0] in ("Public", "Private") else None
edges = collections.Counter()
for f, v in F.items():
    src = code_domain(f)
    if not src: continue
    for inc in v["includes"]:
        m = re.match(r"([A-Z][A-Za-z]+)/", inc)
        if m and m.group(1) != src: edges[(src, m.group(1))] += 1

def lines_of(path):
    if path in F: return F[path]["lines"]
    try: return (ROOT / path).read_bytes().count(b"\n") + 1
    except Exception: return 0

def unit_of(path):
    p = path.split("/")
    if p[0] in ("Public", "Private") and path.endswith((".h", ".cpp")):
        return "/".join(p[1:]).rsplit(".", 1)[0] if len(p) > 2 else "main"
    if p[0] == "Desktop" and path.endswith((".h", ".cpp")):
        return path.rsplit(".", 1)[0]
    return path

def describe(unit):
    if unit in D: return D[unit]
    best = None
    for key in D:
        if "*" in key and fnmatch.fnmatch(unit, key):
            if best is None or len(key) > len(best): best = key
    return D[best] if best else None

# ---------- tests: described from their own check names ----------
def humanize(name):
    name = re.sub(r"^(Test|test_)", "", name)
    words = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", name).replace("_", " ").split()
    return " ".join(w.lower() if i else w.capitalize() for i, w in enumerate(words))
def test_summary(path):
    text = (ROOT / path).read_text(errors="replace")
    names = re.findall(r"^(?:static\s+)?void\s+(Test\w+)\s*\(", text, re.M) + re.findall(r"^\s+def\s+(test_\w+)", text, re.M)
    names += re.findall(r"^\s*(?:test|it)\(\s*['\"]([^'\"]{3,90})", text, re.M)
    stem = re.sub(r"(Tests?|\.test|\.live|Live)$", "", Path(path).stem.split(".")[0])
    subject = humanize(stem) if stem else Path(path).stem
    if names:
        shown = "; ".join(humanize(n) if n.startswith(("Test", "test_")) else n for n in names[:3])
        more = f" (+{len(names) - 3} more)" if len(names) > 3 else ""
        return f"{len(names)} checks. {shown}{more}."
    messages = re.findall(r'(?:Check|Require|Expect|Assert|CHECK|REQUIRE|Fail|expect)\w*\s*\([^;"]*"([A-Z][^"]{15,140})"', text)
    messages = list(dict.fromkeys(m.rstrip(" :") for m in messages if len(m.split()) >= 4 and "/" not in m.split()[0]))
    setup = re.compile(r"could not|cannot (?:start|open|bind|create|write|read)|fixture|failed to start|did not start|needs (?:its|the)|requires the|must be available|is missing|not found", re.I)
    behaviour = [m for m in messages if not setup.search(m)]
    messages = behaviour or messages
    if messages:
        shown = "; ".join(f"“{m.rstrip('.')}”" for m in messages[:2])
        return f"{len(messages)} assertion{'s' if len(messages) != 1 else ''}, e.g. {shown}."
    desc = F.get(path, {}).get("desc", "")
    if desc: return re.split(r"(?<=[.!?])\s", desc)[0][:180]
    if "Fixture" in path or "fixture" in path.lower(): return f"Fixture or harness for {subject.lower()} tests."
    if path.endswith(".h"): return f"Shared test helpers ({subject.lower()})."
    return f"Checks for {subject.lower()}."

# ---------- coverage ----------
units = collections.OrderedDict()
for f in files:
    units.setdefault(unit_of(f), []).append(f)
missing = []
for u, fs in units.items():
    if u.startswith("Tests/") or u.startswith("docs/"): continue
    if describe(u) is None: missing.append(u)
if missing:
    print("MISSING DESCRIPTIONS:"); print("\n".join(missing)); sys.exit(1)

def domain_of(unit):
    return unit.split("/")[0] if "/" in unit else unit
size = collections.Counter(); ucount = collections.Counter()
for u, fs in units.items():
    d = domain_of(u) if any(f.startswith(("Public/", "Private/")) for f in fs) else None
    if d == "main": d = "Core"
    if d: size[d] += sum(lines_of(f) for f in fs); ucount[d] += 1

out_deps = collections.defaultdict(set); in_deps = collections.defaultdict(set)
for (a, b) in edges: out_deps[a].add(b); in_deps[b].add(a)

def table(rows):
    s = "| Unit | Lines | What it does |\n| --- | ---: | --- |\n"
    for u, fs in rows:
        s += f"| `{u}` | {sum(lines_of(f) for f in fs):,} | {describe(u)} |\n"
    return s

LAYERS = [
    ("Orchestration", ["Runtime"]),
    ("Conversation and thinking", ["Agents", "LLM", "Intelligence", "Memory", "Identity", "Emotion", "Autonomy", "Initiative", "Learning", "Evaluation", "Improvement"]),
    ("Senses and expression", ["Speech", "Vision", "Perception", "Attachments", "Visual", "Presence", "Presentation", "Performance", "Content"]),
    ("Acting in the world", ["Actions", "Planning", "Policy", "Audit", "Filesystem", "Process", "Windows", "Computer", "Goals", "Browser", "Internet", "Workspace", "Mcp", "Skills"]),
    ("Foundations", ["Core", "Resources", "Diagnostics"]),
]
covered = {d for _, ds in LAYERS for d in ds}
assert covered >= set(size) - {"main"}, set(size) - covered

md = []
w = md.append
w(f"""# Revia code map

The complete map of the codebase: what each part does, where it lives, how the parts connect, and
why they exist. It covers every tracked file. Pass 1 of the improvement work: it records what is,
not what should change. The measured observations at the end are the input for pass 2.

Generated at commit `{head}` from hand-written descriptions plus measured facts (line counts,
`#include` dependencies). For the reasoning behind the design, the invariants and the turn path in
detail, read [ARCHITECTURE.md](ARCHITECTURE.md); this map links into it rather than repeating it.

Regenerate it with `python Tools/CodeMap/build_code_map.py` after changing code; the descriptions
live in `Tools/CodeMap/descriptions.py`. The generator refuses to write the map while any tracked
file lacks a description, and `--check` reports a stale map.

## Contents

1. [How to read this map](#how-to-read-this-map)
2. [The big picture](#the-big-picture)
3. [Where data lives](#where-data-lives)
4. [Subsystems](#subsystems), by layer
5. [Desktop shell](#desktop-shell)
6. [Tools and services](#tools-and-services)
7. [Tests](#tests)
8. [Configuration, assets, build and CI](#configuration-assets-build-and-ci)
9. [Measured observations for pass 2](#measured-observations-for-pass-2)

## How to read this map

- **Unit.** `Public/<Domain>/x.h` declares a contract and `Private/<Domain>/x.cpp` implements it;
  the map lists them as one unit, `<Domain>/x`. A unit with only a header is a value type or
  settings struct. Desktop `.h`/`.cpp` pairs are one unit too.
- **Lines** are physical lines of the unit's files at `{head}`.
- **Depends on / used by** come from `#include` lines between domains. They show compile-time
  coupling, which is a good proxy for "knows about".
- The code is C++20 with Qt 6 for the desktop. Python, Node and PowerShell appear only in `Tools`
  (workers, adapters, setup) and their tests.

Size at a glance: {sum(lines_of(f) for f in files if f in F):,} lines of code in {len(F):,} files.
Production C++ is {sum(size.values()):,} lines in {sum(ucount.values())} units across {len(size)} domains; the Qt desktop
{sum(lines_of(f) for f in files if f.startswith('Desktop/') and f in F):,}; tests {sum(lines_of(f) for f in files if f.startswith('Tests/') and f in F):,}; tools {sum(lines_of(f) for f in files if f.startswith('Tools/') and f in F):,}.

## The big picture

Revia is a local companion: a desktop application that talks (typed and spoken), sees (screen,
camera, uploaded media), remembers, has a persistent personality and emotions, and can act on the
computer within explicit permissions. Models run locally through llama.cpp and Python workers that
Revia starts and owns; a cloud model is optional.

### Processes

Revia is one native process that owns several child processes. Every child listens only on
loopback and is started, supervised and stopped by a C++ owner.

```mermaid
flowchart LR
    subgraph Revia["Revia process (ReviaDesktop.exe or the R_E_V_I_A terminal)"]
        S["ReviaSession"]
    end
    S --> L1["llama-server: Main / Fast / Expert tiers<br/>(LLM/LLamaCPP)"]
    S --> L2["llama-server: embeddings<br/>(LLM/LLamaCPP)"]
    S --> W["whisper-server<br/>(Speech)"]
    S --> T["Qwen3-TTS workers, one per device<br/>Tools/qwen_tts_service.py (Speech)"]
    S --> I["Image worker<br/>Tools/revia_image_service.py (Visual)"]
    S --> V["Video worker per job<br/>Tools/revia_video_worker.py (Visual)"]
    S --> B["Browser hosts (Node)<br/>Tools/Browser (Browser, Internet)"]
    S --> M["Optional memory NLI and reranker<br/>Tools/Workers (Memory)"]
    S --> MC["MCP servers (Mcp)"]
    S --> G["Guest VM worker over SSH<br/>Tools/Guest (Workspace)"]
    S --> API["Local HTTP API and web guest<br/>(Presence, in process)"]
    P["Presence adapters: Live2D, Discord voice, web demo<br/>Tools/Presence"] -.files and local API.-> API
    S -.avatar state file.-> P
```

### Layers inside the process

```mermaid
flowchart TB
    Shell["Shells: Desktop (Qt) and Core/reviaApp (terminal)"] --> Session["Runtime: ReviaSession and ConversationRuntime"]
    Session --> Think["Conversation and thinking<br/>Agents, LLM, Intelligence, Memory, Identity, Emotion,<br/>Autonomy, Initiative, Learning, Evaluation, Improvement"]
    Session --> Senses["Senses and expression<br/>Speech, Vision, Perception, Attachments, Visual,<br/>Presence, Presentation, Performance, Content"]
    Session --> Act["Acting in the world<br/>Actions, Planning, Policy, Audit, Filesystem, Process, Windows,<br/>Computer, Goals, Browser, Internet, Workspace, Mcp, Skills"]
    Think --> Found["Foundations: Core, Resources, Diagnostics"]
    Senses --> Found
    Act --> Found
```

This is the intended shape. The include graph shows where the code departs from it; see the
[measured observations](#measured-observations-for-pass-2).

### One conversation turn, in brief

1. The shell calls `ReviaSession` (typed text, a transcript, an adapter message).
2. `InputArbiter` merges and orders input into a batch; the session admits one foreground
   operation and builds its task contract.
3. `ConversationRuntime` routes the turn: `IntelligenceRouter` picks a tier, and grounding is
   gathered (memory, recall, files, internet lookup, arithmetic, saved source).
4. `TurnCoordinator` runs `ConversationAgent` through `messageRouter` to `llamaCppService`, which
   fits the prompt and streams the reply. The tool loop executes chat tools through
   `ActionRuntime`, so policy and audit still apply.
5. The reply is filtered (`responseFilter`, style policy), shown, and spoken sentence by sentence
   (`speechService`), while emotion, identity and presence are updated.
6. Afterwards the `MemoryAgent` evaluates the turn in the background and may store memories.

The full path with diagrams is in [ARCHITECTURE.md, Current turn path](ARCHITECTURE.md#current-turn-path).

### Every side effect, the same way

`Planning` turns words into a typed `ActionRequest`. `Policy` evaluates risk and permission;
`Actions` asks for confirmation if needed and dispatches it to one narrow executor (`Filesystem`,
`Process`, `Windows`, `Browser`, `Internet`, `Visual`, `Workspace`). `Audit` records intent before
and result after. Chat tools, goals, computer control, skills and MCP all reuse this path. See
[ARCHITECTURE.md, Trust boundary](ARCHITECTURE.md#trust-boundary).

## Where data lives

| Location | Contents | Owner |
| --- | --- | --- |
| `Config/` (tracked) | Default settings, capabilities, profiles, model manifest, evaluation corpora, starter voice | `Core/configManager`, `Policy/permissionStore` |
| `RuntimeData/Companions/registry.json` | Which companions exist and which is selected | `Runtime/companion` |
| Each companion's root | That companion's `RuntimeData` tree, `Memory/revia_memory.db`, `Memory/revia_conversations.db`, `Goals/revia_goals.db`, logs | `Runtime/companion` |
| `RuntimeData/Settings/owner-settings.json` | The owner's overrides over `Config/settings.json` | `Core/settingsFileEditor` |
| `RuntimeData/Preferences/` | Non-authority user preferences | `Core/preferenceStore` |
| `RuntimeData/Identity/identity.json` | Development, preferences, relationships | `Identity/identityStore` |
| `RuntimeData/Agents/` | Agent Studio workflow, provider, tool scope, delegation | `Runtime/agentStudioRuntime`, `Runtime/sessionDelegation` |
| `RuntimeData/Learning/` | Lesson records, kept captures, corrections, dataset lineage | `Learning/*` |
| `RuntimeData/Voices/`, `Songs/`, `Images/`, `Diagrams/`, `Camera/`, `Vision/` | Voices and presets, songs, generated media, captures | Speech, Performance, Visual, Vision |
| `RuntimeData/Presence/` (`Inbox`, `Outbox`, avatar state) | Adapter traffic and the avatar file renderers read | `Presence/presenceRuntime` |
| `RuntimeData/Improvement/`, `Evaluations/`, `ComputerExperience/`, `Skills/`, `Reminders/`, `Browser/Profile`, `VirtualMachine/`, `LocalApi/` | Proposals and workbench, evaluation runs, recorded experiences, skill packages, reminders, private browser profile, guest VM settings, local API token | the matching domain |

`Runtime/runtimeDataBootstrap` creates this layout on first run without overwriting anything.

## Subsystems
""")

for layer, domains in LAYERS:
    w(f"\n### {layer}\n")
    for d in domains:
        rows = [(u, fs) for u, fs in units.items() if any(f.startswith(("Public/", "Private/")) for f in fs) and domain_of(u) == d]
        if d == "Core":
            rows += [(u, fs) for u, fs in units.items() if u == "main"]
        rows.sort(key=lambda r: (-sum(lines_of(f) for f in r[1]), r[0]))
        w(f"\n#### {d}\n\n{N.get(d, '').strip()}\n")
        w(f"\n{size[d] + (lines_of('Private/main.cpp') if d == 'Core' else 0):,} lines in {len(rows)} units. "
          f"**Depends on:** {', '.join(sorted(out_deps[d])) or 'nothing else'}. "
          f"**Used by:** {', '.join(sorted(in_deps[d])) or 'no other domain'}.\n\n")
        w(table(rows))

# Desktop
desk = [(u, fs) for u, fs in units.items() if u.startswith("Desktop/")]
desk_code = sorted([r for r in desk if any(f.endswith((".h", ".cpp")) for f in r[1])], key=lambda r: -sum(lines_of(f) for f in r[1]))
desk_res = [r for r in desk if r not in desk_code]
w(f"""
## Desktop shell

The Qt 6 desktop (`ReviaDesktop`). It renders; it owns no policy, model or memory. `ReviaWindow`
is one class implemented across `reviaWindow.cpp`, `windowShell.cpp`, `companionStudio.cpp` and
`windowLayout.cpp`. It binds a `ReviaSession`, receives `RuntimeEvent`s on the UI thread, and
forwards user operations to the session, usually on worker threads. Panels are narrow views that
consume snapshots and submit explicit owner decisions. See also
[DESKTOP_NAVIGATION.md](DESKTOP_NAVIGATION.md) and [DESKTOP_SETTINGS.md](DESKTOP_SETTINGS.md).

{sum(lines_of(f) for r in desk for f in r[1] if f in F):,} lines of C++ in {len(desk_code)} units.

""")
w(table(desk_code))
res_groups = collections.OrderedDict()
for u, fs in desk_res:
    key = "Desktop/*.svg" if u.endswith(".svg") else u
    res_groups.setdefault(key, []).extend(fs)
w("\nResources:\n\n| File | What it is |\n| --- | --- |\n")
for k, fs in res_groups.items():
    w(f"| `{k}` | {D[k] if k in D else describe(k)}{f' ({len(fs)} files)' if len(fs) > 1 else ''} |\n")

# Tools
w("""
## Tools and services

Everything outside the native process: the Python workers Revia starts, Node hosts and presence
adapters, setup and install scripts, quality and evaluation tools, and the optional training
pipeline ([OFFLINE_TRAINING.md](OFFLINE_TRAINING.md)).

""")
tool_groups = collections.OrderedDict([
    ("Workers Revia starts (Python)", ["Tools/qwen_tts_service.py", "Tools/qwen_lowlatency.py", "Tools/revia_image_service.py", "Tools/revia_video_worker.py", "Tools/Workers/"]),
    ("Optional offline training pipeline", ["Tools/revia_training_worker.py", "Tools/revia_adapter_export.py", "Tools/revia_adapter_evaluation.py", "Tools/revia_dataset_capture.py", "Tools/install_training_assets.py", "Tools/Computer/"]),
    ("Quality and evaluation tools", ["Tools/Quality/", "Tools/RunArchitectureEvaluation.ps1", "Tools/RunCognitionLiveCampaign.ps1", "Tools/Benchmark-ReviaTTS.ps1", "Tools/VerifyStudioCandidate.ps1", "Tools/VerifyWorkbench.ps1"]),
    ("Browser and guest workspace", ["Tools/Browser/", "Tools/Guest/"]),
    ("Presence adapters", ["Tools/Presence/"]),
    ("Setup, install and build", ["setup.bat", "Tools/Setup-Revia.ps1", "Tools/Build.ps1", "Tools/Build/", "Tools/RunDesktop.ps1", "Tools/HealthCheck.ps1", "Tools/ReviaAcceleration.ps1", "Tools/Install", "Tools/Download", "Tools/SelectInstalledMainModel.ps1", "Tools/CreateReviaClass.py", "Tools/CodeMap/"]),
])
placed = set()
tool_units = [(u, fs) for u, fs in units.items() if u.startswith("Tools/") or u == "setup.bat"]
for title, prefixes in tool_groups.items():
    rows = [(u, fs) for u, fs in tool_units if u not in placed and any(u.startswith(p) for p in prefixes)]
    placed |= {u for u, _ in rows}
    w(f"\n### {title}\n\n| File | Lines | What it does |\n| --- | ---: | --- |\n")
    for u, fs in rows:
        w(f"| `{u}` | {lines_of(fs[0]):,} | {describe(u)} |\n")
rest = [(u, fs) for u, fs in tool_units if u not in placed]
if rest:
    w("\n### Other tool files\n\n| File | What it is |\n| --- | --- |\n")
    for u, fs in rest: w(f"| `{u}` | {describe(u)} |\n")

# Tests
test_files = [f for f in files if f.startswith("Tests/")]
code_tests = [f for f in test_files if f.endswith((".cpp", ".h", ".py", ".ps1", ".mjs", ".js"))]
data_tests = [f for f in test_files if f not in code_tests]
cm = (ROOT / "CMakeLists.txt").read_text()
w(f"""
## Tests

{len(test_files)} files, {sum(lines_of(f) for f in code_tests if f in F):,} lines of test code. `CMakeLists.txt` has
{len(re.findall(r'add_test\(', cm))} `add_test` calls, several inside helper functions and loops, which register the
150 CTest entries listed in `Tools/Build/ctest-windows-expected.txt`. CI checks that inventory with
`Tools/Build/InvokeCheckedCTest.ps1`.

How tests are built:

- **`ReviaTests`** (CTest `Revia.Foundation`, plus a few single-mode entries) compiles every
  `Tests/*.cpp` that is not given its own executable. `Tests/foundationTests.cpp` is its driver.
- **Focused executables**, mostly through `revia_add_studio_check`, compile one test file with
  `Tests/Fixture/studioPackageTests.cpp` (for example `ReviaProactiveStateTests`,
  `ReviaDelegationTests`, `ReviaLiveCaptureTests`).
- **`ReviaDesktopStopTests`** runs the Qt suites in `Tests/Fixture` (`--suite=policy`, `video-preview`,
  `theme`, `settings`, `shell`...) offscreen.
- **Python, PowerShell and Node** tests run through their interpreters as CTest entries, for
  example `Revia.TrainingWorkerPolicy` and `Revia.BuildDrivers`.
- **`*Live.cpp` and `*.live.*`** need a real model or device and are opt-in, never part of CTest.
- `Tests/reviaSessionTestAccess.h` and `Tests/actionRuntimeTestAccess.h` give tests controlled
  access to private session and action state.

Each description below comes from the file itself: its test function names where it has them,
otherwise its assertion messages. Assertion messages are written as the failure they report
("Read-only worker accepted a write"), so each one names the behaviour the check guards.

| File | Lines | What it checks |
| --- | ---: | --- |
""")
for f in sorted(code_tests):
    w(f"| `{f}` | {lines_of(f):,} | {test_summary(f)} |\n")
dgroups = collections.Counter(str(Path(f).parent) for f in data_tests)
w("\nTest data: " + "; ".join(f"`{d}/` ({n} files)" for d, n in sorted(dgroups.items())) + ".\n")

# Config etc
w("""
## Configuration, assets, build and CI

| Path | What it is |
| --- | --- |
""")
other = [(u, fs) for u, fs in units.items() if not u.startswith(("Tests/", "Tools/", "Desktop/", "docs/")) and u != "setup.bat"
         and not any(f.startswith(("Public/", "Private/")) for f in fs)]
groups = collections.OrderedDict()
for u, fs in other:
    key = u
    for k in D:
        if "*" in k and fnmatch.fnmatch(u, k): key = k; break
    groups.setdefault(key, []).extend(fs)
for k, fs in groups.items():
    w(f"| `{k}` | {D.get(k) or describe(k)}{f' ({len(fs)} files)' if len(fs) > 1 else ''} |\n")
docs = sorted(f for f in files if f.startswith("docs/"))
w(f"\n`docs/` holds {len(docs)} documents: architecture, design notes, acceptance records and progress logs. "
  "Start with [ARCHITECTURE.md](ARCHITECTURE.md), [ROADMAP.md](ROADMAP.md) and "
  "[COMPANION_UPGRADE_PROGRESS.md](COMPANION_UPGRADE_PROGRESS.md).\n")

# Observations
cycles = sorted({tuple(sorted(e)) for e in edges if (e[1], e[0]) in edges})
largest = sorted(((f, lines_of(f)) for f in files if f in F), key=lambda x: -x[1])[:15]
w(f"""
## Measured observations for pass 2

Facts only, gathered while mapping. Pass 2 decides what, if anything, to change.

### Size concentration

| File | Lines |
| --- | ---: |
""")
for f, n in largest: w(f"| `{f}` | {n:,} |\n")
rs = sum(lines_of(f) for f in units['Runtime/reviaSession'])
w(f"""
- `ReviaSession` is {rs:,} lines in its own `.h`/`.cpp` alone, and the same class is also implemented
  in 18 other `Private/Runtime` files (the `session*.cpp` files, the studios, `qualityFeedback`, `conversationSessions`, `idleComputerActivity`, `speakerEnrollmentRuntime`). Runtime totals {size['Runtime']:,} lines.
- `ReviaWindow` (`reviaWindow.h/.cpp`, `windowShell.cpp`, `companionStudio.cpp`, `windowLayout.cpp`) is
  {sum(lines_of(f) for f in files if f in ('Desktop/reviaWindow.cpp', 'Desktop/windowShell.cpp', 'Desktop/companionStudio.cpp', 'Desktop/windowLayout.cpp', 'Desktop/reviaWindow.h')):,} lines.
- `Tests/foundationTests.cpp` is {lines_of('Tests/foundationTests.cpp'):,} lines; tests are {sum(lines_of(f) for f in code_tests if f in F) * 100 // max(1, sum(lines_of(f) for f in files if f in F)):d}% of all code.

### Dependencies between domains

{len(edges)} directed domain-to-domain include relationships among {len(set(out_deps) | set(in_deps))} domains.

| Domain | Depends on (count) | Used by (count) |
| --- | ---: | ---: |
""")
for d in sorted(set(out_deps) | set(in_deps), key=lambda d: (-len(out_deps[d]), d)):
    w(f"| {d} | {len(out_deps[d])} | {len(in_deps[d])} |\n")
w(f"""
- **Runtime** depends on {len(out_deps['Runtime'])} other domains. As the composition root, it is expected to depend on many.
- **Core** is used by {len(in_deps['Core'])} domains but itself depends on {len(out_deps['Core'])}, including
  {', '.join(sorted(x for x in out_deps['Core'] if x in ('Runtime', 'Agents', 'Speech', 'LLM', 'Memory')))}. A foundation
  that includes the layers above it ties the whole build together.
- **Actions** is used by {len(in_deps['Actions'])} domains and depends on {len(out_deps['Actions'])}, including its own executors.
- {len(cycles)} pairs of domains include each other: {', '.join(f'{a}/{b}' for a, b in cycles)}.

### Process and ownership notes

- The intended layering is documented in [ARCHITECTURE.md, Current owners](ARCHITECTURE.md#current-owners),
  including what each module must not own; the include graph above is the measurable side of it.
- `build-fix-check.log` and `.idea/` are tracked, which looks accidental.
""")
text = "".join(md)
if CHECK:
    current = OUT.read_text() if OUT.exists() else ""
    strip = lambda s: re.sub(r"`[0-9a-f]{7,}`", "`<commit>`", s)
    if strip(current) != strip(text):
        print("docs/CODE_MAP.md is stale: run python Tools/CodeMap/build_code_map.py"); sys.exit(1)
    print("docs/CODE_MAP.md is current."); sys.exit(0)
OUT.write_text(text)
print(f"wrote {OUT.relative_to(ROOT)}: {len(text.splitlines())} lines; {sum(1 for u in units if not u.startswith(('Tests/', 'docs/')))} units described; {len(code_tests)} test files")
