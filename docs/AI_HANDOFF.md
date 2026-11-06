# Revia: start here

Status checked 5 November 2026. This handoff describes a pinned product-source
snapshot. Recheck GitHub before treating it as the latest head.

The Atlas redesign was merged in
[PR #35](https://github.com/MahouSenseii/R.E.V.I.A/pull/35) at
[ac755b28](https://github.com/MahouSenseii/R.E.V.I.A/commit/ac755b28d635676e398623841b7c4c49eee5416c).
The [live Atlas](https://mahousenseii.github.io/R.E.V.I.A/) was verified after
[Pages run 41961109918](https://github.com/MahouSenseii/R.E.V.I.A/actions/runs/41961109918)
succeeded; all six served files matched the reviewed source. This publication
checkpoint does not update or qualify the installed Revia app or local RNL studies.

Read the [live Atlas](https://mahousenseii.github.io/R.E.V.I.A/),
[RNL research summary](https://mahousenseii.github.io/R.E.V.I.A/research.html)
and [structured snapshot](atlas/milestone.json) first. Closed dated records retain
the full earlier findings and failures; their old "current/next/unrun" language
applies to their own date.

## Verified Revia source

[PR #41](https://github.com/MahouSenseii/R.E.V.I.A/pull/41) is merged at
[a4bd7114c0421e2d3f13aff3bae28cea461fbdd2](https://github.com/MahouSenseii/R.E.V.I.A/commit/a4bd7114c0421e2d3f13aff3bae28cea461fbdd2).
[CI 42135568077](https://github.com/MahouSenseii/R.E.V.I.A/actions/runs/42135568077)
passed six jobs: 155 tests passed, 0 failed, 2 optional video skips.
That result is hosted application-source evidence, not a new local test run.

PR28–34 correct user-owned server context, rejected nicknames, later valid name
corrections, reassurance/hostility, recurrence friction, explicit self-correction
and invented messenger history in the host policy. PR37 grounds casual check-ins
("what are you up to?") and first-time "X, not Y" name statements, and keeps
those replies streaming.

PR37 was measured through `ConversationRuntime::EvaluateTurn` with the SHA-pinned
Qwen3.5-4B on three fixed prompts, four passes per round: 5/12 before the fix,
12/12 after it on CPU, 11/12 on the RTX 5070 (the miss was reply length). Host
repair still removes much of what the model invents, and the gates match phrases.
A post-merge review found those phrase lists matched parts of words ("eating"
inside "creating"), which could drop harmless sentences from these two turn
types. PR39 fixed this by matching whole words and phrases, with a regression
for each reported example.

PR41 came from live multi-turn conversations with the pinned 4B through
`ReviaSession` (opt-in `ReviaConversationTrialLive`, shipped 32K context; below
16K no tools are offered). It fixes a name correction that left later facts
under the rejected name, a returning speaker who was never recognised after a
restart, and a memory-selector rule that filed the speaker's own plans and pets
as unattributed rows no private chat retrieves. It also makes "use your agents"
reach the agents (2/10 to 10/10 in replays), and says so when none can start.
Workers now open the files they were asked about. Each fix has a regression
that failed before it.

The installed app was not updated or freshly audited. Model-generated invented
history remains open: the model accepted a false premise in 3 of 3 live runs.
In six live delegated runs after PR41, five handed off, chat stayed responsive and
the workers read the files; one run passed review and acceptance and was reported
55 s after the hand-off. The rest ended unverified, which is the model's own
judgement. Before PR43 such a run discarded what its workers had found, and the
report either said nothing or invented a bug. PR43 keeps that deliverable
(never accepted or reviewed) and relays it as the worker's own unconfirmed claims.
It keeps one only when it meets the node's deliverable contract, not merely a
valid hash. PR42's push run timed out in the Windows WebDemo entry-point test at
10 s while the PR run of the same commit passed (passes take 2.5-7.5 s); the
limit is now 60 s, and whether that run was slow or stuck is unknown. A skipped
correction ("four, not three"), one misfiled correction, and unrelated memories
passing the 0.35 similarity floor still mislead recall. Restart and exit pause
40–70 s after "Shutdown complete" (pre-existing, not investigated).
Physical desktop, real voice/media and release qualification remain separately
scoped. Historical setup/timing failures and long-path save
limits remain in the dated records.

## RNL evidence boundary

RNL is paused; no rerun, lifecycle fixture, continuation or upload is active.
[Public research source](https://github.com/MahouSenseii/R.E.V.I.A/tree/aac6f161fd080db400317670e4b3a78973dbb105/research/rnl)
is pinned at aac6f161fd080db400317670e4b3a78973dbb105 through STATE-010.
Later Atlas summaries do not release the corresponding source and detailed evidence.

STATE-011–016 are completed local studies. STATE-015's alternative reader scored
963/1008 versus 970/1008 originally. STATE-016's separate slots scored 1008/1008
versus 970/1008 for original/sham, recovering 38 errors without new ones.
These are synthetic four-key/eight-value studies with supplied/parsed keys.
They do not establish general routing, language understanding, conversation
memory, equal-capacity training, an LLM replacement or a Revia improvement.
STATE-017 scaling is proposed only. Candidate for Revia: None. Integrated: None.

COMPARE-001 retained 32 Qwen3.5-0.8B responses, then stopped on
"Owned monitor did not join". RNL and exact lookup never ran. No gold accuracy
was graded. The 34 preserved files are not the planned 46-file seal, so there is
no valid comparative result or ranking, especially of the usual 4B model.
Native processes exited and key removal was checked in the owner handoff.

Detailed local methods, source deltas, deterministic fixtures, seeds, predictions,
traces, frozen snapshots and the partial comparison seal are still needed for
reproduction. Raw local manifests/logs can contain private paths or process data.
Review an exact sanitized inventory, hashes and destination before any upload.
This summary alone cannot reproduce the local studies.

## Safe next actions

1. Inspect source statically and reconcile the current head with the pinned one.
2. Obtain the owner stopping handoffs and original evidence before a new model run.
3. Preserve the 32 saved responses. Do not replay the consumed comparison wrapper.
4. Choose one bounded validation, with fresh accounting and exact execution scope.
5. Review the precise export list/destination before publishing local material.

## Atlas development and deployment

The redesigned site is static, dependency-free and limited to the original six
public files. Build with Node: node Tools/Atlas/build.mjs.
Verify: node --test Tools/Atlas/atlas.test.mjs.
Preview: node Tools/Atlas/serve.mjs, then open its loopback URL.
Stop that owned preview process when finished.

The generator reads curated current data from Tools/Atlas/data.mjs, preserves
the marked dated HTML inside the existing pages, and retains the entire previous
milestone under the JSON history field. Do not remove those record markers.

The original Pages workflow still enforces exactly six files, a total below
500000 bytes, privacy patterns, historical totals and installation qualifiers.
No workflow permissions, timeouts or artifact checks were weakened.

[Verified publication checkpoint: Pages run 41961109918](https://github.com/MahouSenseii/R.E.V.I.A/actions/runs/41961109918)
succeeded on 5 November 2026. The live homepage, research navigation and search
were checked, and all six public files matched the reviewed Git bytes.

Historical failure, 4 November 2026:
[earlier Pages run 41893563372](https://github.com/MahouSenseii/R.E.V.I.A/actions/runs/41893563372):
artifact validated and uploaded; both deployment attempts stalled and cancelled at
the ten-minute timeout. Underlying backend cause is unverified. The
[preceding run 41880016023](https://github.com/MahouSenseii/R.E.V.I.A/actions/runs/41880016023)
succeeded. That failure was not rerun and no settings were changed; the later
deployment for merged PR #35 succeeded. The old backend cause remains unresolved.
