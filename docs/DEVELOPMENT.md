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
  layer.cpp / layer.h               ← YOUR layer logic
  pch.h                             ← D3D11 + D3D12 includes (delay-loaded)
  module.def                        ← DLL export list
  *.json                            ← OpenXR loader manifests

openxr-api-layer-tests/
  main.cpp                          ← doctest driver
  mock_runtime.{h,cpp}              ← in-process OpenXR fake
  test_stubs.cpp                    ← entry.cpp substitutes for the test build
  test_*.cpp                        ← YOUR tests

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
2. **`<<AUTHOR_GITHUB_HANDLE>>/OpenXR-Layer-Template`** (this repo) —
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

Then open `XR_APILAYER_NOVENDOR_template.sln` (real names
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

## Releases

The GitHub Actions workflow
([`build-and-release.yml`](../.github/workflows/build-and-release.yml))
builds `Release` and `Debug` x64 on every push to `main` (as a sanity
check), every PR, and every `v*.*.*` tag. On a tag push it
additionally creates a GitHub Release and attaches:

- `XR_APILAYER_NOVENDOR_template-<version>-x64-Setup.exe` —
  Inno Setup installer (recommended for end users)
- `XR_APILAYER_NOVENDOR_template-Release-x64.zip` — raw DLL +
  JSON + PowerShell scripts, for manual installation or development
- `XR_APILAYER_NOVENDOR_template-Debug-x64.zip` — debug build
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
