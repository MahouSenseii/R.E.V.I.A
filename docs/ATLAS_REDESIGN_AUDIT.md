# Atlas redesign audit — 5 November 2026

This is an isolated, local review change. No push, PR, merge, upload, publication,
model run, training, product build, installation or runtime-data modification was
performed. Revia and RNL remain paused. The existing Pages workflow is unchanged.

## Source audit

- GitHub main was checked at d51a2b38fb87f3d0dd63a5e4fcf2796de219ef4a.
- The original Atlas overview/milestone still described the November 3 / PR #27
  snapshot, despite later source fixes through PR #34.
- The current research page contained summaries through STATE014, while public
  reproducible research source was pinned through STATE010. Summary publication
  did not release the corresponding research evidence.
- The owner stopping handoffs, PAUSE-HANDOFF.md and RNL-AI-HANDOFF.md, were read
  locally. Raw handoffs, logs, model responses, machine paths and manifests were
  not copied into the public Atlas.
- The portfolio source and rendered page informed the cream/navy/blue/gold
  palette, editorial headings, spacing and restrained ornament.

Primary public references:

- [Pinned Revia source](https://github.com/MahouSenseii/R.E.V.I.A/commit/d51a2b38fb87f3d0dd63a5e4fcf2796de219ef4a)
- [PR #34](https://github.com/MahouSenseii/R.E.V.I.A/pull/34) and its linked preceding
  PR #28–33 records, reconciled with the owner source manifest
- [Application CI 41947710858](https://github.com/MahouSenseii/R.E.V.I.A/actions/runs/41947710858)
- [Pinned public research](https://github.com/MahouSenseii/R.E.V.I.A/tree/aac6f161fd080db400317670e4b3a78973dbb105/research/rnl)
- [Portfolio source](https://github.com/MahouSenseii/Portfolio) and
  [rendered reference](https://mahousenseii.github.io/Portfolio/)

## Changes

The existing two pages now lead with plain-language conclusions, current source
identity, scoped checks and open work. The overview includes PR #28–34, 20 source
capability areas, evidence definitions and a copyable AI handoff. The research
page presents local STATE011–016, positive and negative results together,
the incomplete comparison and the public/local reproducibility boundary.

Full historical main content is preserved within marked, closed dated records.
Original fragment identifiers remain valid. The previous milestone object is
preserved completely under history; current state lives in current. Historical
153-registration counts remain explicitly historical.

Left navigation, mobile focus containment, Escape, breakpoint cleanup, search,
empty/reset states, disclosure, original deep links and copy feedback are present.
The HTML remains readable without scripts; native details and a noscript mobile
navigation fallback are provided. No remote fonts or dependencies were added.

Build/current content lives in Tools/Atlas. The public artifact remains exactly
index.html, research.html, atlas.css, public.css, atlas.js and milestone.json.
No publishing permissions or privacy checks were changed.

## Pages diagnosis

[Run 41893563372](https://github.com/MahouSenseii/R.E.V.I.A/actions/runs/41893563372)
had two cancelled deployment attempts. Artifact validation, existing Pages
configuration and upload passed both times. Attempt 1 remained updating_pages;
attempt 2 polled blank status. Each was cancelled at the ten-minute job timeout.
The underlying backend cause is unverified; no website-code defect is established.
The [preceding run 41880016023](https://github.com/MahouSenseii/R.E.V.I.A/actions/runs/41880016023)
succeeded. No retry, setting, permission or timeout change was made.

## Verification

- Four dependency-free Node checks cover source/CI identity, research boundaries,
  public artifact/privacy/symlink/totals guards, IDs and fragment links.
- The stale baseline failed the new current-state/history checks before changes.
- Preservation verification compares the retained HTML to pinned originals
  (normalizing only line endings), and deep-compares the full prior milestone.
- Rebuilding twice produces identical bytes.
- Browser checks used 1280px desktop, 390px phone and 320px narrow-phone widths.
  No horizontal page overflow was observed, including opened historical tables.
- Mobile menu focus wraps, Escape returns focus, navigation closes the drawer,
  and desktop resize clears background inertness after the media event settles.
- Technical detail toggles, no-result/reset behavior and handoff copying passed.
- An independent reviewer found an old-study search omission and imprecise
  "negative control" wording. Both were fixed. The unique STATE006 query was
  observed failing, then returning one exposed record after the fix; clearing
  restored the collapsed state. Original product/research deep links also passed.
- Browser console warning/error queries returned no entries.
- Diff whitespace check passed after normalizing generated HTML line endings.

Local screenshots and the exact final file hashes are kept in the task's
verification directory, outside the repository/public artifact.

## Remaining limits

This redesign is not uploaded or deployed. Pages backend diagnosis remains
unverified. No live-model, installed-app, performance, research reproduction or
application release qualification is added by this work. No claim is made that
the local research evidence bundle is public or ready to publish.

Before publication, review these six exact website files and separately review
any proposed research export inventory. Update local-review labels only when an
actual upload/deployment is authorized and its result is known.
