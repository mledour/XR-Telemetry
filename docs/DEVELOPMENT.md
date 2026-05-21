# Development

Notes for anyone forking this template to build their own OpenXR API
layer. End-user install and configuration of YOUR layer go in your
fork's [README](../README.md); this file documents the template's
build system, CI pipeline, and signing flow.

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
    overlay_renderer.{h,cpp}        ← in-headset HUD (D2D/DirectWrite)
    overlay_aggregator.{h,cpp}      ← per-frame metric accumulator
    histogram_ring.h                ← ring buffer for frametime samples
    gpu_telemetry.{h,cpp}           ← NvAPI / DXGI VRAM polling
  fonts/                            ← bundled Rajdhani (subset to ASCII + °)
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
  Init-Template.ps1                 ← post-clone placeholder substitution
  Generate-VersionRc.ps1            ← bakes git tag into VERSIONINFO
  Sign-Artifact.ps1                 ← headless Certum signing (CI)
  Get-CertumTotp.ps1                ← RFC 6238 TOTP generator
  Test-CertumTotp.ps1               ← offline TOTP self-test
  Install-Layer.ps1                 ← HKLM register (manual install)
  Uninstall-Layer.ps1               ← HKLM unregister
```

## Relationship to upstream

Three repos in the lineage:

1. **[`mbucchia/OpenXR-Layer-Template`](https://github.com/mbucchia/OpenXR-Layer-Template)**
   — the original framework. Owns everything under
   `openxr-api-layer/framework/`, the dispatch generator, the entry
   point, the logging helpers, and the basic vcxproj structure.
2. **`mledour/OpenXR-Layer-Template`** (this repo) —
   adds the CI pipeline, code signing, Inno Setup installer, doctest
   + mock runtime, D3D12 support in `pch.h`, and the post-clone init
   script. Framework code is untouched.
3. **Your layer** — a clone of this template via the GitHub "Use this
   template" button. You add layer-specific logic in `layer.cpp` and
   tests in `openxr-api-layer-tests/test_*.cpp`.

Upstream framework changes from mbucchia land in this template via a
manual sync (rare). Your own layer doesn't need to track those —
this template is your stable base.

## Building from source

```powershell
git submodule update --init --recursive
# (one-time, after using the GitHub Template button)
powershell -ExecutionPolicy Bypass -File .\scripts\Init-Template.ps1
```

Then open `XR_APILAYER_MLEDOUR_xr_telemetry.sln` (real names
after init) in Visual Studio and build `Release|x64`. The pre-build
event chain:

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
need to rename a layer after the init script has run.

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

## Overlay renderer paths (D3D11): direct vs shim

The D3D11 overlay renderer has two ways to put a D2D-painted HUD onto
the OpenXR swapchain image. Which one runs in a given session depends
on `overlay.renderer_path` in `settings.json` AND on the app's D3D11
device flags. The setting itself is documented in
`installer/default_settings.json`; this section explains what's behind
each choice.

### Shim path (default + guaranteed-to-work fallback)

```
Layer DLL                                  App
─────────                                  ──────────
m_myDevice (D3D11 + BGRA_SUPPORT)          m_device (the app's D3D11)
m_myShim (BGRA texture, shared NT handle)
m_myShimRenderTarget (D2D RT on shim) ┐
                                       │
        ┌── keyed-mutex pair (key 0/1)─┤
        │                              │
        │                              m_appShim (same texture, opened on app device)
        │                              m_appShimMutex
        │                              m_context->CopyResource(swapchainImage, m_appShim)
        ▼
   D2D paint goes here every frame
```

Per frame:
1. `m_myShimMutex.AcquireSync(0, 50)` — wait for app side to finish
   the previous copy (50 ms ceiling guards against host-side deadlock).
2. `m_core.paint(m_myShimRenderTarget, snap, ...)` — D2D paints.
3. `m_myShimMutex.ReleaseSync(1)` — hand the key over to the copy side.
4. `m_appShimMutex.AcquireSync(1, 50)` — copy side claims the key.
5. `m_context->CopyResource(m_images[idx], m_appShim)` — copy on the
   app's device.
6. `m_appShimMutex.ReleaseSync(0)` — restore key=0 for the next frame.

Why this exists: D2D requires the underlying D3D11 device to have been
created with `D3D11_CREATE_DEVICE_BGRA_SUPPORT`. Almost no VR game
sets that flag (Unity / Unreal RHIs don't, neither do iRacing / DCS /
MSFS / DR2 / etc.) so paint-on-the-app-device fails with
`E_INVALIDARG` on `CreateDxgiSurfaceRenderTarget`. The shim is the
defensive workaround: we own a private D3D11 device with BGRA, paint
there, then copy via a shared NT handle.

Cost per frame: ~100–300 µs (2 keyed-mutex acquire/release pairs +
one cross-device `CopyResource` of a 720×480 BGRA texture).
Memory cost: one 1.4 MB shim texture + one D3D11 device + one
kernel NT handle, all for the session's lifetime.

### Direct path (opt-in fast path)

```
Layer DLL                                  App
─────────                                  ──────────
m_directRTs[N]  (one ID2D1RenderTarget    m_device (the app's D3D11)
                 per swapchain image, on  m_images[N] (OpenXR swapchain)
                 the app's device)
       │
       └── D2D paint goes straight into m_directRTs[idx] every frame
           (NO mutex, NO copy, NO shim)
```

Per frame:
1. `m_core.paint(m_directRTs[idx], snap, ...)` — D2D paints directly
   into the swapchain image via the app's D3D11 device.

That's it. No mutex, no copy. D2D's `BeginDraw`/`EndDraw` saves and
restores the app's D3D11 state, so the app's `ImmediateContext` is
clean afterwards.

### Detection logic

In `D3D11OverlayRenderer::init()`, after the swapchain + images are
populated:

```cpp
if (m_pathSetting != OverlayRendererPath::Shim &&
    tryInitDirectPath()) {
    m_useDirectPath = true;
    // log + return
}
// else fall through to shim init
```

`tryInitDirectPath()` succeeds only when:
1. `m_device->GetCreationFlags() & D3D11_CREATE_DEVICE_BGRA_SUPPORT`
2. `m_core.d2d()->CreateDxgiSurfaceRenderTarget()` succeeds on every
   `m_images[i]` (handles the case where the runtime stores BGRA
   swapchain images as TYPELESS — Pimax has been observed doing this,
   and D2D refuses TYPELESS surfaces).

`renderer_path` semantics:

| Value | Behaviour |
|---|---|
| `auto` (default) | Try direct. If `tryInitDirectPath()` returns false, silently fall back to shim. The probe failures are not logged in this mode. |
| `direct` | Try direct. If the probe fails, log the *specific* reason (missing BGRA_SUPPORT, `IDXGISurface` query fail, or `CreateDxgiSurfaceRenderTarget` failure with HRESULT) and fall back to shim anyway — never refuse to load the HUD. |
| `shim` | Skip the probe entirely, go straight to shim init. Forces the conservative path; useful for opting out if a future D2D / driver change breaks the direct path on a specific machine. |

### Which path will my session pick?

| App / engine | Direct works? | Default (Auto) chooses |
|---|---|---|
| Unity (URP / HDRP) VR | No (no `BGRA_SUPPORT`) | Shim |
| Unreal Engine 4 / 5 VR | No | Shim |
| iRacing, DCS, MSFS, DR2, LMU, AC | No | Shim |
| Custom D3D11 app that uses D2D itself | Likely yes | Direct |
| Test app built with `D3D11_CREATE_DEVICE_BGRA_SUPPORT` for layer testing | Yes | Direct |

In practice the direct path benefits well under 5 % of VR titles. The
setting exists primarily so that future apps that DO request
`BGRA_SUPPORT` (e.g. unified D2D-based HUD pipelines) get the fast
path automatically, and so that anyone debugging a shim glitch can
force the conservative path.

### Why the snapshot test doesn't cover either path

`test_overlay_snapshot.cpp` uses `renderOverlayToTarget` with a WIC
bitmap RT — it doesn't go through `D3D11OverlayRenderer` at all, so
neither the shim nor the direct path is exercised. Both paths produce
visually identical D2D output (same `m_core.paint()` invoked the same
way), so a passing snapshot test does NOT prove that a session-level
regression hasn't crept in.

Real-runtime validation requires running the layer in a HMD with a
game. That's why the `OverlayRendererPath::Direct` log line includes
"DIRECT path — N swapchain RTs on app device" — a user reporting a
HUD issue can grep their session log for that line to confirm which
path was selected.

### Adding a new path

If a future runtime introduces a third compositing model (e.g.
SwapChainPanel for Windows MR Cliff House, or DComp surfaces for
spatial overlays), follow the same pattern:

1. Add a value to `OverlayRendererPath` in `settings.h`.
2. Add a probe helper (`tryInitFooPath()`) that returns false on any
   incompatibility, releasing partial state.
3. Branch `init()` to attempt the new path BEFORE the shim, falling
   through to shim on any failure.
4. Branch `renderAndCompose()` to handle the new path's per-frame work.
5. Document the trade-offs here.

## Snapshot (visual-regression) tests

### What it does and why

`test_overlay_snapshot.cpp` renders the in-headset HUD with hard-coded
mock telemetry into an offscreen WIC bitmap via the exact same
`CoreRenderer` the production renderers use, encodes the bitmap to
PNG, and diffs the result pixel-for-pixel against the golden image
checked in at `screenshots/overlay_snapshot.png`.

This catches things that ordinary unit tests can't: layout shifts of
a few pixels, palette regressions, font substitution, mojibake (a
literal `°` byte gets corrupted to `Â°` when MSVC reads a UTF-8
source file as CP1252), accidental edits to gradients or stroke
widths, off-by-one in any of the dozens of `DrawLine` /
`FillGeometry` / `DrawText` calls inside the renderer. A passing CI
run is a per-commit guarantee that the HUD still looks exactly the
way the golden looks.

The rendering is deterministic across CI runners because:

- WIC's bitmap render target is a **software** D2D context (no GPU,
  no driver differences).
- The mock snapshot in `makeMockSnapshot()` is a hard-coded `OverlaySnapshot`
  struct — no clock reads, no system queries.
- The bundled Rajdhani font (subsetted to ASCII + `°`/`µ`/`×`, ~11 KB
  each weight) is embedded in the test EXE so DirectWrite never falls
  back to whatever system font happens to be installed.
- Both halves of the comparison go through the same PNG
  encode + un-premultiply + decode pipeline (the fresh PNG is written
  to disk, then re-decoded), so PBGRA→BGRA precision noise cancels out.

### How a CI run plays out

1. MSBuild compiles `openxr-api-layer-tests.rc` into the test EXE
   alongside the C++ TUs. rc.exe bakes the Rajdhani TTF bytes into
   the EXE's resource table (custom type `RT_BUNDLED_FONT` == 256,
   IDs 200 + 201).
2. The PowerShell test step runs `openxr-api-layer-tests.exe`. The
   snapshot test creates a 720×480 WIC bitmap RT, calls
   `renderOverlayToTarget(...)`, encodes to `overlay_snapshot.png`
   in the runner's CWD.
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
| `openxr-api-layer-tests/openxr-api-layer-tests.rc` | Resource script that embeds the Rajdhani TTFs into the test EXE |
| `openxr-api-layer/openxr-api-layer.rc.in` | Production-DLL counterpart — same TTFs, same IDs, same custom type |
| `openxr-api-layer/fonts/Rajdhani-{SemiBold,Bold}.ttf` | The bundled fonts — referenced from both `.rc` files |
| `screenshots/overlay_snapshot.png` | The golden, 720×480 RGBA, ~68 KB |
| `openxr-api-layer/utils/overlay_renderer.cpp` | `renderOverlayToTarget()` — the shared entry point used by both the in-headset path and the snapshot test |

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
must be declared in BOTH `.rc` files, with identical custom type
and IDs. Skip the test `.rc` entry and `FindResource` returns null
in the test EXE — the renderer falls back to its degraded path
(e.g. system Bahnschrift instead of bundled Rajdhani for fonts),
which makes the snapshot diverge from the in-headset rendering and
defeats the regression-test value.

Checklist for adding a new bundled resource:

1. **Place the file** under `openxr-api-layer/<some-folder>/` (e.g.
   `openxr-api-layer/fonts/` for fonts, `openxr-api-layer/images/`
   for PNGs). Pick a stable folder so both `.rc` files can reference
   the same path.
2. **Pick a custom resource type** if you don't already have one.
   Numeric IDs ≥ 256 are safe (Windows reserves 1–255 for the
   predefined types like `RT_BITMAP`, `RT_ICON`, etc.). Reuse
   `RT_BUNDLED_FONT (256)` if it's another font.
3. **Pick a resource ID** ≥ 200 (this codebase's convention; under
   200 is reserved for IDs the DLL's `.rc.in` template might add for
   icons, dialogs, etc.).
4. **Declare in `openxr-api-layer/openxr-api-layer.rc.in`**:
   ```rc
   #define IDR_MY_NEW_RESOURCE   202
   #define RT_BUNDLED_FONT       256   // reuse if existing
   IDR_MY_NEW_RESOURCE RT_BUNDLED_FONT "fonts\\MyFile.ttf"
   ```
5. **Declare in `openxr-api-layer-tests/openxr-api-layer-tests.rc`**
   with the SAME ID and type — path is relative to the test project
   so it'll be `..\openxr-api-layer\fonts\MyFile.ttf` (the file lives
   in the layer project, the .rc only references it):
   ```rc
   #define IDR_MY_NEW_RESOURCE   202
   #define RT_BUNDLED_FONT       256
   IDR_MY_NEW_RESOURCE RT_BUNDLED_FONT "..\\openxr-api-layer\\fonts\\MyFile.ttf"
   ```
6. **Add the file to both vcxprojs as `<None Include>`** so Solution
   Explorer shows it and the build triggers when it changes. The
   `<ResourceCompile>` entry that pulls in the `.rc` is already
   there — no new entry needed.
7. **Re-render and refresh the golden**: the snapshot will now use
   your new resource. Regenerate `screenshots/overlay_snapshot.png`
   (currently by copying the fresh CI render manually; a manual
   `workflow_dispatch` workflow to automate this is on the roadmap),
   commit it, push. The diff in the PR will show the rendering
   change.

### When the diff fails

`overlay snapshot — render mock to PNG (visual-regression artifact)`
prints:

```
snapshot diverges from golden: 482 pixels differ; first at (x=178, y=24);
fresh render kept at overlay_snapshot.png
```

Workflow to handle a failure:

1. **Pull the branch locally**, build, run the test. The freshly-rendered
   PNG ends up at `bin\x64\<cfg>\overlay_snapshot.png` (the test EXE's
   working directory).
2. **Open both PNGs side-by-side**. Any image viewer that supports
   tabs / pixel-zoom works; ImageMagick `compare` produces a diff
   image (`compare -metric AE golden.png fresh.png diff.png`).
3. **Decide**: is the diff a real regression (revert / fix the
   offending code) or an intentional design change (update the
   golden)?
4. **To update the golden**: copy the fresh PNG over
   `screenshots/overlay_snapshot.png`, commit, push. The PR diff
   will visualise the change. Reviewer can compare base vs head
   in GitHub's image-diff view.

### Why the test doesn't tolerate any pixel diff

Tempting to allow "small differences" with a perceptual diff library
(SSIM, pHash, …) — but on a deterministic software pipeline like
this one, any pixel diff IS a real change, never noise. Tolerance
turns a strict regression test into a flaky one. If the test ever
becomes flaky despite the software path, the right fix is to find
the source of nondeterminism (system time leaking into a string?
locale-sensitive number format? font fallback?), not to widen the
tolerance.

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
