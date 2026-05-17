# OpenXR-Layer-Template

A starting point for building an OpenXR API layer on Windows, with
everything-but-the-kitchen-sink scaffolding so you can focus on the
layer's own logic instead of plumbing.

Based on [`mbucchia/OpenXR-Layer-Template`](https://github.com/mbucchia/OpenXR-Layer-Template)
— huge thanks to Matthieu Bucchianeri for the original framework. This
template **adds** the following on top of his work:

- **GitHub Actions CI** that builds Debug + Release x64, runs unit
  tests, builds an Inno Setup installer, signs everything, and creates
  a GitHub Release on every `v*.*.*` tag push
- **Code signing pipeline** (Certum Open Source Code Signing Cloud)
  driven headlessly via the undocumented `SimplySignDesktop /autologin`
  flag — the only known public recipe for headless Certum signing on
  a GitHub-hosted Windows runner. Skip-on-fork built in
- **Inno Setup installer** that installs to `C:\Program Files\` (correct
  ACLs for sandboxed identities), registers under HKLM, and creates an
  Add/Remove Programs entry
- **VERSIONINFO from git tag** baked into the DLL via a pre-build script
- **doctest unit test target** with an in-process mock OpenXR runtime,
  so you can drive your layer through `xrCreateInstance` ↔ `xrEndFrame`
  without a loader, GPU, or HMD
- **D3D11 + D3D12 (via D3D11On12) support in `pch.h`**, all delay-loaded
  so your layer DLL doesn't load `d3d11.dll` / `d3d12.dll` into game
  processes that never exercise your graphics path
- **HKLM install / uninstall PowerShell scripts** + signed installer
- **`docs/DEVELOPMENT.md`** with the signing pipeline written up in
  enough detail that you (or your future self) can debug it
- **`docs/CTS_TESTING.md`** for how to run the OpenXR CTS against your
  layer on a real machine — useful long before you ship

## Quick start

```powershell
# 1. Click "Use this template" on GitHub to get your own copy, then
#    clone it locally.
git clone https://github.com/<your-user>/<your-repo>.git
cd <your-repo>

# 2. Pull the OpenXR submodules.
git submodule update --init --recursive

# 3. Run the post-clone init script. It asks for your vendor tag,
#    layer name, your real name, year, etc., and substitutes them
#    across the tree (filenames AND file contents).
#    Windows:
powershell -ExecutionPolicy Bypass -File .\scripts\Init-Template.ps1
#    macOS / Linux / WSL:
#    bash scripts/init-template.sh

# 4. Open the renamed .sln in Visual Studio 2019+ and build Release|x64.
#    The pre-build event runs framework\dispatch_generator.py to produce
#    dispatch.gen.{h,cpp} from layer_apis.py, then bakes the version into
#    VERSIONINFO. You should get a DLL + test binary at the first build.
```

The shipped skeleton overrides exactly one OpenXR function
(`xrCreateInstance`) and just logs the application name and runtime
identity. That's a sanity check the layer is loading. From there:

- Add functions to `override_functions` in
  [`openxr-api-layer/framework/layer_apis.py`](./openxr-api-layer/framework/layer_apis.py)
- Override the matching method on `OpenXrLayer` in
  [`openxr-api-layer/layer.cpp`](./openxr-api-layer/layer.cpp)
- Anything you don't override is forwarded to the next layer / runtime
  automatically by the framework

The `layer.cpp` in the template has commented examples for a typical
`xrLocateViews` override.

## What's in the box

```
.
├── .github/workflows/build-and-release.yml   # CI: build, test, sign, release
├── installer/installer.iss                   # Inno Setup script (auto-built)
├── openxr-api-layer/
│   ├── framework/                            # dispatch generator, entry, log
│   ├── layer.cpp / layer.h                   # ← your layer code goes here
│   ├── pch.h                                 # D3D11 + D3D12 includes (delay-loaded)
│   └── XR_APILAYER_<vendor>_<name>.json      # loader manifest
├── openxr-api-layer-tests/
│   ├── main.cpp                              # doctest entry point
│   ├── mock_runtime.{h,cpp}                  # in-process OpenXR mock
│   ├── test_example.cpp                      # ← your tests go here
│   └── test_stubs.cpp                        # symbols entry.cpp normally provides
├── scripts/
│   ├── Init-Template.ps1                     # post-clone placeholder substitution
│   ├── Generate-VersionRc.ps1                # bakes git tag into VERSIONINFO
│   ├── Get-CertumTotp.ps1                    # RFC 6238 TOTP (Certum auth)
│   ├── Sign-Artifact.ps1                     # headless Certum signing flow
│   ├── Test-CertumTotp.ps1                   # offline TOTP self-test
│   ├── Install-Layer.ps1                     # HKLM register (manual install)
│   └── Uninstall-Layer.ps1                   # HKLM unregister
├── docs/
│   ├── DEVELOPMENT.md                        # CI + signing + framework internals
│   └── CTS_TESTING.md                        # how to run OpenXR CTS locally
├── external/                                 # OpenXR SDK + MixedReality (submodules)
├── LICENSE                                   # MIT (yours + mbucchia attribution)
└── README.md                                 # ← rewrite this for your layer
```

## Frame-timing analysis

This layer's runtime job is to record one CSV row per frame, capturing
**every CPU + GPU segment** of the OpenXR frame cycle. The output lands
at
`%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_xr_telemetry\sessions\YYYY-MM-DD_HH-MM-SSZ_<AppName>.csv`,
one file per session, openable directly in Excel / Pandas / LibreOffice.

### Schema

```
frame, timestamp_qpc,
wait_block_ns, pre_begin_ns, app_cpu_ns, end_frame_ns, frame_total_ns,
gpu_time_ns,
period_ns, headroom_pct, gpu_headroom_pct, should_render
```

| Column | Definition | What it captures |
|---|---|---|
| `frame` | Sequential counter, 0-based | Frame index since session start |
| `timestamp_qpc` | Raw `QueryPerformanceCounter` tick at xrEndFrame entry | Frame-end wall clock; convert to seconds with `QueryPerformanceFrequency` |
| `wait_block_ns` | `tWaitOut − tWaitIn` | **Compositor throttle.** Time the runtime made the app wait inside `xrWaitFrame`. Big = compositor has headroom and is rate-limiting the app. Small = app is the bottleneck, runtime returned the moment it could. |
| `pre_begin_ns` | `tBegin − tWaitOut` | **Housekeeping.** Time between `xrWaitFrame` returning and `xrBeginFrame` being called. Game-side input poll, state update before render kickoff. Usually small (~50-300 µs); larger means the app is doing meaningful work here. |
| `app_cpu_ns` | `tEnd − tWaitOut` | **Wait→End window** = `pre_begin_ns` + render submission. The CPU time the app spent between `xrWaitFrame` returning and `xrEndFrame` being called. Render-thread heaviness. |
| `end_frame_ns` | Duration of the downstream `xrEndFrame` call | **Runtime/compositor ingest overhead.** Time the runtime + any downstream layer spent inside `xrEndFrame` (layer composition, projection correction, compositor handoff). On mature runtimes (SteamVR / Oculus) typically a few hundred µs; on young runtimes (Pimax OpenXR 0.1.0) can reach 1-2 ms. |
| `frame_total_ns` | `tEnd_now − tEnd_prev` | **Full cycle duration.** End-to-end wall clock of the previous frame. Includes the post-`xrEndFrame` work (game simulation, physics, AI, input polling) that `app_cpu_ns` cannot see because it happens AFTER the app returned from rendering and BEFORE the next `xrWaitFrame`. `0` on the first frame of a session. |
| `gpu_time_ns` | D3D11 timestamp delta from `xrBeginFrame` to `xrEndFrame` | **App GPU work for this frame.** Measured with `D3D11_QUERY_TIMESTAMP` bracketed by a `D3D11_QUERY_TIMESTAMP_DISJOINT` for frequency validation. `0` when no D3D11 binding was seen at `xrCreateSession` (Vulkan / OpenGL / D3D12 apps), when the disjoint query reported `Disjoint == true` (driver invalidated the range), or when the GPU result wasn't yet ready at session end. |
| `period_ns` | `XrFrameState.predictedDisplayPeriod` | Target frame budget reported by the runtime. ~11.11 ms @ 90 Hz, ~8.33 ms @ 120 Hz, ~13.89 ms @ 72 Hz. Constant for a given session. |
| `headroom_pct` | `(1 − (frame_total_ns − wait_block_ns) / period_ns) × 100` | **CPU % of the frame budget not spent on app CPU work this cycle.** Matches fpsVR / OpenXR Toolkit semantics. Negative ⇒ CPU-bound this frame. Falls back to `(1 − app_cpu_ns / period_ns) × 100` on the first frame where `frame_total_ns = 0`. |
| `gpu_headroom_pct` | `(1 − gpu_time_ns / period_ns) × 100` | **GPU % of the frame budget not spent on app GPU work this cycle.** Negative ⇒ GPU-bound this frame. `100.00` when `gpu_time_ns == 0` (no D3D11 binding, disjoint range invalid, or result unavailable at session end) — same default as fpsVR / OpenXR Toolkit overlays. Filter on `gpu_time_ns > 0` to exclude unmeasured rows from GPU statistics. |
| `should_render` | `XrFrameState.shouldRender` as 0/1 | Whether the runtime asked the app to render this frame. `0` means a skipped frame (focus loss, scene transition); typically filter these out for steady-state analysis. |

The file is closed with a trailing footer line (commented `#`-prefixed)
recording total written rows and per-cause dropped counts:

```
# session_end written=2024 dropped_try_lock=0 dropped_queue_full=0 dropped_disk_write=0
```

Pandas reads it transparently with `pd.read_csv(path, comment='#')`.

### Anatomy of a frame cycle

Stitched chronologically, the CPU columns cover 100% of the cycle.
The GPU column runs in parallel (the GPU is processing the app's draws
while the CPU is already moving on to the next frame's sim work):

```
CPU timeline:

xrEndFrame_prev ↓
                ├──────── post_end ────────────┐
                                               ↓
                                          xrWaitFrame ↓
                                          ├ wait_block ┤
                                                       ↓
                                              ├ pre_begin ┤
                                                          ↓ xrBeginFrame
                                              ├ render_submission ┤
                                                                  ↓ xrEndFrame ↓
                                                                  ├ end_frame ┤
                                                                              ↓
                ←──────────────────── frame_total ────────────────────────────→


GPU timeline (parallel, may overlap with the next CPU frame):

                                              xrBeginFrame ↓
                                                        ├──── gpu_time ───┤
                                                                          ↓ xrEndFrame
```

Derived segments (not stored, computed in analysis):

- `render_submission_ns = app_cpu_ns − pre_begin_ns`
  *time spent in `xrBeginFrame` → `xrEndFrame` submitting draw calls*
- `post_end_ns = frame_total_ns − wait_block_ns − pre_begin_ns − app_cpu_ns − end_frame_ns`
  *time spent OUTSIDE OpenXR calls — game simulation, physics, AI,
  event polling. The metric `app_cpu_ns` alone misses this entirely.*

### Note on GPU result latency

GPU timestamp queries are asynchronous: a query issued on the CPU at
`xrBeginFrame_N` returns its result only after the GPU has actually
processed that point of the command stream, which is typically 1-3
frames later. The layer therefore **defers each CSV row by ~3-4 frames**
so every row has `gpu_time_ns` aligned with its own `frame_index`.
Practical implications:

- The CSV always lags the live frame loop by a few frames. Invisible
  in offline analysis; just be aware if you `tail -f` the file
  during a session — the latest few frames haven't been written yet.
- Frames that are pending when `xrDestroySession` fires (or when the
  process exits) flush with `gpu_time_ns = 0` rather than vanishing.
  In analysis, treat the last ~4 rows of a session as "GPU result
  unavailable", or filter `gpu_time_ns > 0` before computing GPU
  averages.
- `gpu_time_ns` measures the **app's** GPU work between
  `xrBeginFrame` and `xrEndFrame`. The compositor / reprojection /
  warp work the runtime adds after the app submits the frame is NOT
  captured — runtimes don't expose those timings to layers in any
  portable way.

### What each timing diagnoses

| Symptom | Where to look | Reading |
|---|---|---|
| Frame drops, app feels stuttery | `frame_total_ns` > `period_ns` × 1.5 | App is missing the deadline. Then drill down into which segment dominates. |
| **CPU-bound vs GPU-bound** | Compare `headroom_pct` and `gpu_headroom_pct` averages | Whichever is **lower** is your bottleneck. If both are low and similar, you're balanced and would need to optimise both to gain headroom. If only CPU is low, simplify game logic / render submission. If only GPU is low, reduce shader complexity / resolution / pixel-shader-heavy effects. |
| Render thread heavy | `render_submission_ns` (`= app_cpu_ns − pre_begin_ns`) close to `period_ns` | Too many draw calls / state changes / GPU-queue stalls in the submission path. Profile with PIX / RenderDoc. |
| Game simulation heavy | `post_end_ns` (derived) dominates the cycle while `render_submission_ns` stays small | Physics, AI, scripting between frames is the bottleneck. Render thread is fine; the game logic thread is the problem. |
| GPU-bound | `gpu_headroom_pct` consistently < 5%, `gpu_time_ns` ≈ `period_ns` | The GPU is saturated on app work — pixel shaders, post-processing, MSAA, resolution scaling. Drop one of those before touching CPU side. |
| GPU idle but CPU busy | `gpu_headroom_pct` > 60% while `headroom_pct` < 20% | Classic "app cannot feed the GPU fast enough" — usually render-thread-bound. Look at `render_submission_ns`. |
| Runtime overhead | `end_frame_ns` consistently > ~1 ms | The runtime / compositor itself is slow ingesting frames. Switching runtimes (SteamVR ↔ Oculus ↔ vendor) can move the needle. Young runtimes are typical culprits. |
| Compositor underused | `wait_block_ns` close to `period_ns`, `frame_total_ns` ≈ `period_ns` | Healthy. The app finishes early, runtime throttles, equilibrium. The opposite (small `wait_block_ns`) means the app is keeping up only just. |
| GPU not measured | `gpu_time_ns == 0` for every row, but the game uses Vulkan / OpenGL / D3D12 | Expected: V2.0 of the layer only instruments D3D11 (and only directly — D3D12-via-D3D11On12 is a V2.1 follow-up). `gpu_headroom_pct` reads as `100.00` by default in this case (same as OXRT's overlay convention) — that's the formula's natural value with `gpu_time_ns=0`, not a real "GPU is idle" reading. The CPU columns remain accurate; just filter `gpu_time_ns > 0` if you want to exclude these rows from GPU stats. |

### Relationship to other VR frame-analysis tools

**OpenXR-side tools — semantically equivalent to this layer.** Same
definitions of App CPU / wait block / headroom; this layer just stores
per-frame to CSV instead of overlaying or aggregating.

- [`xrframetools`](https://github.com/mbucchia/XR-FrameTools) (mbucchia)
  — CLI that captures OpenXR timings via ETW and writes CSV. Closest
  conceptual sibling to this layer. Columns line up: their `App CPU`
  ≈ our `frame_total_ns − wait_block_ns`, their `App GPU` is the
  V2 work we haven't done yet, their `Compositor` ≈ our
  `wait_block_ns` + `end_frame_ns`.
- [**OpenXR Toolkit**](https://github.com/mbucchia/OpenXR-Toolkit)
  (mbucchia) — live overlay. Its `appCpuTimeUs` is sampled between two
  consecutive `xrWaitFrame` entries excluding the wait block (we sample
  between two consecutive `xrEndFrame` entries, same semantics in
  steady state). OXRT averages over a 1 s window; we store raw
  per-frame, jitter ±2-3 pp. Match the overlay number with a rolling
  mean of 90 rows:

  ```python
  import pandas as pd
  df = pd.read_csv(path, comment='#').iloc[30:]   # skip warmup
  df['headroom_smooth'] = df['headroom_pct'].rolling(90).mean()
  ```

  OXRT's column-by-column mapping: `appCpuTimeUs` ≈
  `frame_total_ns − wait_block_ns`, `renderCpuTimeUs` ≈
  `app_cpu_ns − pre_begin_ns`, `endFrameCpuTimeUs` ≈ `end_frame_ns`,
  `waitCpuTimeUs` ≈ `wait_block_ns`, `appGpuTimeUs` ≈ `gpu_time_ns`
  (both measure the app's `xrBeginFrame → xrEndFrame` GPU window via
  D3D11 timestamp queries).

- [**fpsVR**](https://store.steampowered.com/app/908520/fpsVR/) — paid
  Steam overlay, SteamVR-only. Different storage strategy, same idea
  of CPU vs GPU headroom relative to the predicted display period.

**Complementary / different layer of the stack.** Worth knowing they
exist; not aliases for what this layer does.

- [**PresentMon**](https://github.com/GameTechDev/PresentMon) (Intel)
  — captures DXGI/D3DKMT *Present* events via ETW, not OpenXR.
  Sees `Present()` calls regardless of API (2D, VR, doesn't matter),
  gives you actual present-to-display latency, GPU busy time, tearing
  mode. Doesn't know about `xrWaitFrame` / `xrEndFrame` directly. Use
  alongside this layer if you want to correlate compositor handoff
  with the actual flip — they capture orthogonal layers of the stack.
- **GPUView** — lower-level still: DirectX scheduler queue visualised
  per-context. Diagnoses GPU stalls / context contention that look
  like CPU time from above.

### Sanity checks on a fresh capture

After a 10-20 s session, opening the CSV should show:

- `period_ns` constant for all rows, equal to `1e9 / target_refresh_rate`.
- `frame_total_ns` ≈ `period_ns` in steady state (after first ~30 warmup frames).
- `frame_total_ns ≈ wait_block_ns + pre_begin_ns + app_cpu_ns + end_frame_ns + post_end_ns`
  for any row (within ~100 ns of rounding). Useful invariant check on the
  data integrity.
- `headroom_pct` aligned with what fpsVR / OXRT overlay displayed at
  the same moment (rolling-mean of 90 rows).
- For a D3D11 app, `gpu_time_ns > 0` for every row past the first ~4
  frames (the warmup window the layer needs to drain its query ring).
  For Vulkan / OpenGL / D3D12 apps, `gpu_time_ns == 0` for every row
  — that's expected, the layer just doesn't instrument those graphics
  APIs yet. CPU columns remain accurate.
- `gpu_headroom_pct` aligned with what fpsVR / OXRT overlay reports as
  "GPU headroom" (also rolling-mean 90 rows; sampling noise per frame
  is larger on GPU than CPU because the GPU schedules work in larger
  chunks).

A CSV with `wait_block_ns ≈ 0` and `headroom_pct < 50%` for sustained
stretches is a hot scene worth investigating; if `post_end_ns` is the
dominant segment there, your game logic thread is what needs profiling
— not the renderer. If both `headroom_pct` and `gpu_headroom_pct` are
< 20% on the same frames, the workload is balanced at the limit and
you need to cut on both sides; if only one is low, that's where to
focus.

## CI / signing setup

Skip this section if you don't care about signed releases — the CI
will build and produce unsigned artifacts just fine without any
secrets. The signing steps fall through cleanly on PR builds, forks,
and any tag push made without the secrets configured.

If you DO want signed releases:

1. Get a code signing certificate. The cheapest legitimate option for
   open-source projects is [Certum Open Source Code Signing Cloud](https://shop.certum.eu/en/open-source-code-signing-in-the-cloud-1-year.html)
   (~25 €/yr at time of writing).
2. Enroll the 2FA in the SimplySign portal. During the QR-code step,
   save the Base32 **seed** (the otpauth URI's `secret=` parameter) —
   not the current 6-digit code. You need the seed for CI.
3. Add three GitHub repo Secrets (Settings → Secrets and variables →
   Actions):
   - `CERTUM_USERNAME` — your SimplySign portal email
   - `CERTUM_TOTP_SEED` — the Base32 seed from step 2
   - `CERTUM_CERT_THUMBPRINT` — 40-hex-char SHA-1 of the issued cert
4. Tag a release (`git tag v0.0.1 && git push origin v0.0.1`). The
   workflow signs the DLL + Setup.exe and attaches everything to a
   GitHub Release automatically.

[`docs/DEVELOPMENT.md`](./docs/DEVELOPMENT.md) has the full write-up
on how the signing pipeline works — including why we use
`SimplySignDesktop /autologin` and what to do when Certum updates the
desktop client.

## Test loop

`openxr-api-layer-tests` builds alongside the DLL and runs after every
build (the workflow fails on a non-zero test exit). Add your own tests
under `openxr-api-layer-tests/test_*.cpp` and list them in the test
project's `<ClCompile>` items.

Two patterns to know:

- **Unit tests on pure helpers** — just `#include` the header and
  `CHECK(...)`. No OpenXR types involved. Fast.
- **Integration tests via `mock_runtime`** — drives `OpenXrLayer`
  through `xrCreateInstance` ↔ `xrEndFrame` with a fake runtime that
  produces deterministic FOV / pose data. No GPU, no HMD, no loader.

`test_example.cpp` shows the minimal doctest pattern and has a
commented-out integration example.

## Releasing

```powershell
git tag v0.1.0
git push origin v0.1.0
```

The workflow handles the rest:

1. Builds Debug + Release x64 in parallel
2. Runs the test binary; non-zero exit fails the job
3. (Release only) Builds the Inno Setup installer
4. (Tag push only) Signs the DLL + installer if the Certum secrets
   are set
5. (Tag push only) Creates a GitHub Release with both ZIPs + the
   Setup.exe attached

Builds on non-tag pushes (main, PRs, manual `workflow_dispatch`)
produce unsigned verification artifacts and don't create a release.

## Conventions inherited from this template

These are decisions baked into the framework — feel free to override
them if your layer has different needs, but they're the defaults:

- **HKLM registration**, not HKCU (anti-cheat and OpenXR Tools for WMR
  expect this — see the upstream `docs/openxr_api_layers_best_practices.md`
  rule 2)
- **Settings file per OpenXR application** at
  `%LOCALAPPDATA%\<your-layer-name>\<app>_settings.json`, plus a
  global `settings.json` template
- **Log file per application** at
  `%LOCALAPPDATA%\<your-layer-name>\<app>.log`
- **Graceful degradation, never crash** — every overridden method
  should check `m_bypassApiLayer` and forward to the runtime if your
  feature can't run safely for this context
- **Delay-loaded D3D DLLs** so the layer doesn't bloat the process
  load image of Vulkan-only or D3D12-only games that don't trigger
  your graphics path

## License

MIT License — see [LICENSE](./LICENSE).

The framework code (`openxr-api-layer/framework/`, dispatch generator,
`module.def`, entry point, logging helpers) is the work of Matthieu
Bucchianeri (`mbucchia`), Copyright © 2022-2023. Everything else is
new in this template. If you fork this template, your own work goes
under your name in the LICENSE; please keep mbucchia's attribution
intact.
