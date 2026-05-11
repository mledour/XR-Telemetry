# Init-Template.ps1
#
# Post-clone substitution for OpenXR-Layer-Template. Prompts for the
# values that vary per layer (vendor tag, layer name, author name,
# year, GitHub owner), then walks the tree and replaces the
# <<PLACEHOLDER>> tokens in BOTH file contents and filenames.
#
# Run ONCE, right after `git clone` + `git submodule update --init
# --recursive`. The script is idempotent in spirit — running it again
# after a successful run is a no-op (the placeholders aren't there
# anymore) — but rerunning it after partial substitution leads to
# weirdness, so do it once and commit the result.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\scripts\Init-Template.ps1
#
# Non-interactive (useful in CI / scripted setups):
#   powershell -ExecutionPolicy Bypass -File .\scripts\Init-Template.ps1 `
#       -Vendor MLEDOUR -LayerName fov_crop -AuthorName "Michael Ledour" `
#       -Year 2026 -GitHubOwner mledour -AuthorEmail "you@example.com"
#
# Validation: vendor must be uppercase letters / digits / underscore
# only (Khronos convention); layer name must be lowercase letters /
# digits / underscore. The OpenXR loader does not enforce these
# beyond "starts with XR_APILAYER_" but every layer in the wild
# follows them.

[CmdletBinding()]
param(
    [string] $Vendor,
    [string] $LayerName,
    [string] $AuthorName,
    [string] $AuthorNameAsOnCert,  # used in release-notes verification command
    [string] $AuthorEmail,
    [string] $GitHubOwner,
    [int]    $Year = (Get-Date).Year,
    [switch] $DryRun,                # print what would change, don't write
    [switch] $NoConfirm              # skip the final confirmation prompt
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

# ---- Gather values --------------------------------------------------------

function Read-Required {
    param([string] $Prompt, [string] $Default, [string] $Regex, [string] $RegexHint)
    while ($true) {
        $hint = if ($Default) { " [$Default]" } else { '' }
        $value = Read-Host "$Prompt$hint"
        if ([string]::IsNullOrWhiteSpace($value)) { $value = $Default }
        if ([string]::IsNullOrWhiteSpace($value)) {
            Write-Host '  (required)' -ForegroundColor Yellow
            continue
        }
        if ($Regex -and ($value -notmatch $Regex)) {
            Write-Host "  must match: $RegexHint" -ForegroundColor Yellow
            continue
        }
        return $value
    }
}

if (-not $Vendor) {
    Write-Host ''
    Write-Host 'OpenXR API layer names follow the convention' -ForegroundColor Cyan
    Write-Host '    XR_APILAYER_<VENDOR>_<layer_name>'
    Write-Host 'Vendor is UPPERCASE (e.g. MLEDOUR, MBUCCHIA, NOVENDOR), layer'
    Write-Host 'name is lowercase_with_underscores (e.g. fov_crop, toolkit).'
    Write-Host ''
    $Vendor = Read-Required `
        -Prompt 'Vendor tag (UPPERCASE, letters/digits/underscore)' `
        -Default 'NOVENDOR' `
        -Regex '^[A-Z0-9_]+$' `
        -RegexHint '^[A-Z0-9_]+$'
}
if (-not $LayerName) {
    $LayerName = Read-Required `
        -Prompt 'Layer short name (lowercase_with_underscores)' `
        -Default 'my_layer' `
        -Regex '^[a-z][a-z0-9_]*$' `
        -RegexHint '^[a-z][a-z0-9_]*$ (starts with a letter)'
}
if (-not $AuthorName) {
    $AuthorName = Read-Required -Prompt 'Author name (for LICENSE and source headers)'
}
if (-not $AuthorNameAsOnCert) {
    $AuthorNameAsOnCert = Read-Required `
        -Prompt 'Author name AS PRINTED ON YOUR CODE-SIGNING CERT (Certum cap-sensitive; use Author name if you have no cert yet)' `
        -Default $AuthorName
}
if (-not $AuthorEmail) {
    $AuthorEmail = Read-Required -Prompt 'Author email (used in source-file headers only)'
}
if (-not $GitHubOwner) {
    $GitHubOwner = Read-Required `
        -Prompt 'GitHub owner / org (used in repo URL references)' `
        -Default $env:USERNAME
}
$Year = [int]$Year  # cast in case CLI passed a string

# Substitutions applied to BOTH file contents AND filenames.
#
# The fully-qualified layer-name placeholder is the literal string
# `XR_APILAYER_NOVENDOR_template` — this matches:
#   - The .sln / .json filenames the template ships with.
#   - References to those filenames in CI workflow env vars, the
#     vcxproj, scripts, docs, etc.
#   - The string baked into the DLL's LAYER_NAME preprocessor define
#     by way of $(SolutionName).
# We substitute it as a single unit so the renamed .sln, the renamed
# JSON manifests, and every reference to them stay consistent.
#
# `<<VENDOR>>` and `<<LAYER_NAME>>` are kept as separate placeholders
# for the few content spots that need vendor or layer-short-name
# standalone — e.g. the ETW trace string in layer.cpp, or a comment
# that splits them out for readability. They never appear in file
# names (which would break Windows checkout — `<` and `>` are
# reserved characters).
#
# Order matters: the longest pattern goes first so `<<AUTHOR_NAME>>`
# does not accidentally match the inside of
# `<<AUTHOR_NAME_AS_ON_CERT>>`.
$fullLayerName = "XR_APILAYER_${Vendor}_${LayerName}"
$substitutions = @(
    @{ From = 'XR_APILAYER_NOVENDOR_template'; To = $fullLayerName }
    @{ From = '<<VENDOR>>';                    To = $Vendor }
    @{ From = '<<LAYER_NAME>>';                To = $LayerName }
    @{ From = '<<AUTHOR_NAME_AS_ON_CERT>>';    To = $AuthorNameAsOnCert }
    @{ From = '<<AUTHOR_NAME>>';               To = $AuthorName }
    @{ From = '<<AUTHOR_EMAIL>>';              To = $AuthorEmail }
    @{ From = '<<AUTHOR_GITHUB_HANDLE>>';      To = $GitHubOwner }
    @{ From = '<<YEAR>>';                      To = $Year.ToString() }
)

Write-Host ''
Write-Host '--- Summary ---' -ForegroundColor Cyan
foreach ($s in $substitutions) {
    Write-Host ('  {0,-32} -> {1}' -f $s.From, $s.To)
}
Write-Host ''

if (-not $NoConfirm) {
    $ok = Read-Host 'Proceed with these substitutions? [y/N]'
    if ($ok -notmatch '^[Yy]') {
        Write-Host 'Aborted.' -ForegroundColor Yellow
        exit 1
    }
}

# ---- File enumeration ----------------------------------------------------
# We only touch text files we control — never anything under external/
# (the OpenXR submodules) or .git/, and never binary blobs.

$skipDirs = @('\\.git\\', '\\external\\', '\\packages\\', '\\bin\\', '\\obj\\', '\\stage\\')
$textExts = @(
    '.cpp', '.h', '.hpp', '.c', '.cc',
    '.json', '.def', '.config', '.vcxproj', '.filters', '.sln',
    '.yml', '.yaml',
    '.ps1', '.py', '.bat', '.cmd', '.sh',
    '.md', '.txt', '.iss', '.in', '.rc',
    '.gitignore', '.gitattributes', '.gitmodules', '.clang-format'
)

$allFiles = Get-ChildItem -Path $repoRoot -Recurse -File | Where-Object {
    $p = $_.FullName
    foreach ($skip in $skipDirs) { if ($p -match $skip) { return $false } }
    $ext = $_.Extension.ToLowerInvariant()
    if (-not $ext) {
        # Files with no extension (LICENSE, THIRD_PARTY, etc.) — include
        # if the file is plain text. Cheap heuristic: read 256 bytes,
        # reject if any NUL bytes.
        $bytes = [System.IO.File]::ReadAllBytes($p) | Select-Object -First 256
        return -not ($bytes -contains 0)
    }
    return $textExts -contains $ext
}

Write-Host ("Scanning {0} files ..." -f $allFiles.Count) -ForegroundColor Cyan

# ---- Content substitution -----------------------------------------------

$filesModified = 0
$totalReplacements = 0
foreach ($file in $allFiles) {
    $orig = [System.IO.File]::ReadAllText($file.FullName)
    $new  = $orig
    foreach ($s in $substitutions) {
        # Plain String.Replace — not Regex — so the placeholders don't
        # need escaping and we don't accidentally match anything else.
        $new = $new.Replace($s.From, $s.To)
    }
    if ($new -ne $orig) {
        $filesModified++
        # Rough count: number of removed characters per substitution
        # isn't 1:1 with replacement count, but as an indicator of
        # progress it's good enough for the run log.
        $delta = ($orig.Length - $new.Length)
        $totalReplacements += [Math]::Max(1, [Math]::Abs($delta))
        if ($DryRun) {
            Write-Host ("  would modify: {0}" -f (Resolve-Path -Relative $file.FullName))
        } else {
            [System.IO.File]::WriteAllText($file.FullName, $new)
        }
    }
}
Write-Host ("Content: {0} files modified" -f $filesModified) -ForegroundColor Green

# ---- File / directory rename --------------------------------------------
# Some files have placeholders in their NAMES too — the .sln, the
# loader JSON manifests. Rename them after the content pass so
# references inside (which we just substituted) and the file names
# both end up consistent.

function Rename-WithPlaceholders {
    param([string] $Path)
    $name = Split-Path -Leaf $Path
    $newName = $name
    foreach ($s in $substitutions) {
        $newName = $newName.Replace($s.From, $s.To)
    }
    if ($newName -eq $name) { return }
    $parent = Split-Path -Parent $Path
    $newPath = Join-Path $parent $newName
    if ($DryRun) {
        Write-Host ("  would rename: {0} -> {1}" -f $name, $newName)
    } else {
        Rename-Item -Path $Path -NewName $newName -Force
        Write-Host ("  renamed: {0} -> {1}" -f $name, $newName) -ForegroundColor Green
    }
}

# Files first (depth-first so we don't try to rename inside a dir we
# just renamed — though in practice the placeholder filenames live at
# the repo root and in openxr-api-layer/, so order is fine).
$pendingRenames = Get-ChildItem -Path $repoRoot -Recurse -File | Where-Object {
    $p = $_.FullName
    foreach ($skip in $skipDirs) { if ($p -match $skip) { return $false } }
    foreach ($s in $substitutions) { if ($_.Name.Contains($s.From)) { return $true } }
    return $false
}
foreach ($f in $pendingRenames) { Rename-WithPlaceholders -Path $f.FullName }

# Directories last (deepest first so renaming a child doesn't change
# the parent's path mid-iteration).
$pendingDirs = Get-ChildItem -Path $repoRoot -Recurse -Directory |
    Sort-Object @{ Expression = { $_.FullName.Length } } -Descending |
    Where-Object {
        $p = $_.FullName
        foreach ($skip in $skipDirs) { if ($p -match $skip) { return $false } }
        foreach ($s in $substitutions) { if ($_.Name.Contains($s.From)) { return $true } }
        return $false
    }
foreach ($d in $pendingDirs) { Rename-WithPlaceholders -Path $d.FullName }

# ---- Sanity check --------------------------------------------------------
# Anything that still contains '<<' is either a placeholder we missed
# or unrelated source code that happens to use C++ stream-output. We
# print a short report so the user can spot leftovers.

if (-not $DryRun) {
    Write-Host ''
    Write-Host 'Sanity check (any remaining placeholders that look like ours):' -ForegroundColor Cyan
    $remaining = Select-String -Path (Join-Path $repoRoot '*') -Pattern '<<[A-Z_]+>>' -Recurse 2>$null |
        Where-Object {
            $p = $_.Path
            foreach ($skip in $skipDirs) { if ($p -match $skip) { return $false } }
            return $true
        } |
        Select-Object -First 20
    if ($remaining) {
        foreach ($r in $remaining) {
            Write-Host ("  {0}:{1}: {2}" -f (Resolve-Path -Relative $r.Path), $r.LineNumber, $r.Line.Trim())
        }
        Write-Host '(if the above are real layer-template placeholders the script missed, fix them by hand.)' -ForegroundColor Yellow
    } else {
        Write-Host '  (none — all placeholders substituted)' -ForegroundColor Green
    }
}

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host 'Next steps:' -ForegroundColor Cyan
Write-Host ('  1. Open XR_APILAYER_{0}_{1}.sln in Visual Studio 2019+.' -f $Vendor, $LayerName)
Write-Host '  2. git submodule update --init --recursive (if not already done)'
Write-Host '  3. Build Release|x64. The pre-build event regenerates dispatch.gen.{h,cpp}.'
Write-Host '  4. Edit openxr-api-layer/layer.cpp to add your layer logic.'
Write-Host '  5. git commit -am "Initialize layer from template"'
Write-Host '  6. (Optional) Add Certum signing secrets — see README + docs/DEVELOPMENT.md.'
