# Test-CertumTotp.ps1
#
# Offline smoke test for Get-CertumTotp.ps1 against the RFC 6238
# Appendix B reference vectors. No network, no Certum, no real secrets
# — just confirms our HMAC-SHA256 / HMAC-SHA1 implementations match the
# RFC.
#
# Why both algorithms: Certum SimplySign uses SHA-256, but the script
# also exposes SHA-1 (RFC 6238 default) via -Algorithm. Testing both
# means a future regression that breaks dispatch is caught here, not on
# the runner during a release.
#
# Run before relying on the CI signing flow; if every check passes, the
# TOTP we feed SimplySign Desktop is correct in principle, and any auth
# failure can be blamed on credentials/clock skew/network rather than
# the math.
#
# Usage (from the repo root):
#   pwsh -File scripts\Test-CertumTotp.ps1
#
# Exit codes: 0 on success, 1 if any vector fails.

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$gen       = Join-Path $scriptDir 'Get-CertumTotp.ps1'

# RFC 6238 reference secrets (Appendix B intro). The HMAC key length
# matches the hash output, so the SHA-256 vectors use a 32-byte secret,
# not the 20-byte one used for SHA-1. Base32 of these ASCII strings
# gives the seed format Certum uses in its otpauth:// URIs.
$seedSha1   = 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ'                     # "12345678901234567890"
$seedSha256 = 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQGEZA' # "12345678901234567890123456789012"

# Vectors from RFC 6238 Appendix B. 6-digit values are the SHA-truncated
# tail of the 8-digit value (last 6 chars), which is what the script
# returns when -Digits 6 (the default).
$vectors = @(
    # SHA-256 — what Certum actually uses.
    @{ Algo='SHA256'; Seed=$seedSha256; Time=59;          Expected8='46119246' },
    @{ Algo='SHA256'; Seed=$seedSha256; Time=1111111109;  Expected8='68084774' },
    @{ Algo='SHA256'; Seed=$seedSha256; Time=1234567890;  Expected8='91819424' },
    @{ Algo='SHA256'; Seed=$seedSha256; Time=2000000000;  Expected8='90698825' },
    # SHA-1 — covers the dispatch path even if no one uses it for Certum.
    @{ Algo='SHA1';   Seed=$seedSha1;   Time=59;          Expected8='94287082' },
    @{ Algo='SHA1';   Seed=$seedSha1;   Time=1234567890;  Expected8='89005924' }
)

$failed = 0
foreach ($v in $vectors) {
    $expected6 = $v.Expected8.Substring($v.Expected8.Length - 6)
    $got8 = & $gen -Base32Seed $v.Seed -Algorithm $v.Algo -Digits 8 -Time $v.Time
    $got6 = & $gen -Base32Seed $v.Seed -Algorithm $v.Algo -Digits 6 -Time $v.Time
    $ok8  = $got8 -eq $v.Expected8
    $ok6  = $got6 -eq $expected6
    if ($ok8 -and $ok6) {
        Write-Host ("OK   {0}  T={1,10}  6={2}  8={3}" -f $v.Algo, $v.Time, $got6, $got8)
    } else {
        Write-Host ("FAIL {0}  T={1,10}  6={2} (want {3})  8={4} (want {5})" -f `
                   $v.Algo, $v.Time, $got6, $expected6, $got8, $v.Expected8)
        $failed++
    }
}

if ($failed -gt 0) {
    Write-Error "$failed of $($vectors.Count) RFC 6238 vectors failed."
    exit 1
}
Write-Host "All $($vectors.Count) RFC 6238 vectors passed."
