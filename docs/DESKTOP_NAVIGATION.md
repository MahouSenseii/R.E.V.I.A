# Revia desktop navigation

The desktop has six main tabs. Related pages sit underneath their main tab so
navigation fits without left/right scroll arrows. The existing pages and their
runtime connections are retained.

| Main tab | Pages |
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

Main tabs use rounded selected glass cards; subtabs use a smaller underline.
The shell is slightly more opaque to reduce distracting background bleed, and
inactive navigation and explanatory text use brighter colors. The compact
status row still wraps to two rows. Header status explanations can wrap beyond
the previous fixed height.

`Desktop/tabNavigation` reparents existing pages once, then reapplies tab bar
settings after companion panels are rebuilt without resetting the selected
main tab. `SelectNavigationPage` selects every ancestor tab so Voice Details
and diagnostic screenshots still reach nested pages. Original screenshot names
such as `Agent Studio` and `Skills & Learning` remain supported through tab
tooltips. Offscreen smoke captures explicitly register the same Windows font
as the existing fixture; native Windows launches use their ordinary font setup.
The responsive layout reapplies explicit primary-tab padding after grouping so
first-launch captions use the new selector's metrics. This follows Qt's
[stylesheet recomputation guidance](https://doc.qt.io/qt-6.8/stylesheet-syntax.html).

## Director status

| Area | Status |
| --- | --- |
| Director / navigation and presentation | Complete. Six main tabs, grouped existing pages, arrow-free bars and improved text contrast. |
| Director / lifecycle connections | Complete. Initial, failure and rebuilt Voice routes select nested pages; companion rebuilds restore bar settings. |
| Foundation Supervisor / independent source review | Complete. Lifecycle and diagnostic-font follow-ups reviewed with no remaining blocker. |
| Director / viewport verification | Complete. All 21 leaf routes at 760×540, 1040×720 and 1600×1000; compact Memory retains both required rows. The same fixture passed at 125% display scaling. |
| Director / repository delivery | Complete. Verified implementation merged to main, pushed to GitHub, remote identity confirmed and the merged package branch removed. |

## Verification evidence

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
