# Sign-Artifact.ps1
#
# Code-signs a Windows binary (DLL or EXE) using a Certum SimplySign Cloud
# certificate, headlessly, from CI.
#
# How this works
# --------------
# SimplySignDesktop.exe ships an undocumented headless login mode:
#
#     SimplySignDesktop.exe /autologin <username> <totp>
#
# Discovered via ILSpy by an anonymous commenter on
# https://www.devas.life/how-to-automate-signing-your-windows-app-with-certum/.
# Certum does not document this flag anywhere, but it's been verified to
# work on non-interactive Windows agents — exactly the GitHub Actions
# runner environment, where the Session-1 desktop exists but no human
# is around to click on a tray icon.
#
# After /autologin succeeds, SimplySignDesktop runs as a background
# process (no UI, no tray icon) acting as the bridge between Certum's
# cloud HSM and the Windows certificate store. Once the cert appears in
# CurrentUser\My, signtool.exe with /sha1 <thumbprint> can sign as
# normal.
#
# The flow:
#   1. Stop any leftover SimplySignDesktop.exe (graceful /close, then
#      kill if needed). A stale session blocks the new /autologin.
#   2. For each TOTP drift offset in [0, -1, +1] (current 30s window,
#      then previous, then next): generate that OTP and run /autologin.
#      A successful login leaves the process running; a failed login
#      makes it exit within ~3 s. We detect that and try the next
#      offset. This handles ±30 s clock drift between the runner and
#      Certum's TOTP server without sacrificing security (each window
#      is still 30 s wide, the only widening is in WHICH window we try).
#   3. Poll Cert:\CurrentUser\My for the configured thumbprint to
#      confirm the cloud HSM session is live and the cert is bridged.
#   4. signtool sign /sha1 /tr http://time.certum.pl /td sha256
#      /fd sha256 + signtool verify /pa for each input file.
#   5. Graceful /close to leave the runner clean.
#
# Required GitHub Secrets (mapped into env vars by the workflow):
#   CERTUM_USERNAME         — SimplySign portal email
#   CERTUM_TOTP_SEED        — Base32 seed from "Show secret key" (the
#                             same string in the otpauth:// URI's
#                             secret= parameter)
#   CERTUM_CERT_THUMBPRINT  — 40-hex-char SHA-1 of the issued certificate
#                             (no spaces). Find it once with:
#                               Get-ChildItem Cert:\CurrentUser\My |
#                                   Where-Object Subject -Match 'Le ?[Dd]our' |
#                                   Format-List Thumbprint, Subject
#
# Certum SimplySign uses 2FA where the TOTP IS the second factor — no
# separate static password is required.
#
# This script intentionally does NOT echo credentials into logs. The OTP
# is short-lived (30 s window); the username is in the secret store; the
# thumbprint is technically public anyway (it's in every signed binary).

[CmdletBinding()]
param(
    # One or more files to sign. Globs allowed.
    [Parameter(Mandatory = $true, Position = 0)]
    [string[]] $Path,

    # SimplySignDesktop.exe location. The 64-bit MSI installs to
    # `C:\Program Files\Certum\SimplySign Desktop\` (real 64-bit app
    # — `Program Files (x86)` would be wrong). Override for self-hosted
    # runners with a non-default install layout.
    [string] $SimplySignExe = 'C:\Program Files\Certum\SimplySign Desktop\SimplySignDesktop.exe',

    # signtool.exe. The Windows SDK ships it but does NOT add it to PATH
    # by default on the GitHub Actions windows-2022 image, so we resolve
    # it by searching the SDK install paths if the default isn't on PATH.
    # Override to skip the search if you know the exact path.
    [string] $SignToolExe = 'signtool.exe',

    # Description embedded in the signature ("More info" line in the UAC
    # prompt).
    [string] $Description = 'XR_APILAYER_NOVENDOR_template',

    # How many seconds to give /autologin before deciding it failed.
    # A bad OTP / bad username makes the process exit within ~1-2 s in
    # practice; we wait 5 s to be safe.
    [int] $AutoLoginProbeS = 5,

    # How long to poll Cert:\CurrentUser\My for the thumbprint after a
    # successful /autologin before giving up.
    [int] $CertAppearTotalS = 30
)

$ErrorActionPreference = 'Stop'

function Require-Env {
    param([string] $Name)
    $val = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($val)) {
        throw "Required environment variable '$Name' is empty. Add it as a GitHub Secret and map it in the workflow."
    }
    return $val
}

# Stop any running SimplySignDesktop, gracefully if possible. A leftover
# instance from an earlier (failed) run blocks the new /autologin —
# Certum allows only one cloud session per host.
function Stop-SimplySignDesktop {
    param([string] $ExePath)
    $existing = @(Get-Process -Name 'SimplySignDesktop' -ErrorAction SilentlyContinue)
    if ($existing.Count -eq 0) { return }

    Write-Host ("Stopping {0} existing SimplySignDesktop process(es) ..." -f $existing.Count)
    # /close asks the app to shut down its session cleanly. It returns
    # immediately; the process exits asynchronously. We then wait for
    # the previous instances to actually go away, falling back to Kill.
    try {
        Start-Process -FilePath $ExePath -ArgumentList '/close' -Wait -ErrorAction SilentlyContinue
    } catch {
        # /close on a non-running app may itself error. Ignore.
    }
    foreach ($p in $existing) {
        try {
            if (-not $p.WaitForExit(5000)) {
                Write-Host ("  PID {0} did not exit on /close; killing." -f $p.Id)
                $p.Kill()
                [void]$p.WaitForExit(2000)
            }
        } catch {
            # Already dead — fine.
        }
    }
}

# Resolve signtool.exe to a usable absolute path. Tries (in order):
#   1. The -SignToolExe parameter as-given (might be a full path or
#      something already on PATH).
#   2. Get-Command — picks it up from PATH if any caller added it.
#   3. The Windows SDK install tree under
#      `C:\Program Files (x86)\Windows Kits\10\bin\<sdk-ver>\x64\`,
#      preferring the highest SDK version available.
# Throws if none of these work, with the search paths shown.
function Resolve-SignTool {
    param([string] $Hint)

    if ([System.IO.Path]::IsPathRooted($Hint) -and (Test-Path $Hint)) {
        return (Resolve-Path $Hint).Path
    }
    $cmd = Get-Command $Hint -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    # Search the SDK. Pattern matches multiple SDK versions installed
    # side-by-side (10.0.19041.0, 10.0.22621.0, etc.) and picks the
    # latest by lexicographic sort of the version segment, which is
    # equivalent to numeric for SDK versions.
    $sdkRoots = @(
        'C:\Program Files (x86)\Windows Kits\10\bin',
        'C:\Program Files\Windows Kits\10\bin'
    )
    $candidates = foreach ($root in $sdkRoots) {
        if (Test-Path $root) {
            Get-ChildItem -Path $root -Filter 'signtool.exe' -Recurse -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' }
        }
    }
    $best = $candidates |
        Sort-Object @{Expression = {
            # Pull the version from the path so x64 + version sort numeric-ish.
            if ($_.FullName -match '\\bin\\(\d+(?:\.\d+){2,3})\\') { $matches[1] } else { '0' }
        }} -Descending |
        Select-Object -First 1
    if ($best) { return $best.FullName }

    throw "Could not find signtool.exe. Looked in: PATH, '$Hint', $($sdkRoots -join '; '). Install the Windows 10 SDK or pass -SignToolExe with the full path."
}

# Try a single /autologin attempt with a specific TOTP value. Returns
# $true if the process is still running after $AutoLoginProbeS seconds
# (the success signal — bad creds make it exit fast). Returns $false
# otherwise, after which the caller should try the next drift offset.
function Try-Autologin {
    param(
        [string] $ExePath,
        [string] $Username,
        [string] $Totp
    )
    # ProcessStartInfo with ArgumentList passes the OTP as a *single
    # argument* without going through cmd.exe's parser, which keeps it
    # out of the runner's command-line transcript that ps/Tasklist would
    # show.
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $ExePath
    $psi.ArgumentList.Add('/autologin')
    $psi.ArgumentList.Add($Username)
    $psi.ArgumentList.Add($Totp)
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow  = $true

    $proc = [System.Diagnostics.Process]::Start($psi)
    Write-Host ("Started SimplySignDesktop /autologin (PID={0}); probing for {1}s ..." -f $proc.Id, $AutoLoginProbeS)
    Start-Sleep -Seconds $AutoLoginProbeS
    $proc.Refresh()
    if ($proc.HasExited) {
        Write-Host ("  exited with code {0} — login rejected." -f $proc.ExitCode)
        return $false
    }
    Write-Host '  still running — login accepted.'
    return $true
}

# --- Resolve inputs -------------------------------------------------------
$username   = Require-Env 'CERTUM_USERNAME'
$totpSeed   = Require-Env 'CERTUM_TOTP_SEED'
$thumbprint = (Require-Env 'CERTUM_CERT_THUMBPRINT') -replace '\s', ''

if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
    throw "CERTUM_CERT_THUMBPRINT must be 40 hex characters (SHA-1 of the cert). Got length $($thumbprint.Length)."
}
if (-not (Test-Path $SimplySignExe)) {
    throw "SimplySign Desktop not found at '$SimplySignExe'. Install it on the runner first (see workflow's 'Install SimplySign Desktop' step)."
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$gen       = Join-Path $scriptDir 'Get-CertumTotp.ps1'

# --- Login ----------------------------------------------------------------
# Clean slate first.
Stop-SimplySignDesktop -ExePath $SimplySignExe

# Try the current 30 s window, then the previous, then the next. Each
# offset is +/- one period (30 s). We never go further: a clock that's
# off by >30 s from Certum's NTP-disciplined server is broken in a way
# that needs investigation, not silent compensation.
$now    = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$period = 30
$loggedIn = $false
foreach ($driftSteps in @(0, -1, 1)) {
    $time = $now + ($driftSteps * $period)
    Write-Host ("Trying TOTP at drift offset {0:+0;-0;0} step ..." -f $driftSteps)
    $totp = & $gen -Base32Seed $totpSeed -Time $time
    if ([string]::IsNullOrWhiteSpace($totp) -or $totp.Length -ne 6) {
        throw "Get-CertumTotp.ps1 produced an unexpected value (length=$($totp.Length))."
    }

    if (Try-Autologin -ExePath $SimplySignExe -Username $username -Totp $totp) {
        $loggedIn = $true
        break
    }
    # Failed — make sure we clean up before retrying with the next offset.
    Stop-SimplySignDesktop -ExePath $SimplySignExe
}
if (-not $loggedIn) {
    throw @"
SimplySignDesktop /autologin failed at all three drift offsets (0, -1, +1).
Likely causes:
  1. CERTUM_TOTP_SEED is wrong (decode mismatch — confirm it's the Base32
     string from the otpauth:// URI's secret= parameter, not the current
     6-digit code).
  2. CERTUM_USERNAME is wrong.
  3. The runner clock is off by more than 30 s from UTC.
  4. The SimplySign account has been locked (too many failed attempts).
"@
}

# --- Wait for the cert in CurrentUser\My ---------------------------------
# Belt-and-braces: the process running is one signal, the cert actually
# materialising in the Windows store is the *real* proof we can sign.
Write-Host "Waiting up to ${CertAppearTotalS}s for cert thumbprint $thumbprint to appear in CurrentUser\My ..."
$deadline = (Get-Date).AddSeconds($CertAppearTotalS)
$cert = $null
while ((Get-Date) -lt $deadline) {
    $cert = Get-ChildItem -Path 'Cert:\CurrentUser\My' -ErrorAction SilentlyContinue |
            Where-Object { $_.Thumbprint -eq $thumbprint } |
            Select-Object -First 1
    if ($null -ne $cert) { break }
    Start-Sleep -Milliseconds 1000
}
if ($null -eq $cert) {
    throw @"
Login succeeded but cert $thumbprint did not appear in CurrentUser\My within ${CertAppearTotalS}s.
Likely causes:
  1. CERTUM_CERT_THUMBPRINT is for a different (e.g. previously renewed)
     cert. Check with 'Get-ChildItem Cert:\CurrentUser\My' against the
     SimplySign portal's certificate page.
  2. The cert was revoked.
"@
}
Write-Host "Found cert: $($cert.Subject)"

# --- Resolve files and sign ---------------------------------------------
$files = @()
foreach ($p in $Path) {
    $resolved = Get-ChildItem -Path $p -ErrorAction SilentlyContinue
    if (-not $resolved) {
        throw "No files matched '$p'."
    }
    $files += $resolved
}

# Resolve once so the per-file loop doesn't pay the cost N times, and
# so a missing-signtool error fires before we waste a TOTP window.
$resolvedSignTool = Resolve-SignTool -Hint $SignToolExe
Write-Host "Using signtool at: $resolvedSignTool"

# signtool invocation matches the Certum manual prescription verbatim.
try {
    foreach ($f in $files) {
        Write-Host "Signing $($f.FullName) ..."
        & $resolvedSignTool sign `
            /sha1 $thumbprint `
            /tr   'http://time.certum.pl' `
            /td   sha256 `
            /fd   sha256 `
            /d    $Description `
            /v    `
            $f.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "signtool failed for $($f.FullName) (exit $LASTEXITCODE)."
        }

        # /pa = use the default "any" policy. Without it, signtool verify
        # defaults to driver-signing rules and rejects user-mode DLLs.
        & $resolvedSignTool verify /pa /v $f.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "signtool verify failed for $($f.FullName) (exit $LASTEXITCODE)."
        }
    }
}
finally {
    # Always tear down the session cleanly, even if signtool failed.
    # Leaving SimplySignDesktop running ties up Certum's one-session
    # quota and burns a TOTP window for the next CI run.
    Write-Host 'Closing SimplySign Desktop session ...'
    try { Stop-SimplySignDesktop -ExePath $SimplySignExe } catch { }
}

Write-Host 'All artifacts signed and verified.'
