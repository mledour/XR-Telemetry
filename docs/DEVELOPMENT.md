# Development

Notes for developers building, testing, or contributing to this layer.
End-user install and configuration live in the [README](../README.md);
this file documents the build system, CI pipeline, snapshot tests, and
signing flow — most of which is inherited from the upstream template
(see "Relationship to upstream" below).

## Prerequisites

- **Visual Studio 2019 or newer** (MSBuild + NuGet integration)
- **Python 3** in `PATH` — required by
  `openxr-api-layer/framework/dispatch_generator.py` at pre-build time
- The `external/` git submodules populated:
  ```
  git submodule update --init --recursive
  ```
- **Inno Setup 6** — only needed if you want to build the `Setup.exe`
  installer locally. CI installs it automatically.

## Where things live

```
openxr-api-layer/
  framework/                        ← mbucchia's framework (do not edit
    dispatch_generator.py             unless you know why)
    dispatch.gen.{h,cpp}              ← regenerated from layer_apis.py
    entry.cpp                         ← DLL entry point + loader negotiation
    log.{h,cpp}                       ← file + ETW logging helpers
  utils/
    overlay_renderer.{h,cpp}        ← in-headset HUD (GPU shaders + glyph atlas)
    overlay_aggregator.{h,cpp}      ← per-frame metric accumulator
    histogram_ring.h                ← ring buffer for frametime samples
    gpu_telemetry.{h,cpp}           ← NvAPI / DXGI VRAM polling
  fonts/                            ← bundled font collection (see bundled_fonts.rc.inc)
  layer.cpp / layer.h               ← YOUR layer logic
  pch.h                             ← D3D11 + D3D12 includes (delay-loaded)
  openxr-api-layer.rc.in            ← VERSIONINFO + RT_BUNDLED_FONT template
  module.def                        ← DLL export list
  *.json                            ← OpenXR loader manifests

openxr-api-layer-tests/
  main.cpp                          ← doctest driver
  mock_runtime.{h,cpp}              ← in-process OpenXR fake
  test_stubs.cpp                    ← entry.cpp substitutes for the test build
  test_*.cpp                        ← YOUR tests
  test_overlay_snapshot.cpp         ← visual-regression vs screenshots/
  openxr-api-layer-tests.rc         ← duplicate RT_BUNDLED_FONT for the test
                                       EXE (see "Snapshot tests" below)

screenshots/
  overlay_snapshot.png              ← golden image diffed by the snapshot test

scripts/
  Generate-VersionRc.ps1            ← bakes git tag into VERSIONINFO
  Sign-Artifact.ps1                 ← headless Certum signing (CI)
  Get-CertumTotp.ps1                ← RFC 6238 TOTP generator
  Test-CertumTotp.ps1               ← offline TOTP self-test
  Install-Layer.ps1                 ← HKLM register (manual install)
  Uninstall-Layer.ps1               ← HKLM unregister
  Tracing.wprp                      ← WPR profile for ETW capture
```

## Relationship to upstream

Three repos in the lineage:

1. **[`mbucchia/OpenXR-Layer-Template`](https://github.com/mbucchia/OpenXR-Layer-Template)**
   — the original framework. Owns everything under
   `openxr-api-layer/framework/`, the dispatch generator, the entry
   point, the logging helpers, and the basic vcxproj structure.
2. **`mledour/OpenXR-Layer-Template`** — adds the CI pipeline, code
   signing, Inno Setup installer, doctest + mock runtime, D3D12
   support in `pch.h`, and the post-clone init script on top of
   mbucchia's framework. Framework code is untouched.
3. **This repo (`XR-Telemetry`)** — the actual layer,
   spun off from (2) via the GitHub "Use this template" button. Adds
   the layer-specific logic in `layer.cpp` / `utils/*` and tests in
   `openxr-api-layer-tests/test_*.cpp`.

Framework + tooling changes from (1)/(2) land here via a manual sync
(rare). Day-to-day work on this layer doesn't track those — the
template is the stable base.

## Building from source

```powershell
git submodule update --init --recursive
```

Then open `XR_APILAYER_MLEDOUR_xr_telemetry.sln` in Visual Studio and
build `Release|x64`. The pre-build event chain:

1. `framework/dispatch_generator.py` reads `framework/layer_apis.py`
   and regenerates `dispatch.gen.{h,cpp}` with one virtual method per
   function listed in `override_functions`.
2. `scripts/Generate-VersionRc.ps1` reads the current git tag (or
   `git describe` output for non-tag builds), then substitutes the
   version into `openxr-api-layer.rc` and `version.h` from their
   `.in` templates.

A post-build event runs `scripts\sed.exe` to substitute
`$(SolutionName)` into the loader's JSON manifest, so renaming the
`.sln` automatically renames the manifest too — useful if you ever
need to rename the layer.

The test binary (`openxr-api-layer-tests.exe`) builds alongside the
DLL and runs your `test_*.cpp` files via doctest. A non-zero exit
fails the CI job.

## Graphics API support in pch.h

`pch.h` pulls in headers for D3D11, D3D12, and D3D11On12 — types only,
no symbol references — and the vcxproj delay-loads `d3d11.dll` and
`d3dcompiler_47.dll`. The net effect:

| Your layer's graphics needs | What's loaded into the host process |
|---|---|
| None (pure metadata-only layer) | Nothing — D3D DLLs stay unloaded |
| D3D11 only | `d3d11.dll` loaded the first time you call into it |
| D3D12 (via D3D11On12 bridge) | `d3d11.dll` only (D3D11On12CreateDevice is exported by `d3d11.dll`) |

The D3D11On12 bridge is the recommended path for D3D12 hosts: it lets
your upload code stay written in plain D3D11 while talking to a
D3D12 swapchain. Pattern:

```cpp
// Detect the host's graphics binding.
const auto* d3d12Binding = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(
    findInNextChain(sessionCreateInfo->next, XR_TYPE_GRAPHICS_BINDING_D3D12_KHR));
if (d3d12Binding) {
    // Wrap the D3D12 device in a D3D11 facade.
    ID3D12CommandQueue* queues[] = { d3d12Binding->queue };
    HRESULT hr = D3D11On12CreateDevice(
        d3d12Binding->device, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, reinterpret_cast<IUnknown* const*>(queues), 1, 0,
        &d3d11Device, &d3d11Context, nullptr);
    // ... QueryInterface for ID3D11On12Device, use CreateWrappedResource
    //     on swapchain images, AcquireWrappedResources / Copy / Release,
    //     context->Flush() before xrReleaseSwapchainImage ...
}
```

If a D3D11On12 path is overkill for your layer, drop the `d3d12.h`
and `d3d11on12.h` includes from `pch.h` — they're optional weight on
build time only.

## Snapshot (visual-regression) tests

### What it does and why

`test_overlay_snapshot.cpp` renders the in-headset HUD with hard-coded
mock telemetry into an offscreen D3D11 texture (WARP) via
`renderOverlayToTextureD3D11` — the exact same GPU pipeline (chrome
shapes + glyph atlas + instanced bars) the in-headset D3D11 path uses —
reads the pixels back, encodes them to PNG, and diffs the result
pixel-for-pixel against the golden image checked in at
`screenshots/overlay_snapshot.png`.

This catches things that ordinary unit tests can't: layout shifts of
a few pixels, palette regressions, font substitution, mojibake (a
literal `°` byte gets corrupted to `Â°` when MSVC reads a UTF-8
source file as CP1252), accidental edits to gradients or stroke
widths, off-by-one in any of the glyph, grid, or bar instances the
renderer emits. A passing CI
run is a per-commit guarantee that the HUD still looks exactly the
way the golden looks.

The rendering is deterministic across CI runners because:

- The HUD is rendered on a **WARP** (software-rasteriser) D3D11
  device — no physical GPU, no driver differences run-to-run.
- The mock snapshot in `makeMockSnapshot()` is a hard-coded `OverlaySnapshot`
  struct — no clock reads, no system queries.
- The bundled font collection (see
  [`openxr-api-layer/fonts/bundled_fonts.rc.inc`](../openxr-api-layer/fonts/bundled_fonts.rc.inc))
  is embedded in the test EXE so DirectWrite never falls back to
  whatever system font happens to be installed.
- Both halves of the comparison go through the same PNG
  encode + un-premultiply + decode pipeline (the fresh PNG is written
  to disk, then re-decoded), so PBGRA→BGRA precision noise cancels out.

### How a CI run plays out

1. MSBuild compiles `openxr-api-layer-tests.rc` into the test EXE
   alongside the C++ TUs. rc.exe bakes the bundled TTF bytes into
   the EXE's resource table (custom type `RT_BUNDLED_FONT` == 256,
   declared in `bundled_fonts.rc.inc`).
2. The PowerShell test step runs `openxr-api-layer-tests.exe`. The
   snapshot test creates a WARP D3D11 BGRA8 render target sized to the
   overlay texture, calls `renderOverlayToTextureD3D11(...)`, reads the
   pixels back, and encodes `overlay_snapshot.png` in the runner's CWD.
3. The test then decodes both the golden (`screenshots/overlay_snapshot.png`,
   path resolved from `__FILE__` so it's CWD-independent) and the
   fresh PNG, and compares the BGRA byte buffers.
4. `CHECK(diff.differingPixels == 0)` — pass or fail.

There is no artifact upload step: the golden is the source of
truth in the repo, and a passing test is the confirmation it still
matches. If the test fails locally, the fresh PNG is on disk for
inspection; if it fails in CI, the assertion message includes the
count of differing pixels and the `(x, y)` of the first one.

### What lives where

| File | Role |
|---|---|
| `openxr-api-layer-tests/test_overlay_snapshot.cpp` | The TEST_CASE, mock data, WIC encode + decode + diff helpers |
| `openxr-api-layer-tests/openxr-api-layer-tests.rc` | Resource script that embeds the bundled TTFs into the test EXE |
| `openxr-api-layer/openxr-api-layer.rc.in` | Production-DLL counterpart — same TTFs, same IDs, same custom type |
| `openxr-api-layer/fonts/*.ttf` | The bundled font files — declared in `bundled_fonts.rc.inc`, referenced from both `.rc` files |
| `screenshots/overlay_snapshot.png` | The golden image diffed by the snapshot test |
| `openxr-api-layer/utils/overlay_renderer.cpp` | `renderOverlayToTextureD3D11()` — the shared entry point used by both the in-headset path and the snapshot test |

### Adding a new resource (font, image, …) used by the renderer

The renderer loads resources via:

```cpp
HMODULE hMod = nullptr;
GetModuleHandleExW(
    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCWSTR>(&pickSwapchainFormat),  // any in-DLL function
    &hMod);
HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(200),
                            MAKEINTRESOURCEW(/*RT_BUNDLED_FONT*/256));
```

`pickSwapchainFormat` is defined in `overlay_renderer.cpp`. When the
production DLL is loaded, that function lives in the DLL, so
`GetModuleHandleEx` returns the DLL's `HMODULE` and `FindResource`
hits the DLL's resource table. **When the test EXE runs**, the same
`overlay_renderer.cpp` is statically linked into the EXE — so the
same `GetModuleHandleEx` call returns the **test EXE's** `HMODULE`
and `FindResource` hits the **test EXE's** resource table.

Net consequence: any resource the renderer wants to read at runtime
must be present at the SAME custom type + ID in BOTH binaries'
resource tables. If the test EXE's table is missing an entry the
DLL's table has, `FindResource` returns null in the test, the
renderer silently falls back to its degraded path (e.g. system
Bahnschrift instead of bundled Barlow for fonts), and the
snapshot test compares against a render that doesn't match
production — defeating the visual-regression contract.

To make this drift impossible by construction, the project keeps
the resource declarations in a **single shared include**:
`openxr-api-layer/fonts/bundled_fonts.rc.inc`. Both
`openxr-api-layer/openxr-api-layer.rc.in` (layer DLL) and
`openxr-api-layer-tests/openxr-api-layer-tests.rc` (test EXE)
`#include` it. The TTF basenames inside the include are bare —
each project's vcxproj sets a `<ResourceCompile>`
`<AdditionalIncludeDirectories>` pointing at
`$(SolutionDir)openxr-api-layer\fonts` so rc.exe's `/I`
search-path resolves the basenames to the same files on disk
regardless of which binary is being built.

(An earlier attempt used a `FONTS_PREFIX` macro defined to a
per-project relative path and counted on the rc preprocessor
concatenating adjacent string literals the way the C compiler
does — it doesn't, RC2135 file-not-found at build time. The `/I`
mechanism is what rc.exe actually documents.)

Both sides compile down to byte-for-byte identical resource
tables. Adding a font / changing an ID / removing a resource is
now a one-file edit — there's no longer a "second place" where
someone could forget.

Checklist for adding a new bundled resource:

1. **Place the file** under `openxr-api-layer/fonts/` (or another
   folder, IF you also add that folder to both vcxprojs'
   `<ResourceCompile><AdditionalIncludeDirectories>` so rc.exe's
   `/I` finds it).
2. **Pick a custom resource type** if you don't already have one.
   Numeric IDs ≥ 256 are safe (Windows reserves 1–255 for the
   predefined types like `RT_BITMAP`, `RT_ICON`, etc.). Reuse
   `RT_BUNDLED_FONT (256)` if it's another font.
3. **Pick a resource ID** ≥ 200 (this codebase's convention; under
   200 is reserved for IDs the DLL's `.rc.in` template might add for
   icons, dialogs, etc.).
4. **Declare in the shared include**
   `openxr-api-layer/fonts/bundled_fonts.rc.inc` — single source of
   truth, no other `.rc` file should grow an inline declaration:
   ```rc
   #define IDR_MY_NEW_RESOURCE 202
   IDR_MY_NEW_RESOURCE RT_BUNDLED_FONT "MyFile.ttf"
   ```
   The filename argument is a **bare basename** — no `fonts\\`
   prefix, no `..\\openxr-api-layer\\fonts\\` prefix. rc.exe finds
   the actual file on disk via the project's
   `<ResourceCompile><AdditionalIncludeDirectories>` `/I` entry.
5. **Add the file to both vcxprojs as `<None Include>`** so Solution
   Explorer shows it and the build triggers when it changes. The
   `<ResourceCompile>` entries that pull in the `.rc` files are
   already there — no new entry needed.
6. **Re-render and refresh the golden**: the snapshot will now use
   your new resource. See "Regenerating the golden" below — either
   the `Update overlay snapshot golden` workflow does the work, or
   you copy the failing CI run's fresh render into `screenshots/`
   by hand.

What NOT to do:

- **Don't add `RT_BUNDLED_FONT` declarations directly inside
  `openxr-api-layer.rc.in` or `openxr-api-layer-tests.rc`.** They
  belong in the shared include. A direct declaration in only one
  side defeats the purpose of the shared include and re-opens
  the silent-drift bug.
- **Don't put a path prefix on the filename argument.** Write the
  bare basename ("MyFile.ttf"), not "fonts\\MyFile.ttf" or
  "..\\openxr-api-layer\\fonts\\MyFile.ttf". A prefix hard-codes
  one binary's view of the filesystem into the shared include —
  it breaks the other binary's build (or worse, succeeds because
  the prefix happens to also resolve from the other project's
  directory by coincidence). `/I` is the documented mechanism for
  cross-binary path resolution; let it do its job.

### When the diff fails

`overlay snapshot — render mock to PNG (visual-regression artifact)`
prints:

```
snapshot diverges from golden: 482 pixels differ; first at (x=178, y=24);
fresh render kept at overlay_snapshot.png
```

The PR's Actions tab then carries an automatic artifact
`overlay-snapshot-diff-<branch>-<attempt>/` with:

- `new/overlay_snapshot.png` — the freshly-rendered output
- `old/overlay_snapshot.png` — the committed golden

Download the artifact, open both PNGs in any image viewer (or run
`compare -metric AE old.png new.png diff.png` from ImageMagick for a
per-pixel difference map). Workflow to handle the failure:

1. **Decide first whether the change is intentional.** If your last
   commit touched the renderer (layout, palette, fonts, …), the diff
   is expected. If it didn't, the diff is a real regression — revert
   and investigate.
2. **For a real regression**: the diff artifact + the `(x, y)` of the
   first differing pixel from the test log usually localises the
   problem in seconds.
3. **For an intentional change**: regenerate the golden — see below.

The automatic artifact is only uploaded when the test fails (so
green-CI runs stay clean) and only on the Release matrix entry
(Debug renders bit-identically because the entire pipeline is
software WIC + D2D — no point uploading the same image twice).

### Regenerating the golden

Two ways to do this, both safe:

**Option A — `Update overlay snapshot golden` workflow (recommended)**

The `.github/workflows/update-overlay-snapshot.yml` workflow runs
the test on CI, captures the fresh render, and either uploads it as
an artifact for review (dry-run, the default) or commits it directly
to the source branch.

1. GitHub → Actions tab → "Update overlay snapshot golden" →
   "Run workflow".
2. Pick your feature branch in the ref dropdown.
3. Leave the **`commit`** checkbox unchecked → "Run workflow". This
   is the dry-run: builds, runs the test, and uploads an artifact
   `overlay-snapshot-regen-<branch>/` containing both `new/overlay_snapshot.png`
   (the fresh render) and `old/overlay_snapshot.png` (the current
   golden).
4. Download the artifact, eyeball both PNGs in your image viewer.
5. Happy with the change? Re-run the workflow with the
   **`commit`** checkbox ticked. The bot now overwrites
   `screenshots/overlay_snapshot.png` on your branch and pushes
   a commit; the normal CI then re-runs and the snapshot test
   passes against the new baseline.

Why two steps: the golden is the visual contract for the whole HUD.
A single-click "regenerate" workflow makes it too easy for a layout
regression to sneak in unnoticed. The dry-run forces a deliberate
30-second eyeball check before the file actually changes.

The workflow refuses to push to branches with branch-protection
that disallow GITHUB_TOKEN — main is usually protected this way,
so golden updates on main MUST go through a PR. That's a feature.

**Option B — manual fallback**

If CI is down or you want full local control:

1. Locally: build + run the test (`bin\x64\Release\openxr-api-layer-tests.exe`).
   The test fails the snapshot CHECK but writes
   `overlay_snapshot.png` to the test EXE's working directory.
2. Copy the fresh PNG over `screenshots/overlay_snapshot.png`.
3. `git commit` + push.

The PR diff in GitHub's image-diff view shows the rendering change
side-by-side for reviewers.

### Why the test doesn't tolerate any pixel diff

Tempting to allow "small differences" with a perceptual diff library
(SSIM, pHash, …) — but on a deterministic software pipeline like
this one, any pixel diff IS a real change, never noise. Tolerance
turns a strict regression test into a flaky one. If the test ever
becomes flaky despite the software path, the right fix is to find
the source of nondeterminism (system time leaking into a string?
locale-sensitive number format? font fallback?), not to widen the
tolerance.

## ETW tracing

The layer emits structured events via `TraceLoggingWrite(...)` to an
ETW provider named `OpenXRTemplate`
(`{cbf3adcd-42b1-4c38-830c-91980af201f8}`). `Log()` writes a parallel
text log alongside the `sessions/` folder under `%LOCALAPPDATA%\<layer>\`,
but ETW is what surfaces in profilers and merges cleanly with kernel /
D3D / DXGI traces.

[`scripts/Tracing.wprp`](../scripts/Tracing.wprp) is a Windows
Performance Recorder profile bundling the layer's provider with
Watson + DXGI Debug:

```powershell
# Capture (admin shell)
wpr -start scripts\Tracing.wprp -filemode
# … reproduce the issue inside the OpenXR game …
wpr -stop trace.etl

# View: open trace.etl in Windows Performance Analyzer (WPA), or
# `tracelog -dump trace.etl` for a textual dump.
```

The provider name is inherited from mbucchia's template, so WPA
labels traces as `OpenXRTemplate` rather than `XrTelemetry`. Edit
[`framework/log.cpp`](../openxr-api-layer/framework/log.cpp) (provider
name + GUID) if you want to rebrand.

## Conformance testing

Before tagging a `v*.*.*` release, run the OpenXR Conformance Test
Suite against the layer on your target runtime — see
[`CTS_TESTING.md`](./CTS_TESTING.md) for the release-gate playbook
(baseline vs with-layer diff, runtime setup, release checklist).

## Releases

The GitHub Actions workflow
([`build-and-release.yml`](../.github/workflows/build-and-release.yml))
builds `Release` and `Debug` x64 on every push to `main` (as a sanity
check), every PR, and every `v*.*.*` tag. On a tag push it
additionally creates a GitHub Release and attaches:

- `XR_APILAYER_MLEDOUR_xr_telemetry-<version>-x64-Setup.exe` —
  Inno Setup installer (recommended for end users)
- `XR_APILAYER_MLEDOUR_xr_telemetry-Release-x64.zip` — raw DLL +
  JSON + PowerShell scripts, for manual installation or development
- `XR_APILAYER_MLEDOUR_xr_telemetry-Debug-x64.zip` — debug build
  with full symbols, for troubleshooting

To publish a new release:

```bash
git tag v0.1.0
git push origin v0.1.0
```

The tag is derived into the DLL's `VERSIONINFO` resource at build
time via
[`scripts/Generate-VersionRc.ps1`](../scripts/Generate-VersionRc.ps1),
and into the installer's filename and `AppVersion` via the
`/DMyAppVersion` flag passed to ISCC.

## Code signing

The template ships a fully wired-up Certum SimplySign Cloud signing
pipeline. It's **optional** — without the GitHub Secrets set, every
signing step skips cleanly and you get unsigned artifacts.

When configured, both the layer DLL and the `Setup.exe` installer are
signed with a Certum Open Source Code Signing Cloud certificate
(FIPS 140-2 Level 2 cloud HSM, ~25 €/yr at time of writing — the
cheapest legitimate option for personal open-source projects in
2026).

**Only tag pushes are signed.** Push-to-main, pull-request,
workflow_dispatch and fork-triggered builds all produce **unsigned**
artifacts on purpose — they're verification builds, not user-facing
ones, and signing every commit would burn a Certum cloud-HSM session
per push for no benefit. Forks additionally don't have access to the
GitHub secrets by GitHub's own design.

Anti-cheat systems may still flag any layer DLL — even a signed one
— loaded into a hooked process; SmartScreen "Unknown publisher"
warnings disappear once the signed installer accumulates enough
downloads to build reputation.

### How CI signs in an automated way

Certum's official manual only documents the interactive flow (open
SimplySign Desktop, type the TOTP from a phone, click "Sign in").
SimplySign Desktop **does ship an undocumented headless mode**,
discovered by reverse-engineering the binary with ILSpy:

```
SimplySignDesktop.exe /autologin <username> <totp>
```

Discovered and shared in a comment thread on
[devas.life](https://www.devas.life/how-to-automate-signing-your-windows-app-with-certum/).
Certum doesn't document this anywhere — including the official
"Code Signing in the Cloud" manual — but it's been confirmed to work
on non-interactive Windows agents (CI services running as Windows
services), which is exactly the GitHub Actions runner profile.

After `/autologin` succeeds, SimplySignDesktop runs as a background
process (no UI, no tray icon) acting as the bridge between Certum's
cloud HSM and the Windows certificate store. signtool with
`/sha1 <thumbprint>` then sees the cert through CurrentUser\My and
signs as normal.

The companion flag `/close` shuts the session down gracefully — useful
both as cleanup at the end of a run, and as a "clear leftover state"
step before a new login (Certum allows only one cloud session per
host at a time).

The pieces:

1. The workflow downloads the SimplySign Desktop MSI from
   `files.certum.eu` at a **pinned version** (`SIMPLYSIGN_VERSION`
   env var in `build-and-release.yml`) and installs it silently with
   `msiexec /qn /norestart`.
2. [`scripts/Get-CertumTotp.ps1`](../scripts/Get-CertumTotp.ps1)
   regenerates a 6-digit RFC 6238 TOTP on demand from the Base32 seed
   that the SimplySign portal exposes under "Show secret key" (the
   same string visible in the `secret=` parameter of the
   `otpauth://` enrolment URI). Pure PowerShell + .NET, no extra
   modules to install on the runner. Uses **HMAC-SHA256** — Certum's
   `otpauth://` URI specifies `algorithm=SHA256`, not the RFC 6238
   default of SHA-1. Generating SHA-1 codes against a SHA-256 seed
   produces wrong codes silently, so this is verified against RFC 6238
   Appendix B vectors in
   [`scripts/Test-CertumTotp.ps1`](../scripts/Test-CertumTotp.ps1).
3. [`scripts/Sign-Artifact.ps1`](../scripts/Sign-Artifact.ps1)
   stops any leftover SimplySignDesktop process (`/close` then kill
   if needed), then runs `/autologin <username> <totp>` and watches
   the launched process: a successful login keeps the process alive
   indefinitely (the bridge is now active), a failed login makes it
   exit within a couple of seconds. We probe for ~5 s before deciding.
   On failure we retry with the previous and next TOTP windows
   (drift offsets `-1`, `+1` × 30 s) to absorb runner-clock skew of
   up to ±30 s. If all three fail, we give up with a clear actionable
   message. After login, we poll `Cert:\CurrentUser\My` for the
   configured thumbprint to confirm the bridge is live, then run
   `signtool sign /sha1 <thumb> /tr http://time.certum.pl /td sha256
   /fd sha256` exactly as the Certum manual prescribes, then
   `signtool verify /pa /v`. A `finally` block runs `/close` so the
   runner doesn't leave a lingering session that would block the
   next CI run.
4. The workflow gates every signing-related step (MSI cache restore,
   MSI install, sign DLL, sign Setup.exe) on a `should_sign` flag
   computed once up front. The flag is true iff **all three** of:
   - `GITHUB_REF` matches `refs/tags/v*` (we're on a release tag),
   - the matrix entry is `Release` (Debug never signs), and
   - the `CERTUM_USERNAME` secret is set (PR/fork builds without
     access to secrets fall through cleanly).

   Computing it once and only once keeps the per-step `if:`
   conditions tidy and avoids referencing `secrets.*` inside step-
   level `if:` expressions, which GitHub Actions does not allow.

5. The MSI itself is cached via `actions/cache@v4` keyed on
   `SIMPLYSIGN_VERSION`. The first signed-release run after a
   version bump downloads the ~50-100 MB installer; subsequent runs
   on the same version reuse the cached bytes. The msiexec install
   step always runs — we deliberately do **not** cache the installed
   `Program Files\Certum\SimplySign Desktop\` tree because the
   install registers components the cert-store bridge depends on,
   and a plain directory copy would skip that registration.

### Brittleness notes

The `/autologin` flag is undocumented, so a future SimplySign Desktop
release could rename or remove it. Mitigations baked in:

- **Version-pinned MSI download.** The workflow installs a specific
  `SIMPLYSIGN_VERSION` (currently `9.4.3.90`) — Certum can't push a
  silent breaking change. Bumping is a deliberate step, see below.
- **Process-state probe.** A bad OTP or a removed `/autologin` flag
  both manifest as the launched process exiting within a couple of
  seconds; we detect that and fail fast with a labelled error
  instead of "succeeding" with an unsigned binary.
- **Cert-store poll.** Even if `/autologin` somehow returned 0
  without genuinely logging in, the cert wouldn't appear in
  `CurrentUser\My`, and we'd fail before signtool ever runs.

When bumping `SIMPLYSIGN_VERSION` in the workflow:

1. Run `scripts\Sign-Artifact.ps1` against the new MSI on a local
   Windows VM with the secrets set. Verify a sample DLL signs.
2. If `/autologin` no longer behaves the same way, fall back to a
   self-hosted runner with an interactively-logged-in SimplySign
   session (see "Why not just SendKeys" below) until a fix is found.
3. Then bump the env var in `.github/workflows/build-and-release.yml`.

### Why not just SendKeys

We tried first. Certum's GUI login window does have a recognizable
title (`SimplySign Desktop`) and tab order (username → Tab → OTP →
Enter), so a `WScript.Shell.AppActivate` + `SendKeys` flow looks
plausible on paper, and works on a developer's interactive desktop
(per [devas.life](https://www.devas.life/how-to-automate-signing-your-windows-app-with-certum/)).
On a `windows-2022` GitHub-hosted runner, however, SimplySign
Desktop creates its windows hidden by default (it's tray-app
flavored), and force-showing them via `ShowWindow(SW_RESTORE)` was
observed to *destroy* the window outright — runs left the
SimplySign* PID owning only `Default IME` windows. After 60 s of
patient waiting, the login dialog never auto-appeared either.
`/autologin` sidesteps the entire UI lifecycle.

### Required GitHub Secrets

These three secrets must be configured at the repository level
(Settings → Secrets and variables → Actions → New repository secret)
for Release-tag builds to produce signed binaries. They are **never**
echoed by `Sign-Artifact.ps1` and `Get-CertumTotp.ps1`, and the
PowerShell scripts pass them as process arguments rather than through
`cmd /c` so they don't leak into the shell-history transcript.

Certum SimplySign uses 2FA where the **TOTP is the second factor** —
there is no separate static password to set, so we don't need a
`CERTUM_PASSWORD` secret. Username + freshly-generated TOTP is the
full credential set SimplySign Desktop expects.

| Secret | Source | Format |
|--------|--------|--------|
| `CERTUM_USERNAME` | SimplySign portal login (the email you registered with) | string |
| `CERTUM_TOTP_SEED` | SimplySign portal → "Show secret key" (the Base32 string behind the QR code, NOT a snapshot of the current 6-digit code) | Base32, 16+ chars |
| `CERTUM_CERT_THUMBPRINT` | SHA-1 thumbprint of the issued certificate, no spaces | 40 hex chars |

To find the thumbprint once the cert is loaded into your local
SimplySign Desktop session:

```powershell
Get-ChildItem Cert:\CurrentUser\My |
    Where-Object Subject -Match '<your-name>' |
    Format-List Thumbprint, Subject, NotAfter
```

Re-run that command after each Certum certificate renewal — the
thumbprint changes with every new cert, so the secret has to be
updated. The renewal cadence is yearly for the Open Source tier.

### Renewing the seed / rotating credentials

If the TOTP seed leaks (or you suspect it has), reset it from the
SimplySign portal: a new seed invalidates the old one immediately.
Update `CERTUM_TOTP_SEED` in the GitHub Secrets in the same window
or the next CI run will fail to log in.

## License

MIT License — see [LICENSE](../LICENSE).

Based on the
[OpenXR-Layer-Template](https://github.com/mbucchia/OpenXR-Layer-Template)
by Matthieu Bucchianeri (`mbucchia`), Copyright © 2022–2023. The
framework code (dispatch generator, entry point, logging helpers) is
his work; everything you add in `layer.cpp` and friends is yours.
