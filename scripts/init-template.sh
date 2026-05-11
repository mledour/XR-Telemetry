#!/usr/bin/env bash
#
# init-template.sh — bash port of Init-Template.ps1.
#
# Post-clone substitution for OpenXR-Layer-Template. Prompts for the
# values that vary per layer (vendor tag, layer name, author name,
# year, GitHub owner), then walks the tree and replaces placeholder
# tokens in BOTH file contents and filenames.
#
# Run ONCE, right after `git clone` + `git submodule update --init
# --recursive`. Idempotent in spirit — running it again after a
# successful run is a no-op (the placeholders aren't there anymore)
# — but rerunning after partial substitution leads to weirdness, so
# do it once and commit the result.
#
# Functional parity with Init-Template.ps1; the PowerShell version is
# what Windows / VS users will run, this is for macOS / Linux / WSL
# dev machines.
#
# Usage:
#   bash scripts/init-template.sh                 # interactive
#   bash scripts/init-template.sh --help          # show flags
#
# Non-interactive (CI / scripted setups):
#   bash scripts/init-template.sh \
#       --vendor MLEDOUR \
#       --layer-name fov_crop \
#       --author-name "Michael Ledour" \
#       --author-email "you@example.com" \
#       --github-owner mledour \
#       --year 2026 \
#       --no-confirm

set -euo pipefail

# Resolve the repo root from this script's own path (this script lives
# under scripts/) so it works regardless of the caller's CWD.
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

# ---- defaults / flags ----------------------------------------------------

vendor=""
layer_name=""
author_name=""
author_name_as_on_cert=""
author_email=""
github_owner=""
year=""
dry_run=0
no_confirm=0

usage() {
    sed -n 's/^# \{0,1\}//p' "$0" | head -25
    cat <<'EOF'

Flags:
  --vendor VENDOR                 UPPERCASE tag (e.g. MLEDOUR, NOVENDOR)
  --layer-name NAME               lowercase_with_underscores (e.g. fov_crop)
  --author-name "NAME"            for LICENSE + source headers
  --author-name-as-on-cert "NAME" Certum cert subject CN (defaults to --author-name)
  --author-email EMAIL            for source headers
  --github-owner OWNER            GitHub user/org used in repo URL references
  --year YEAR                     defaults to the current year
  --no-confirm                    skip the final "Proceed?" prompt
  --dry-run                       print what would change without writing
  --help                          show this help
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --vendor)                  vendor="$2"; shift 2 ;;
        --layer-name)              layer_name="$2"; shift 2 ;;
        --author-name)             author_name="$2"; shift 2 ;;
        --author-name-as-on-cert)  author_name_as_on_cert="$2"; shift 2 ;;
        --author-email)            author_email="$2"; shift 2 ;;
        --github-owner)            github_owner="$2"; shift 2 ;;
        --year)                    year="$2"; shift 2 ;;
        --no-confirm)              no_confirm=1; shift ;;
        --dry-run)                 dry_run=1; shift ;;
        --help|-h)                 usage ;;
        *)
            echo "Unknown flag: $1" >&2
            echo "Run with --help for the list." >&2
            exit 2
            ;;
    esac
done

# ---- prompts -------------------------------------------------------------

# read_required PROMPT DEFAULT REGEX REGEX_HINT
# Loops until the user enters a non-empty value that matches REGEX
# (if a regex is provided). If REGEX is empty, any non-empty value
# is accepted. Sets the result in the global $reply.
read_required() {
    local prompt="$1" default="${2:-}" regex="${3:-}" hint="${4:-}"
    local hint_str=""
    [[ -n "$default" ]] && hint_str=" [$default]"
    while :; do
        # `read -r` so backslashes aren't interpreted. We can't use
        # `read -p` with multi-line prompts portably, so echo + read.
        printf '%s%s: ' "$prompt" "$hint_str"
        local value
        if ! IFS= read -r value; then
            echo "" >&2
            echo "(no input — aborted)" >&2
            exit 1
        fi
        [[ -z "$value" ]] && value="$default"
        if [[ -z "$value" ]]; then
            echo "  (required)" >&2
            continue
        fi
        if [[ -n "$regex" && ! "$value" =~ $regex ]]; then
            echo "  must match: $hint" >&2
            continue
        fi
        reply="$value"
        return
    done
}

if [[ -z "$vendor" ]]; then
    echo
    echo "OpenXR API layer names follow the convention"
    echo "    XR_APILAYER_<VENDOR>_<layer_name>"
    echo "Vendor is UPPERCASE (e.g. MLEDOUR, MBUCCHIA, NOVENDOR), layer"
    echo "name is lowercase_with_underscores (e.g. fov_crop, toolkit)."
    echo
    read_required \
        "Vendor tag (UPPERCASE, letters/digits/underscore)" \
        "NOVENDOR" \
        '^[A-Z0-9_]+$' \
        '^[A-Z0-9_]+$'
    vendor="$reply"
fi

if [[ -z "$layer_name" ]]; then
    read_required \
        "Layer short name (lowercase_with_underscores)" \
        "my_layer" \
        '^[a-z][a-z0-9_]*$' \
        '^[a-z][a-z0-9_]*$ (starts with a letter)'
    layer_name="$reply"
fi

if [[ -z "$author_name" ]]; then
    read_required "Author name (for LICENSE and source headers)" "" "" ""
    author_name="$reply"
fi

if [[ -z "$author_name_as_on_cert" ]]; then
    # When --no-confirm was set we treat all "has a sensible default"
    # fields as fully non-interactive: default to the author name and
    # never prompt. Same applies to --github-owner below.
    if [[ "$no_confirm" -eq 1 ]]; then
        author_name_as_on_cert="$author_name"
    else
        read_required \
            "Author name AS PRINTED ON YOUR CODE-SIGNING CERT (Certum cap-sensitive; use Author name if no cert yet)" \
            "$author_name" "" ""
        author_name_as_on_cert="$reply"
    fi
fi

if [[ -z "$author_email" ]]; then
    read_required "Author email (used in source-file headers only)" "" "" ""
    author_email="$reply"
fi

if [[ -z "$github_owner" ]]; then
    if [[ "$no_confirm" -eq 1 ]]; then
        github_owner="${USER:-${USERNAME:-unknown}}"
    else
        read_required \
            "GitHub owner / org (used in repo URL references)" \
            "${USER:-${USERNAME:-}}" \
            "" ""
        github_owner="$reply"
    fi
fi

if [[ -z "$year" ]]; then
    year="$(date +%Y)"
fi

# ---- summary ------------------------------------------------------------

# Substitutions applied to BOTH file contents AND filenames. Order
# matters: the longest pattern goes first so Michael Ledour does not
# accidentally match the inside of Michael Ledour.
full_layer_name="XR_APILAYER_${vendor}_${layer_name}"

# Parallel arrays — bash 3 (macOS default) does not support
# associative arrays portably, so we use index-aligned lists.
subs_from=(
    "XR_APILAYER_MLEDOUR_xr_telemetry"
    "MLEDOUR"
    "xr_telemetry"
    "Michael Ledour"
    "Michael Ledour"
    "michael.ledour@gmail.com"
    "mledour"
    "2026"
)
subs_to=(
    "$full_layer_name"
    "$vendor"
    "$layer_name"
    "$author_name_as_on_cert"
    "$author_name"
    "$author_email"
    "$github_owner"
    "$year"
)

echo
echo "--- Summary ---"
for i in "${!subs_from[@]}"; do
    printf '  %-32s -> %s\n' "${subs_from[$i]}" "${subs_to[$i]}"
done
echo

if [[ "$no_confirm" -ne 1 ]]; then
    printf 'Proceed with these substitutions? [y/N] '
    IFS= read -r ok
    if [[ ! "$ok" =~ ^[Yy] ]]; then
        echo "Aborted."
        exit 1
    fi
fi

# ---- platform-portable sed in-place -------------------------------------
# macOS / BSD sed wants `sed -i '' -e ...`, GNU sed wants `sed -i -e ...`.
# Detect once.
if sed --version >/dev/null 2>&1; then
    SED_INPLACE=(sed -i)
else
    SED_INPLACE=(sed -i '')
fi

# Escape replacement so '&', '\', and the delimiter '|' don't get
# special-cased inside sed. We use '|' as the delimiter to keep '/'
# (common in URLs and paths) unambiguous.
escape_sed_replacement() {
    printf '%s' "$1" | sed -e 's/[&|\\]/\\&/g'
}

# ---- file enumeration ---------------------------------------------------
# Only touch text files we control — never anything under external/,
# .git/, packages/, bin/, obj/, stage/. The find call enumerates the
# tree and filters by extension. Use a bash array for find arguments
# instead of eval-ing a string: with eval the shell tries to
# glob-expand bare `*.md` patterns against the current directory,
# which silently drops most files (only ones whose extension happens
# to also exist at the CWD survive). That bug ate LICENSE and the
# README/DEVELOPMENT.md files in testing — see the testing notes in
# the commit message.

prune_args=(
    -path "$repo_root/.git" -prune -o
    -path "$repo_root/external" -prune -o
    -path "$repo_root/packages" -prune -o
    -path "$repo_root/bin" -prune -o
    -path "$repo_root/obj" -prune -o
    -path "$repo_root/stage" -prune -o
)

# Text-file extensions we always treat as text. Built as a properly-
# quoted find -name argument array so the shell can't glob-expand
# the wildcards.
text_exts=(cpp h hpp c cc json def config vcxproj filters sln yml yaml ps1 py bat cmd sh md txt iss in rc gitignore gitattributes gitmodules clang-format)
ext_args=()
for ext in "${text_exts[@]}"; do
    if [[ ${#ext_args[@]} -gt 0 ]]; then
        ext_args+=(-o)
    fi
    ext_args+=(-name "*.$ext")
done

# Known extensionless text files we always treat as substitution
# targets at the repo root. Explicit list beats a NUL-byte content
# heuristic — the heuristic was found in testing to silently exclude
# LICENSE on at least one bash/find combination.
extensionless_text_files=(LICENSE THIRD_PARTY)

# Collect candidate files into an array. Bash 3 doesn't have
# readarray, so use a while-read loop.
mapfile_files() {
    local out_var="$1"
    local -a accum=()
    while IFS= read -r -d '' f; do
        accum+=("$f")
    done < <(find "$repo_root" "${prune_args[@]}" -type f \( "${ext_args[@]}" \) -print0)
    for name in "${extensionless_text_files[@]}"; do
        local p="$repo_root/$name"
        [[ -f "$p" ]] && accum+=("$p")
    done
    eval "$out_var=(\"\${accum[@]}\")"
}

declare -a all_files=()
mapfile_files all_files

echo "Scanning ${#all_files[@]} files ..."

# ---- content substitution ----------------------------------------------

files_modified=0
for f in "${all_files[@]}"; do
    # Snapshot original content size to detect changes cheaply.
    pre_hash="$(LC_ALL=C wc -c < "$f")"
    for i in "${!subs_from[@]}"; do
        from="${subs_from[$i]}"
        to_escaped="$(escape_sed_replacement "${subs_to[$i]}")"
        from_escaped="$(printf '%s' "$from" | sed -e 's/[][\\^$.*/+?(){}|]/\\&/g')"
        if [[ "$dry_run" -eq 1 ]]; then
            # In dry-run, check whether the substitution would change
            # the file but don't write.
            if grep -q -F -- "$from" "$f" 2>/dev/null; then
                echo "  would modify: ${f#$repo_root/}"
                break
            fi
        else
            "${SED_INPLACE[@]}" -e "s|${from_escaped}|${to_escaped}|g" "$f"
        fi
    done
    post_hash="$(LC_ALL=C wc -c < "$f")"
    if [[ "$pre_hash" != "$post_hash" ]]; then
        files_modified=$((files_modified + 1))
    fi
done
echo "Content: $files_modified files modified"

# ---- file / directory rename -------------------------------------------
# Rename files whose NAME contains one of the placeholder strings.
# Files first (deepest-first), then directories (also deepest-first
# so a parent rename doesn't invalidate child paths mid-iteration).

rename_with_placeholders() {
    local path="$1"
    local name new_name parent new_path
    name="$(basename "$path")"
    new_name="$name"
    for i in "${!subs_from[@]}"; do
        new_name="${new_name//${subs_from[$i]}/${subs_to[$i]}}"
    done
    [[ "$new_name" == "$name" ]] && return
    parent="$(dirname "$path")"
    new_path="$parent/$new_name"
    if [[ "$dry_run" -eq 1 ]]; then
        echo "  would rename: $name -> $new_name"
    else
        # Use git mv if the file is git-tracked, fall back to plain mv
        # for untracked files (matters when run before the first commit).
        if (cd "$repo_root" && git ls-files --error-unmatch "${path#$repo_root/}" >/dev/null 2>&1); then
            (cd "$repo_root" && git mv "${path#$repo_root/}" "${new_path#$repo_root/}")
        else
            mv "$path" "$new_path"
        fi
        echo "  renamed: $name -> $new_name"
    fi
}

# Files (matching ANY placeholder in basename).
while IFS= read -r -d '' f; do
    base="$(basename "$f")"
    matched=0
    for i in "${!subs_from[@]}"; do
        if [[ "$base" == *"${subs_from[$i]}"* ]]; then
            matched=1; break
        fi
    done
    [[ "$matched" -eq 1 ]] && rename_with_placeholders "$f"
done < <(find "$repo_root" "${prune_args[@]}" -type f -print0)

# Directories — deepest first so renaming a child doesn't change a
# parent's path mid-iteration. -depth on BSD find (macOS) takes no
# operand and recurses depth-first; same name + behaviour on GNU find.
while IFS= read -r -d '' d; do
    base="$(basename "$d")"
    matched=0
    for i in "${!subs_from[@]}"; do
        if [[ "$base" == *"${subs_from[$i]}"* ]]; then
            matched=1; break
        fi
    done
    [[ "$matched" -eq 1 ]] && rename_with_placeholders "$d"
done < <(find "$repo_root" "${prune_args[@]}" -type d -depth -print0 2>/dev/null \
         || find "$repo_root" "${prune_args[@]}" -depth -type d -print0)

# ---- sanity check ------------------------------------------------------
# Anything still matching <<...>> in our placeholder shape probably
# means we missed a substitution. Print a short report.

if [[ "$dry_run" -ne 1 ]]; then
    echo
    echo "Sanity check (any remaining placeholders that look like ours):"
    # `grep -rE` on macOS BSD grep works the same way as GNU here.
    leftovers=$(grep -rEn '<<[A-Z_]+>>' "$repo_root" 2>/dev/null \
        | grep -vE '/\.git/|/external/|/packages/|/bin/|/obj/|/stage/' \
        | head -20 || true)
    if [[ -n "$leftovers" ]]; then
        echo "$leftovers" | sed "s|^${repo_root}/||"
        echo "(if the above are real layer-template placeholders the script missed, fix them by hand.)"
    else
        echo "  (none — all placeholders substituted)"
    fi
fi

echo
echo "Done."
echo "Next steps:"
echo "  1. Open XR_APILAYER_${vendor}_${layer_name}.sln in Visual Studio 2019+."
echo "  2. git submodule update --init --recursive (if not already done)"
echo "  3. Build Release|x64. The pre-build event regenerates dispatch.gen.{h,cpp}."
echo "  4. Edit openxr-api-layer/layer.cpp to add your layer logic."
echo '  5. git commit -am "Initialize layer from template"'
echo "  6. (Optional) Add Certum signing secrets — see README + docs/DEVELOPMENT.md."
