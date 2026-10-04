# Revia sidebar navigation

The desktop has six sidebar destinations and no main tab row. Settings stays
at the bottom. Related pages retain their subtabs in the content area. The
existing pages and runtime connections are retained.

| Sidebar destination | Pages |
| --- | --- |
| Chat | Conversation, answer review, voice status and input |
| Workspace | Canvas, Vision |
| Companion | Profiles, Mind, Memory, Voice, Presence, Audience |
| Studio | Agents, Learning, Development |
| Runtime | Activity, Pipelines, Internet, Resources |
| Settings | General, Permissions |

Mind retains its existing Now, Development, Relationships and Drives pages.
There are 21 leaf pages across the complete navigation tree. Runtime retains
its live activity count. Long content uses each page's existing vertical scroll
area; tab captions stay visible.

The selected sidebar destination uses a rounded glass highlight; subtabs use
a smaller underline. The sidebar collapses to an icon rail below 1000 pixels
of window width, and can be collapsed manually in larger windows. Icons retain
accessible names and tooltips. Status cards and controls reflow for the actual
content width after sidebar space is deducted. Profiles stacks its selection,
sampling and voice controls when its panel becomes narrow.

`Desktop/tabNavigation` reparents existing pages once, then reapplies tab bar
settings after companion panels are rebuilt without resetting the selected
main destination. `SelectNavigationPage` selects every ancestor tab so Voice Details
and diagnostic screenshots still reach nested pages. Original screenshot names
such as `Agent Studio` and `Skills & Learning` remain supported through tab
tooltips. Offscreen smoke captures explicitly register the same Windows font
as the existing fixture; native Windows launches use their ordinary font setup.
`Desktop/navigationSidebar` projects the hidden root tab widget instead of
creating another page owner. It mirrors navigation disabling during companion
replacement, updates selection when a deep link opens a page, and refreshes
the Runtime activity count. Collapsing the sidebar asks the window to reflow
its existing controls. The current destination supplies the content title.

An Error status offers **Details**, which opens the existing Runtime Activity
page. **Open folder** there reaches the runtime logs. A GitHub source download
does not install model weights or llama.cpp; run `setup.bat` on the target
computer. The [laptop setup instructions](../README.md#2-run-the-one-command-setup)
describe Minimal setup with Windows voice fallback. The screenshot's generic
error identifies a failed model startup, but the exact cause requires the
target machine's startup messages or worker stderr. This package does not
claim a successful model load on the user's laptop.

## Director status

| Area | Status |
| --- | --- |
| Director / sidebar and presentation | Implemented. Six destinations, Settings footer, collapse control, compact rail and content title. |
| Director / lifecycle connections | Implemented. Existing deep links and companion replacement lock drive the same page stack. |
| Foundation Supervisor / startup diagnosis and source review | No source blocker found. GitHub source omits installed runtimes and model weights; exact laptop startup cause remains unconfirmed. |
| Director / viewport verification | Verified. Twenty-one routes at three window sizes, compact Profiles, Memory rows, accessible rail, live count, collapse/expand and error Details; additional 125% display-scaling fixture passed. |
| Director / repository delivery | Verification complete. Authorized main integration, GitHub push and package branch cleanup are next. |

## Sidebar verification

The final desktop targets built successfully. All 47 registered checks were
verified across the complete registry run and focused reruns on the same frozen
source and binaries. The complete run took 207.18 seconds and passed 46 checks,
including Foundation. Its desktop fixture failed early screenshot saves because
the new capture output directory had not been created before those phases. With
that directory prepared, the unchanged failed fixture passed in 11.68 seconds.
An earlier registry run failed the existing cue fixture's local port-pair setup;
the unchanged Foundation rerun passed in 142.23 seconds. These failed runs remain
in the evidence; this record does not claim a single clean 47-check invocation.

The desktop checks exercise all 21 leaf routes at 760 × 540, 1040 × 720 and
1600 × 1000, with no navigation scroll arrows or horizontal page overflow.
They verify compact and manual sidebar modes, accessible labels, reverse deep
links, live Runtime counts, profile replacement disabling, Error Details and
the original/current Memory rows. The separate fixture at 125% display scaling
also passed. Fresh Qt renders were inspected, and the production executable
captured Chat and both legacy Studio screenshot aliases successfully.

Ten implementation and test source fingerprints and both desktop executable
fingerprints match the final build. Evidence, including initial Profiles
overflow, both registry negatives, corrected renders and independent source
review, is retained in `build/studio-20261004-sidebar`, excluded from Git.
Personality guidance, authored profiles, saved memory, model configuration and
permission policy are unchanged. Laptop model startup remains unverified.

## Previous main-tab consolidation

The initial failing fixture reproduced the old 15-tab root and scroll-arrow
dependence before implementation. The final frozen implementation passed all 47
registered checks in 186.23 seconds, including desktop stop, Studio panels,
SVG rendering and the native Windows desktop smoke check. The additional
125% display-scaling fixture passed in 10.64 seconds. Representative Qt renders
were inspected for readable navigation, status wrapping and compact page layout.
Real executable captures verify complete first-launch captions and both legacy
Studio screenshot names. Earlier clipped and glyph-box diagnostic captures
remain as negative evidence; corrected captures supersede them.

The compact Memory render scrolls explicitly to its table and retains the
existing assertion that both original and corrected rows are visible. No check
was weakened to hide overflow. Negative captures and intermediate failures are
retained with final evidence in `build/studio-20261004-navigation`, excluded from
Git. The package changes desktop presentation and checks only; authored profiles,
personality guidance, model settings, saved memory and permission policy are
unchanged. No model inference or usage reset is needed for these UI checks.

Implementation commit `1fcf0198d9463de38498e1e2bf076a011c46b6f5` was
fast-forwarded into main and pushed. Local main, origin/main and GitHub main
matched before `codex/revia-clean-navigation` was deleted. The four affected
desktop checks passed on merged main in 6.84 seconds. Git checkout converted
line endings; normalized source fingerprints proved all nine implementation
and test files retained exactly the tested content. This delivery record adds
no implementation changes.
