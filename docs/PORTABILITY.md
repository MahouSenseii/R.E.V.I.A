# Portability

Revia targets **Windows 10 1809 or newer, x64**. Within that target it should
build on any developer machine and run on any user machine, including one with
no discrete GPU.

Cross-platform support is out of scope. `CMakeLists.txt` links `sapi`, `dxgi`,
`gdiplus`, `gdi32`, and `shell32`, speech synthesis uses Windows SAPI, and the
action layer uses Windows UI Automation. Porting means replacing those
subsystems, not recompiling.

## The failure this work addressed

On a clean machine the desktop shell silently did not exist. `find_package(Qt6
... QUIET)` failed, the `ReviaDesktop` target was never created, `Build.ps1`
printed only the CLI path, and nothing reported an error. The build "succeeded"
and produced a different program than the one documented.

Three separate causes, all now fixed:

1. Qt was pinned to exactly `%USERPROFILE%\Qt\6.8.3\mingw_64`. A different patch
   version, a different install root, or an MSVC kit all failed silently.
2. Qt was the only heavy dependency with no `Tools\Install*.ps1` script, because
   Qt's official installer requires an account and a GUI.
3. `QUIET` plus a `STATUS` message meant the failure was invisible in normal
   build output.

## Phase 1 — build portability (done)

**`CMakeLists.txt`**

- Qt discovery now resolves in order: `-DREVIA_QT_ROOT=...`, the `REVIA_QT_ROOT`
  environment variable, then the newest `6.*/mingw_64` kit found under
  `%USERPROFILE%\Qt`, `C:\Qt`, or `%QT_ROOT_DIR%`. Version comparison is a
  natural sort, so 6.10 ranks above 6.9.
- `find_package` is no longer `QUIET`. Missing Qt emits a `WARNING` with the
  exact commands to fix it. `-DREVIA_REQUIRE_DESKTOP=ON` promotes that to a
  `FATAL_ERROR` — use it in CI.
- `-DREVIA_BUILD_DESKTOP=OFF` remains the way to opt out deliberately.

**`Tools\InstallQt.ps1` (new)**

Provisions Qt via `aqtinstall`, which pulls the same official packages from
Qt's mirrors without an account. It also installs Qt's own CMake, Ninja, and
MinGW 13.1.0 packages, which removes the project's dependency on CLion's
bundled toolchain.

MinGW 13.1.0 is not arbitrary: it is the compiler the Qt 6.8 `mingw_64` kit was
built with, and it matches the GCC version CLion currently bundles. An MSVC kit
will not link against this build.

**`Tools\Build.ps1`**

- Searches PATH, then a Qt Tools installation, then CLion — instead of assuming
  CLion is present.
- Prints the resolved compiler, CMake, and Ninja paths, because an ABI mismatch
  between the compiler and the Qt kit is otherwise very hard to attribute.
- Warns explicitly when `ReviaDesktop.exe` was not produced.

## Phase 2 — CPU and non-NVIDIA runtime (done)

`InstallLlamaCpp.ps1` and `InstallWhisper.ps1` originally hardcoded CUDA 12.4
builds. On a machine without an NVIDIA GPU that was ~1.25 GB of unusable
download, and a CUDA 12.4 llama.cpp binary is not an appropriate Blackwell
runtime for the target RTX 5070.

Both scripts now share `Tools\ReviaAcceleration.ps1`, which probes the display
adapters and the presence of `vulkan-1.dll`, then resolves a pinned asset. Every
asset is still verified by SHA-256.

| Accelerator | llama.cpp | whisper.cpp | Total |
|---|---|---|---|
| CUDA (NVIDIA) | 239 MB + 373 MB cudart | 640 MB cuBLAS | ~1.25 GB |
| Vulkan (AMD, Intel, NVIDIA) | 33 MB | 20 MB OpenBLAS | ~53 MB |
| CPU | 18 MB | 20 MB OpenBLAS | ~38 MB |

whisper.cpp v1.9.2 publishes no Vulkan build for Windows, so Vulkan machines get
the OpenBLAS CPU build.

Override detection with `-Accelerator cpu|vulkan|cuda`, which is also how you
build a package for a machine other than the one you are on. On CUDA installs,
`InstallLlamaCpp.ps1` additionally selects CUDA 13.3 for RTX 50-series hardware
and CUDA 12.4 for earlier cards; `-CudaRuntime 12.4|13.3` overrides it.

`InstallLlamaCpp.ps1` writes `ThirdParty\llama.cpp\revia-backend.json` recording
which variant is installed, and reinstalls when the accelerator changes rather
than leaving a stale runtime in place.

`InstallWhisper.ps1` aligns `speechRecognition.useGpu` in `Config\settings.json`
with what it installed. The C++ runtime passes `-ng` for CPU or `-dev N` for an
exact planned CUDA device. On a mixed RTX 5070 + RTX 2070 machine, automatic
placement keeps this CUDA 12.4 whisper worker on the 2070.

**The C++ runtime degrades correctly and now understands multiple devices.** It
asks the installed llama.cpp executable for its backend device inventory rather
than treating DXGI's largest display adapter as the whole machine. Exact IDs are
routed to chat/vision, Qwen voice, whisper, and embeddings before their owners
start. If that inventory is unavailable, placement falls back to backend auto/CPU
without inventing a CUDA ordinal. CPU thread shares preserve logical cores for
Windows, and llama/SQLite RAM caches retain configured OS headroom.

Qwen3-TTS uses an isolated PyTorch 2.7.1 + CUDA 12.8 environment so a target
machine containing both Blackwell (RTX 5070) and Turing (RTX 2070) can load the
worker on either card. The service selects BF16 only on hardware that reports
support and uses FP16 on Turing. This is pipeline isolation, not model-parallel
speech: one conversational utterance remains on one resident voice worker.

## Phase 3 — redistributable package (not started)

Remaining work to make a double-clickable build:

- **`ReviaDesktop` now links `-static-libgcc -static-libstdc++`.** Before this it
  depended on `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, and `libwinpthread-1.dll`
  being present, which is only true on a machine with a toolchain installed. A
  shared Qt build cannot be fully statically linked, so `windeployqt` still
  supplies the Qt DLLs. *If the `--whole-archive -lwinpthread` link option causes
  a duplicate-symbol failure, drop that line and let `windeployqt` copy
  `libwinpthread-1.dll` instead.*
- Verify `windeployqt` output on a machine with no Qt and no toolchain. This is
  the only reliable test; a dev box has the DLLs on PATH and will mask a gap.
- Add `Tools\Package.ps1` producing a versioned zip of the executable, deployed
  Qt runtime, `Config\`, and `Tools\`, excluding `Models\` and `ThirdParty\`.
- First-run flow: detect missing models and offer to run the installers, rather
  than failing on a missing `modelPath`.
- Qt open source is LGPLv3. Dynamic linking as done here is compatible with
  redistribution, but the obligations are worth reading before shipping binaries.
  This is not legal advice.

## Known limits on a low-end machine

These are honest constraints, not bugs:

- **The chat model is the real floor.** The configured Qwen3-VL 8B Q4_K_M is
  roughly 5 GB. On CPU it will generate at a few tokens per second. A 3B or 4B
  Q4 model is the practical choice for CPU-only, set via `llm.modelPath`.
- **Vision on CPU is slow enough to feel broken.** Consider defaulting
  `vision.enabled` to false when the CPU runtime is installed.
- **Qwen3-TTS on CPU is not usable in real time.** `speech.qwenDevice` already
  falls back to CPU below `qwenMinimumFreeVramMiB`, and synthesis failures fall
  back to Windows SAPI. On a CPU-only machine, do not assign a Qwen voice; SAPI
  is the zero-setup path and needs no download.
- **`InstallQwenTTS.ps1` pulls PyTorch**, which is a multi-GB download that a
  CPU-only install does not benefit from. Skip it unless voice cloning is wanted.

## Verifying a change to any of this

Run on a machine, or VM, with **no Qt, no CLion, and no NVIDIA GPU**:

```powershell
.\Tools\InstallQt.ps1
.\Tools\InstallLlamaCpp.ps1        # should select cpu or vulkan
.\Tools\InstallWhisper.ps1
.\Tools\DownloadEmbeddingModel.ps1
Remove-Item -Recurse -Force .\build\debug
.\Tools\Build.ps1
```

Expected: configure prints `Revia: using Qt kit at ...`, no Qt warning appears,
`ReviaDesktop.exe` is produced, `Revia.Foundation` and `Revia.DesktopSmoke` both
pass, and the app launches with chat on CPU.
