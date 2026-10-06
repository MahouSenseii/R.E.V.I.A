# Using the continuity and capability upgrades

The existing companion remains the speaker. These changes connect its context,
memory, art and admitted tools; they do not replace the authored personality.
The implementation plan and verification ledger are in
[capability integration](superpowers/plans/2026-10-06-capability-integration.md).

## Conversation and memory

Introduce yourself in a private typed conversation, for example “My name is
Davis.” Revia can then restore compatible private conversation history and
retrieve facts attributed to that participant. Changing the participant or
audience clears private working continuity. Older unattributed stored facts
remain visible in the owner's Memory view; the migration does not guess their
owner or automatically disclose them in a new person's conversation.

Continuity keeps bounded recent turns plus source-linked decisions, corrections,
constraints and unfinished work. Context diagnostics distinguish backend token
counts from the conservative fallback and report when fitting removed history.
This is still finite context, and retrieval does not guarantee that every past
detail will be recalled. Model context remains at its configured size until
larger contexts have demonstrated suitable quality and resource use.

## Programs, files and interactive browsing

In Permissions, the Commands card controls approved executable paths and whether
they may run inside delegated tasks. Enter absolute executable paths, one per
line, and apply the settings. Keep working files within the approved roots.
Command interpreters have their own toggle. A working directory limits the tool
request; it cannot isolate an approved executable from the rest of Windows.

The typed process tool returns stdout, stderr, exit status and timeout/cancel
state with bounded output. File writes require the expected prior content
identity, which refuses changes when another writer changed the file. Stop
terminates the owned process tree. A failed or uncertain effect is not replayed
automatically as if it were harmless.
Guarded writes hold the existing file exclusively, but are not atomic replacement:
a disk/write failure can leave a partial update. Inspect that result before recovery.

The Interactive browser card is separate from Internet & Research. Enable
navigation and, if needed, interaction, then add exact origins such as
`https://example.com` and apply. Paths and wildcard hosts are not origins.
The local-test option explicitly permits loopback HTTP fixtures. Task interaction
must also be enabled before a delegated task can click or fill without a separate
step confirmation. Existing authority restrictions still apply.

Interactive browsing uses an owned Edge profile. Revia sees bounded page text
and current element IDs, rather than arbitrary page scripts. A stale target must
be observed again. Audience changes, disabled execution, permission edits and
task completion close this browser. Read-only research keeps its existing worker.

Use the existing `/operate <task>` entry point for bounded computer tasks.
Recovery can wait, observe again or request vision. Final success requires an
independent criterion: the initial native acceptance implementation supports
placing exact supplied content in the requested application, or reading it back
from a unique named browser field on the originally requested origin. A browser
draft does not prove that it was submitted. More general goals
may execute useful verified steps yet remain unresolved at final acceptance.
Inspect the result and evidence instead of treating an unresolved goal as success.

## Agent workers

Agent Studio offers the normal local model, a local model with permitted tools,
and the deterministic diagnostic. Tool-enabled workers use the existing graph,
reviewer and parent acceptance. Their tools are bounded file reads/listings,
guarded writes and explicitly permitted process execution. They do not receive a
second desktop driver. Calls, tool steps and output consume cumulative workflow
budgets, including across saved retries. An execution receipt is evidence for
review; it does not make a weak model's reasoning correct.
If an interrupted worker might already have performed a tool effect, inspect the
actual result and supply fresh recovery evidence through Retry. Resume does not
silently repeat that uncertain effect.

## Art

Ask for a picture or illustration in ordinary chat, or use `/imagine <prompt>`.
Diagram requests stay with the structured diagram renderer. A successful image
includes a verified raster artifact displayed through Canvas. The local image
runtime must be installed on each machine that performs generation; downloading
the source repository alone does not install model weights.

See [image setup and measured results](IMAGE_GENERATION.md) for installation,
provider settings, memory use and quality limits. SD-Turbo is the small baseline;
SDXL-Turbo is an optional tested alternative, not a guarantee of faithful
character art or editing.

## Guest environments and games

[Guest preparation](GUEST_ENVIRONMENT.md) provides a readiness doctor and a
reviewable Windows Sandbox configuration with separate input and artifact
mounts. The development PC needs the Windows Sandbox feature installed before
live guest testing. Preparing a configuration does not establish guest desktop
control, reconnect/reset, GPU latency or competent game play. Those remain
separate delivery and qualification work.
