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
