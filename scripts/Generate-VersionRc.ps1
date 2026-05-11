# Generate-VersionRc.ps1
#
# Produces openxr-api-layer.rc from openxr-api-layer.rc.in by substituting
# @VERSION_MAJOR@, @VERSION_MINOR@, @VERSION_PATCH@, @VERSION_STRING@, and
# @SOLUTION_NAME@.
#
# Version source, in priority order:
#   1. $env:LAYER_VERSION  (set by CI from the git tag: v1.2.3 -> 1.2.3,
#                           or a synthetic 0.0.0-dev-<sha> for non-tag pushes)
#   2. `git describe --tags --always --dirty` with the leading 'v' stripped
#   3. Literal "0.0.0-dev" if git is unavailable or has no tags
#
# The output file is rewritten only when its content actually changes so
# MSBuild's fast-up-to-date check doesn't trigger spurious rebuilds of the
# RC file on every edit.

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$TemplatePath,
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [Parameter(Mandatory=$true)][string]$SolutionName
)

$ErrorActionPreference = 'Stop'

function Resolve-Version {
    if ($env:LAYER_VERSION) {
        return $env:LAYER_VERSION
    }
    try {
        $desc = git describe --tags --always --dirty 2>$null
        if ($LASTEXITCODE -eq 0 -and $desc) {
            return ($desc -replace '^v', '')
        }
    } catch {}
    return '0.0.0-dev'
}

$fullVersion = Resolve-Version

# Parse the leading MAJOR.MINOR.PATCH. Everything after that (e.g. -rc1,
# -dev-abc1234, -dirty) is kept in $fullVersion for the string field but
# does not feed the numeric FILEVERSION tuple, which has to be 4 uint16.
$major = 0; $minor = 0; $patch = 0
if ($fullVersion -match '^(\d+)\.(\d+)\.(\d+)') {
    $major = [int]$Matches[1]
    $minor = [int]$Matches[2]
    $patch = [int]$Matches[3]
}

if (-not (Test-Path $TemplatePath)) {
    throw "Template not found: $TemplatePath"
}

# .Replace() is a literal string replacement — @VERSION_MAJOR@ etc. contain
# no regex metacharacters, but using -replace here would still be a needless
# regex roundtrip.
$content = (Get-Content -LiteralPath $TemplatePath -Raw)
$content = $content.Replace('@VERSION_MAJOR@',  $major.ToString())
$content = $content.Replace('@VERSION_MINOR@',  $minor.ToString())
$content = $content.Replace('@VERSION_PATCH@',  $patch.ToString())
$content = $content.Replace('@VERSION_STRING@', $fullVersion)
$content = $content.Replace('@SOLUTION_NAME@',  $SolutionName)

# Only write if the output has actually changed. MSBuild hashes input/output
# files for incremental build tracking; rewriting an identical file with a
# fresh mtime forces the RC compiler to re-run for no reason.
$needsWrite = $true
if (Test-Path -LiteralPath $OutputPath) {
    $existing = Get-Content -LiteralPath $OutputPath -Raw
    if ($existing -ceq $content) {
        $needsWrite = $false
    }
}

if ($needsWrite) {
    Set-Content -LiteralPath $OutputPath -Value $content -NoNewline -Encoding UTF8
    Write-Host "Generate-VersionRc: wrote $OutputPath (version=$fullVersion)"
} else {
    Write-Host "Generate-VersionRc: $OutputPath already up to date (version=$fullVersion)"
}
