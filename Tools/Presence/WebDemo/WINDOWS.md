# Windows setup and Render activation

The implementation is already in this directory. Deployment still needs your
Render service, three independently created secrets, and your running PC/model.
The production Portfolio origin is exactly `https://mahousenseii.github.io`.
Never append `/Portfolio/` to a CORS origin.

## 1. Build and install

From the R.E.V.I.A repository root, with the existing native toolchain and Node 24:

```powershell
.\Tools\Build.ps1
npm --prefix .\Tools\Presence\WebDemo ci --omit=dev --ignore-scripts
```

The application is `build\debug\ReviaDesktop.exe`, not a target named `Revia`.
Use `-Executable` on the helper for another freshly built desktop executable.
The normal desktop startup loads its configured local model; the helper does not
change model configuration. If an existing desktop is running without the web
environment, close it normally and restart with the helper. It cannot change a
running process's environment.

## 2. Prepare your private local files

Prefer a private directory outside the repository and OneDrive. The build copies
the Tools directory to build output, so secrets placed inside Tools could also be
copied there, despite Git ignoring them. The following creates only a directory
and copies blank examples; run the copy commands once, before filling them in:

```powershell
$webConfig = Join-Path $env:LOCALAPPDATA 'Revia\WebDemo'
New-Item -ItemType Directory -Force -Path $webConfig | Out-Null
Copy-Item .\Tools\Presence\WebDemo\.env.revia.example (Join-Path $webConfig '.env.revia')
Copy-Item .\Tools\Presence\WebDemo\.env.bridge.example (Join-Path $webConfig '.env.bridge')
```

Restrict access to your Windows account. Fill these files using a local editor;
do not paste secrets into chat, command arguments, GitHub, or screenshots.
The helper parses values as data, never as PowerShell code. File values are
authoritative; inherited `NODE_ENV` and web settings cannot override them.

Create these **yourself**, using your password manager's random generator:

| Secret | Where the original goes | Where its SHA-256 digest goes |
|---|---|---|
| Independent host bearer, e.g. 64 random hexadecimal characters | `.env.bridge`: `REVIA_WEB_HOST_TOKEN` | Render: `REVIA_WEB_HOST_TOKEN_SHA256` |
| Independent local bearer, e.g. 64 random hexadecimal characters | `.env.revia`: `REVIA_WEB_LOCAL_TOKEN` | Nowhere |
| Independent invite code, e.g. 64 random hexadecimal characters | Your password manager; give privately to invited guests | Render: `REVIA_WEB_INVITE_SHA256` |

Host and local secrets must be different, printable ASCII without spaces, 32–256
characters. High entropy matters for the invite too. Hash the exact UTF-8 value,
without a newline, using a trusted local utility or password manager. Never use
an online hashing website. This PowerShell snippet prompts privately and outputs
only the lowercase digest; repeat separately for host bearer and invite:

```powershell
$secretInput = Read-Host 'Secret to hash' -AsSecureString
$secretBytes = [Text.Encoding]::UTF8.GetBytes(([Net.NetworkCredential]::new('', $secretInput)).Password)
$sha = [Security.Cryptography.SHA256]::Create()
try { ([BitConverter]::ToString($sha.ComputeHash($secretBytes))).Replace('-', '').ToLowerInvariant() }
finally { [Array]::Clear($secretBytes, 0, $secretBytes.Length); $sha.Dispose(); $secretInput.Dispose() }
```

The helper supplies the same local token and port from `.env.revia` to the bridge.
Do not duplicate them in `.env.bridge`; if you do, disagreement fails validation.
Nothing here generates production credentials.

## 3. Render settings

Create **one Node Web Service** from `MahouSenseii/R.E.V.I.A`, using the branch
containing these changes after you publish it. Local edits alone do not update
Render. Use these settings:

| Field | Value |
|---|---|
| Root Directory | `Tools/Presence/WebDemo` |
| Build Command | `npm ci --omit=dev --ignore-scripts` |
| Start Command | `npm start` |
| Health Check Path | `/healthz` |
| Instances | One; no autoscaling |
| Automatic deploys | Off initially |
| `NODE_VERSION` | `24.21.0` |
| `NODE_ENV` | `production` |
| `REVIA_WEB_BIND` | `0.0.0.0` |
| `REVIA_WEB_HOST_ID` | `revia-public` |
| `REVIA_WEB_ORIGINS` | `https://mahousenseii.github.io` |
| `REVIA_WEB_HOST_TOKEN_SHA256` | Your host bearer's lowercase SHA-256 digest |
| `REVIA_WEB_INVITE_SHA256` | Your invite's lowercase SHA-256 digest |
| `PORT` | Let Render supply it |
| `REVIA_WEB_TRUSTED_PROXY_CIDRS` | Unset until Render verifies actual ingress peers |

The same recipe is in `render.yaml`. For a Blueprint, specify its repository path
`Tools/Presence/WebDemo/render.yaml`; Render defaults to a root-level file.
Choose the hosting plan yourself; the Blueprint's free plan can sleep and has
quotas. `/healthz` measures relay process health, not model readiness.

Copy the actual HTTPS URL assigned by Render. In `.env.bridge`, set
`REVIA_WEB_RELAY_URL` to the same host with `wss://` and `/v1/host` appended.
Do not use a guessed host. Never send the local bearer or the raw invite to Render.

Until proxy trust is verified, clients behind Render ingress can share one
conservative rate bucket. Leave that safe default intact. Do not guess private
CIDRs, use Render outbound ranges, or trust arbitrary forwarding headers. See the
README's proxy section for the existing release requirement.

## 4. One-command local operation

From the repository root, after the two files are complete:

```powershell
$webConfig = Join-Path $env:LOCALAPPDATA 'Revia\WebDemo'
.\Tools\Presence\WebDemo\WebDemo.ps1 check -ConfigDirectory $webConfig
.\Tools\Presence\WebDemo\WebDemo.ps1 start -ConfigDirectory $webConfig
```

`check` makes no connections. `start` launches Revia with web guest enabled,
waits up to three minutes for `online`/`busy`, then runs the outbound bridge in
the current terminal. It refuses to launch onto an occupied native port. Keep
this terminal open. Ctrl+C stops/cleans up the bridge; Revia stays open for your
normal desktop use. Close it normally afterward. If readiness times out, inspect
the desktop's model status; the helper leaves the desktop running for diagnosis.

Separate actions are available:

```powershell
.\Tools\Presence\WebDemo\WebDemo.ps1 revia -ConfigDirectory $webConfig
.\Tools\Presence\WebDemo\WebDemo.ps1 bridge -ConfigDirectory $webConfig
```

In a second terminal, check both ends:

```powershell
$webConfig = Join-Path $env:LOCALAPPDATA 'Revia\WebDemo'
.\Tools\Presence\WebDemo\WebDemo.ps1 status-local -ConfigDirectory $webConfig
.\Tools\Presence\WebDemo\WebDemo.ps1 status-relay -ConfigDirectory $webConfig
```

Local status calls `http://127.0.0.1:17864/web/v1/status` with its local bearer.
Relay status calls the actual HTTPS `/v1/status` without a credential. Both reject
redirects and malformed responses. `online` means ready; an active owner can
cause `busy`. A status command returning successfully is not proof of inference.

If script execution is blocked by your Windows policy, use the equivalent Node
command; no execution-policy change is required:

```powershell
node .\Tools\Presence\WebDemo\operator\main.js start --config-dir $webConfig
```

For manual bridge startup with Node's environment-file loader (default port):

```powershell
node --env-file="$webConfig\.env.revia" --env-file="$webConfig\.env.bridge" .\Tools\Presence\WebDemo\bridge\main.js
```

For a nondefault native port, use the helper or explicitly set the matching
`REVIA_WEB_LOCAL_URL` in the bridge file. Neither method opens a home-router port.

## 5. Real deployed test, while Portfolio stays disabled

Keep `Portfolio/data/revia-demo.json` unchanged until this passes. Build/install
Portfolio's test dependencies if needed (`npm ci`, `npm run build`, and
`npx playwright install chromium` from Portfolio). Start the actual desktop and
bridge above, with the existing real local model ready. Then from R.E.V.I.A:

```powershell
$env:REVIA_WEB_CONFIG_DIR = Join-Path $env:LOCALAPPDATA 'Revia\WebDemo'
$env:REVIA_WEB_PORTFOLIO_ROOT = (Resolve-Path ..\Portfolio).Path
$inviteInput = Read-Host 'Private invitation code' -AsSecureString
$env:REVIA_WEB_TEST_INVITE = ([Net.NetworkCredential]::new('', $inviteInput)).Password
try { npm --prefix .\Tools\Presence\WebDemo run test:deployed }
finally { Remove-Item Env:REVIA_WEB_TEST_INVITE; $inviteInput.Dispose() }
```

This opens the **published HTTPS Portfolio** in Chromium with two independent
guest contexts. Only each browser's `data/revia-demo.json` is overridden to point
at your real relay. All API requests, CORS enforcement, TLS, session admission,
message replies and session deletion go through the deployed backend. There is
no API interception, localhost rewrite, fake model, or disabled TLS check.
No credentials or transcripts are printed. This test does not publish anything.

Then check cancellation during a longer generation, owner pause/resume, owner
activity preemption, wrong invite rejection, and bridge disconnect/reconnect with
fresh sessions. Run two simultaneous independent browser sessions and verify no
cross-session access/history. Verify ingress source limits with Render's actual
trust contract; the automated local privacy suite remains the deeper isolation
test. Native owner controls are documented in `docs/WEB_DEMO.md` and are never
forwarded by the bridge. Stop the connector before closing Revia.

Only after the real test and deployment checks pass, set the Portfolio file to
`enabled: true` and set `relayUrl` to the **actual HTTPS relay origin**, without
`/v1/host`, path, query, or fragment. Build/test and publish Portfolio by its
existing Pages workflow, then repeat a normal browser conversation with no config
override. If the real URL is absent or any gate fails, keep:

```json
{ "enabled": false, "relayUrl": "" }
```

## Verification commands

```powershell
cmake --build --preset debug --parallel 2
ctest --preset debug --output-on-failure --timeout 600
npm --prefix .\Tools\Presence\WebDemo test
$env:REVIA_WEB_NATIVE_HOST = (Resolve-Path .\build\debug\ReviaWebGuestHost.exe).Path
npm --prefix .\Tools\Presence\WebDemo run test:native
# Optional real-model LOCAL smoke; this does not prove hosted TLS/WSS:
$env:REVIA_WEB_PORTFOLIO_ROOT = (Resolve-Path ..\Portfolio).Path
$env:REVIA_WEB_REAL_MODEL = (Resolve-Path Models\Qwen3.5-4B-Q4_K_M.gguf).Path
npm --prefix .\Tools\Presence\WebDemo run test:live
Push-Location ..\Portfolio
try { npm test; npm run build; npm run test:browser }
finally { Pop-Location }
```
