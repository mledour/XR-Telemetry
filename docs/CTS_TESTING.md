# CTS testing

Playbook for running the [OpenXR Conformance Test Suite][cts-repo]
against your layer. The goal is to catch any case where the layer
would make a conformant runtime look non-conformant to an application
— a hard requirement for any OpenXR API layer. See also the
[OpenXR Loader specification][loader-spec] for the layer-loader
contract this relies on.

Everything in this document is **manual** for now. The in-repo PR gate
(mock runtime + doctest) covers the contract surface cheaply on every
push. The CTS complements it by catching the things the mock cannot
see: real loader behaviour, real asymmetric FOVs, real swapchains,
and a real compositor.

## When to run what

| Moment | Target | Tool |
|---|---|---|
| Every PR | Contract-level regressions | Mock runtime tests (CI, automatic) |
| Pre-release (before a `v*.*.*` tag) | Full conformance on the runtime your users will run | **Your primary target runtime** (see below) |
| Pre-release, if a failure is ambiguous | Is the failure my layer or a runtime quirk? | A second runtime for triangulation |
| Iterating without an HMD powered on | Quick sanity | SteamVR + null driver (optional) |
| Conformance submission (hypothetical) | Full matrix | Multiple runtimes × multiple graphics plugins |

**Your primary release gate** is the runtime your users actually run.
For sim-racing on Pimax that's *Pimax native OpenXR*; for Quest users
that's *Meta Link*; for a broad SteamVR audience it's *SteamVR
OpenXR*; for WMR users it's *Microsoft OpenXR* (less common now that
WMR is sunsetted). Pick one as your release gate and stick to it —
adding a second runtime is only worth it when you can't tell whether
a CTS failure is in your layer or in the runtime.

## Get the CTS binary

### Prebuilt (recommended)

Khronos publishes tagged releases:

- https://github.com/KhronosGroup/OpenXR-CTS/releases

Pick the latest `openxr-cts-*-win64.zip` that matches the
`XR_CURRENT_API_VERSION` of this layer's SDK submodule (currently
1.1.x). The archive contains `conformance_cli.exe` and
`conformance_test.exe` plus their graphics-plugin DLLs
(`conformance_test_D3D11.dll`, `conformance_test_D3D12.dll`,
`conformance_test_Vulkan2.dll`, `conformance_test_OpenGL.dll`).

Extract somewhere permanent (e.g. `C:\OpenXR-CTS\`). The CTS does not
install itself into any system path.

### Build from source

Only worth it if you need to patch the CTS or run a non-released
version:

```powershell
git clone --recursive https://github.com/KhronosGroup/OpenXR-CTS
cd OpenXR-CTS
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release -j
```

Output lands in `build\src\conformance\conformance_test\Release\`.

## Runtime setup

Only one OpenXR runtime can be "active" at a time — the CTS talks to
whichever runtime is currently registered under
`HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`. The three
configurations below cover progressively-broader needs; most
releases only need (1).

### 1. Your primary target runtime (release gate)

This is whatever runtime ships in front of the users you actually
care about. Real GPU path, real compositor, real per-eye FOV — a
green CTS here is the only runtime signal that matters for a release
tag.

**Setup** depends on the runtime; the common shape is:

1. Launch the runtime's configuration tool (Pimax Client, SteamVR,
   Meta XR Simulator, Microsoft Mixed Reality Portal, Varjo Base,
   etc.). Confirm the HMD is detected and tracking.
2. Set that runtime as the OpenXR active runtime. Each vendor exposes
   its own toggle:
   - **Pimax**: Pimax Client → Settings → "Set Pimax OpenXR Runtime as active"
   - **SteamVR**: SteamVR → Settings → Developer → "Set SteamVR as OpenXR Runtime"
   - **Meta**: Oculus app → Settings → Beta → "OpenXR Runtime: Oculus"
   - **WMR**: Mixed Reality Portal → Settings → "Set OpenXR Runtime to Windows"
   - **Varjo**: Varjo Base → System → "Set Varjo as OpenXR Runtime"
3. Verify:
   ```powershell
   Get-ItemProperty "HKLM:\SOFTWARE\Khronos\OpenXR\1" ActiveRuntime
   # Must point to YOUR target runtime's JSON.
   ```
4. Close other VR clients to avoid them stealing the runtime on the
   next launch — some configurators rewrite the registry key when
   they open.

**Asymmetric FOV note (Pimax-specific but worth flagging).** Pimax
HMDs default to "parallel projection ON" in Pimax Client, which
hides the native canted asymmetric FOV behind a symmetric emulation.
If you test with this on, your layer's interaction with asymmetric
FOVs is never exercised — turn it OFF in Display/Advanced before
running the CTS. Other runtimes don't have this knob.

**Sanity check**

Wake the HMD by setting it face-up on your desk so the proximity
sensor and IMU come alive. No need to wear it — the mirror window
shows what the CTS renders.

```powershell
cd C:\OpenXR-CTS
.\hello_xr.exe -G D3D11
```

A mirror window with a rendered scene (cube on dark background)
should open and keep running. If `hello_xr` crashes or exits with
`XR_ERROR_RUNTIME_UNAVAILABLE`, fix this before touching the CTS —
no amount of CTS output will be interpretable on a broken runtime.

### 2. A second runtime (optional triangulation)

**When to install this**: you hit a CTS failure on (1) and can't
tell whether your layer or the runtime is at fault. A well-known
mature runtime (SteamVR is the usual choice — heavily battle-tested,
runs on most HMDs via SteamVR drivers) is a good triangulation
target. A test that passes under SteamVR but fails under (1) points
at the runtime, not your layer; the inverse points at your layer.

**Setup** is the same as (1) but flipping the runtime toggle to the
chosen second target. Run the CTS the same way; the output XMLs are
directly comparable to (1).

Don't edit the registry by hand — every vendor's runtime configurator
rewrites the key on launch otherwise.

### 3. SteamVR with the null driver (optional, no-HMD)

**When to install this**: you want to iterate on a CTS-surfaced bug
without powering an HMD on, or you're onboarding a contributor who
doesn't own one.

**Cost**: free, no HMD required.
**Coverage**: contract-level correctness — enumeration, handle
lifetimes, error codes, instance/session lifecycle, extension
interaction.
**Blind spot**: the null driver reports symmetric FOVs and does not
exercise a real GPU pipeline. Any bug that only shows up on
asymmetric FOVs will pass here silently. Never substitute this for
(1) on a release gate.

**Setup**

1. Install Steam and SteamVR from the Steam client.
2. Edit
   `"%ProgramFiles(x86)%\Steam\steamapps\common\SteamVR\drivers\null\resources\settings\default.vrsettings"`:
   - Set `"enable": true` in the `driver_null` section.
3. Edit
   `"%ProgramFiles(x86)%\Steam\steamapps\common\SteamVR\resources\settings\default.vrsettings"`:
   - Set `"activateMultipleDrivers": true` and
     `"forcedDriver": "null"` in the `steamvr` section.
4. Launch SteamVR once. Ignore the "no HMD detected" warning — the
   null driver exposes a virtual head-mounted system that satisfies
   the OpenXR runtime.
5. SteamVR → Settings → Developer → **Set SteamVR as OpenXR
   Runtime**.
6. Sanity-check with `hello_xr.exe -G D3D11` as above.

## Install or disable the layer

Unlike install/uninstall cycles (which need admin elevation), the
OpenXR loader honours the `disable_environment` field of the JSON
manifest. For this layer the variable is:

```
DISABLE_XR_APILAYER_NOVENDOR_template=1
```

Install the layer **once** in an elevated PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-Layer.ps1
```

Then toggle with the env var per-shell:

```powershell
# Baseline: pretend the layer isn't there.
$env:DISABLE_XR_APILAYER_NOVENDOR_template = "1"
# ... run CTS, capture baseline.xml ...

# With the layer:
Remove-Item Env:\DISABLE_XR_APILAYER_NOVENDOR_template
# ... run CTS, capture with_layer.xml ...
```

This is the reliable way to produce two directly-comparable runs in
the same session with the same runtime.

## The canonical CTS invocation

```powershell
cd C:\OpenXR-CTS
.\conformance_cli.exe `
  -G D3D11 `
  --reporter "junit::out=baseline.xml" `
  "~[interactive]"
```

Flag-by-flag (cross-checked against CTS 1.0.34 `--help`):

- `-G D3D11` — graphics plugin. **Always run D3D11 first** — it is
  the most robust. Once it passes, repeat for `D3D12` and `Vulkan2`.
  Long form is `--graphicsPlugin D3D11`.
- `--reporter "junit::out=FILE.xml"` — machine-readable XML output
  (Catch2 v3 `name::key=value` syntax). Keep this for the diff.
- `"~[interactive]"` — **critical**. Excludes the Catch2 tag
  `[interactive]`, which covers tests that require a human to put
  the HMD on and move it. Without this filter the CTS will appear
  to hang on the first interactive test.

The CTS does NOT accept an `--apiVersion` flag — the API version is
determined from the binary itself. Adding one makes the parser
reject everything that follows, and you end up with empty XMLs that
the diff script will happily report as "no regressions" unless you
have the `Test-Path` guard (see below).

Before the first real run, sanity-check the argument line with
`--list-tests` (runs nothing, just prints the inventory):

```powershell
.\conformance_cli.exe -G D3D11 --list-tests
```

If that produces a clean list, your flags are accepted.

Expected runtime on `~[interactive]`: ~30–60 min on a real HMD,
~20–40 min on SteamVR null.

## The golden rule: diff baseline vs with-layer

**Never judge a CTS run in isolation.** Every runtime + graphics
plugin combination produces a handful of pre-existing failures
(every runtime has its known quirks; the null driver skips things).
A single run with failures tells you nothing. The only defensible
signal is: *did this layer cause tests that passed without it to
now fail with it?*

Always run the pair, back-to-back, on the same runtime, in the same
session.

```powershell
# Baseline
$env:DISABLE_XR_APILAYER_NOVENDOR_template = "1"
.\conformance_cli.exe -G D3D11 `
    --reporter "junit::out=baseline.xml" "~[interactive]"

# With layer
Remove-Item Env:\DISABLE_XR_APILAYER_NOVENDOR_template
.\conformance_cli.exe -G D3D11 `
    --reporter "junit::out=with_layer.xml" "~[interactive]"

# Guard: fail loudly if the CTS didn't produce output. Without this,
# Compare-Object on two null inputs reports "no regressions" and you
# would ship thinking the CTS passed.
if (-not (Test-Path baseline.xml)) {
    throw "baseline.xml missing — CTS did not run. Check arguments."
}
if (-not (Test-Path with_layer.xml)) {
    throw "with_layer.xml missing — CTS did not run. Check arguments."
}

# Diff: which testcases FAILED with the layer but not in the baseline?
$base = Select-Xml -Path baseline.xml   -XPath "//testcase[failure]" |
        ForEach-Object { $_.Node.name } | Sort-Object
$with = Select-Xml -Path with_layer.xml -XPath "//testcase[failure]" |
        ForEach-Object { $_.Node.name } | Sort-Object
$regressions = Compare-Object $base $with -IncludeEqual |
               Where-Object SideIndicator -eq '=>' |
               ForEach-Object InputObject

if ($regressions) {
    "REGRESSIONS (failed only with the layer installed):"
    $regressions
    throw "CTS regressed under layer — do not tag."
} else {
    "No regressions. Layer does not break tests the runtime already passes."
}
```

Save this as `scripts\Run-CTS-Diff.ps1` if you end up running it
often.

## Tests to watch closely for your layer

Which Catch2 tags matter most depends on which OpenXR functions your
layer overrides. Mapping from `framework/layer_apis.py`'s
`override_functions` to Catch2 tags:

| You override | CTS tag to watch | Why |
|---|---|---|
| `xrCreateInstance` | `[xrCreateInstance]`, `[instance]` | Loader/layer negotiation, extension contracts. |
| `xrGetSystem` / `xrGetSystemProperties` | `[xrGetSystem]`, `[system]` | If you fake or override HMD identity. |
| `xrEnumerateViewConfigurationViews` | `[xrEnumerateViewConfigurationViews]` | Two-call enumeration contract — counts must match between probe and real call. |
| `xrLocateViews` | `[xrLocateViews]` | Cross-call consistency, asymmetric-FOV handling, sign convention preservation. |
| `xrEndFrame` | `[xrEndFrame]`, `[XR_KHR_composition_layer_*]` | Composition layer validation, sub-image rects, projection-view FOVs. |
| `xrCreateSwapchain` / `xrDestroySwapchain` | `[xrCreateSwapchain]`, `[swapchain]` | Format/size/usage flag contracts. |
| Anything touching reference spaces | `[xrCreateReferenceSpace]`, `[space]` | Pose math, time-extrapolation. |

If your layer rewrites a function the CTS exercises heavily,
`--filter "[xrLocateViews]"` (or whichever tag) is a fast smoke
test before doing the full run.

## Visual sanity check

After any CTS-passing run, spend 30 seconds on `hello_xr` **on the
HMD**, with your layer installed and a configuration that exercises
its most visible behavior. The CTS cannot catch:

- Visible head-tilt deformation or wrong-eye geometry.
- Jitter or swimming at the edges of any rendered area you mutate.
- Asymmetry that's mathematically correct but visually wrong (e.g.
  on a Pimax with parallel projection OFF, results that look
  mirror-symmetric across the eyes when the underlying FOV isn't).
- Stale composition state across `xrEndFrame` calls.

Anything warped, shifted, flickering after the CTS reports green is
a bug the CTS missed.

## Troubleshooting

**Runtime refuses to load the layer.**
Some runtimes cache the layer list at install/launch time. Reboot
the runtime's configurator (Pimax Client, SteamVR, Oculus app, etc.)
after `Install-Layer.ps1` and try again. Some vendor configurators
have an "Enable API layers" toggle worth checking.

**HMD shows in standby during the CTS run.**
Wake it by moving the HMD — many models gate the display on
proximity / IMU motion. Once it's reported live by the configurator,
rerun the CTS; the standby run is noise.

**`conformance_cli.exe` hangs on a specific test.**
Probably an `[interactive]` test that slipped past your filter.
Check that `"~[interactive]"` is quoted — PowerShell eats the tilde
otherwise. If it still hangs, filter to the category that's hanging:
`"[xrLocateViews]"` etc.

**Every test reports SKIPPED.**
Either no OpenXR runtime is active
(`Get-ItemProperty HKLM:\SOFTWARE\Khronos\OpenXR\1 ActiveRuntime`)
or you specified a graphics plugin (`-G`) whose DLL is missing from
the CTS directory.

**"API layer is not present" from the CTS startup log.**
The layer was disabled via env var or never installed. Verify with:
```powershell
Get-ItemProperty "HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit\*"
```
Re-run `Install-Layer.ps1` from an elevated PowerShell if the output
is empty.

**The diff script lists a test as a "regression" but manually running
it in isolation passes.**
Catch2 tests share state across a run. The regression is real but
its root cause might be a preceding test that the layer mutates
global state for. Re-run with `--order rand --rng-seed N` and
bisect with `--start-at TESTNAME`.

## Release checklist

Before pushing a `v*.*.*` tag:

1. [ ] Mock-runtime tests green on the PR branch's latest CI run.
2. [ ] **CTS diff empty on your primary target runtime**, D3D11.
       This is the required gate.
3. [ ] `hello_xr` on the HMD with a representative config looks
       right (no deformation, no jitter, behavior matches what the
       layer is supposed to do).
4. [ ] (If a diff surfaced a failure you can't pin down): re-run
       under a second runtime to triangulate before either fixing
       the layer or filing a runtime bug.
5. [ ] Attach `baseline.xml` + `with_layer.xml` to the GitHub
       release notes for traceability.

If step 2 fails, do not tag. Either fix the layer or — if the
failure is pre-existing in the runtime — document it in the release
notes as a known quirk and get a second opinion before shipping.

[cts-repo]: https://github.com/KhronosGroup/OpenXR-CTS
[loader-spec]: https://registry.khronos.org/OpenXR/specs/1.0/loader.html
