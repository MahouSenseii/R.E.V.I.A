# MCP tools for Revia

Revia calls tools on MCP servers the way she does everything else: as a typed action
(`mcp_tool`) that passes capability policy, confirmation and the audit log. What makes
a server usable is a **manifest** under `Config/Skills/`, and what makes a tool usable
is that the manifest **pins** it.

## Why pinning

A tool's description is prompt text the server sends. A server that changes it after
you trusted it changes what the model is told without anyone agreeing to it (the
"rug pull"). So the manifest records, for every tool, a SHA-256 of the description and
input schema as you saw them, plus the risk you assign. At every connection the live
tool list is compared against the manifest: a tool that is not in the manifest, whose
digest differs, or that the manifest disables is not offered, and the planner only ever
sees the pinned text.

## Setting one up

1. Start the server yourself. Revia never launches a process for this. Most MCP
   servers speak stdio, so run the bridge, which starts each configured server on
   first use and exposes it at `http://127.0.0.1:8760/<id>`:

   ```powershell
   copy Tools\Mcp\bridge.config.example.json Tools\Mcp\bridge.config.json
   node Tools\Mcp\bridge.mjs Tools\Mcp\bridge.config.json
   ```

   `${NAME}` in an arg or env value is read from the bridge's own environment, so a
   token stays out of the file. A server that already speaks Streamable HTTP is named
   directly in its manifest and needs no bridge.

2. In `Config\capabilities.json` set `"mcp": {"enabled": true}`.

3. Write or edit the manifest, `Config\Skills\<id>.json`:

   ```json
   {
     "id": "obs",
     "enabled": true,
     "transport": { "kind": "http", "url": "http://127.0.0.1:8760/obs" },
     "tools": []
   }
   ```

4. In Revia, `/skills pin obs` imports the server's tools as they are now. Every new
   tool is pinned at `destructive` risk, which means it asks for confirmation every
   time; edit `risk` in the manifest to `read_only` or `reversible_write` for the ones
   you have looked at, and `enabled: false` for any you never want offered. `/skills`
   shows what is offered and why the rest is not.

5. Ask her, or `/plan switch OBS to the BRB scene`. The planner sees the pinned
   descriptions, proposes `{"action":"mcp_tool","server":"obs","tool":"...","arguments":{...}}`,
   and the call goes through the same policy every action does.

The three example manifests (OBS, Home Assistant, Playwright) are shipped disabled with
no tools pinned. Home Assistant's own MCP integration speaks the older SSE transport;
the example bridge config reaches it through `mcp-remote`. Playwright belongs in the
VM stage once that exists; until then it runs on your desktop under the same policy as
everything else, so keep its risk at `destructive`.

## What a server cannot do

The client speaks `initialize`, `tools/list` and `tools/call` and nothing else: no
resources, no prompts, no sampling, no elicitation. A server cannot ask Revia for
anything, and what a tool returns is reference data to her, never an instruction.
