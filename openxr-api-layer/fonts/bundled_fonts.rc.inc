// MIT License
//
// Copyright (c) 2026 Michael Ledour
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// =============================================================================
// bundled_fonts.rc.inc — single source of truth for the RT_BUNDLED_FONT
// resources embedded into BOTH the layer DLL and the test EXE.
//
// Why a shared include rather than duplicate declarations in the two .rc
// files:
//   - loadBundledFontCollection (overlay_renderer.cpp) uses
//     GetModuleHandleEx(FROM_ADDRESS, &pickSwapchainFormat) to find the
//     module its FindResource call should target. pickSwapchainFormat is
//     linked into BOTH the layer DLL and the test EXE, so the resolved
//     HMODULE — and therefore the resource table that gets walked — flips
//     depending on which binary is running.
//   - If the two .rc files drift (someone adds a font to the DLL's .rc.in
//     but forgets the test's .rc), the test EXE's FindResource for the
//     new ID returns null, the loader silently falls back to system
//     fonts, and the snapshot test produces a DIFFERENT pixel layout
//     than the in-headset HUD — without any error being raised. The
//     visual-regression test would then succeed against a golden that
//     doesn't match production rendering.
//   - The shared include makes drift impossible by construction: there's
//     no "other place" to add a font.
//
// How the include is used:
//
//   // <some-project>.rc:
//   #include "<relative-path>/bundled_fonts.rc.inc"
//
// That's it on the .rc side. The TTF basenames here are bare — rc.exe
// resolves them via its /I include-search-path list. Both projects'
// vcxproj files have a <ResourceCompile><AdditionalIncludeDirectories>
// entry pointing at $(SolutionDir)openxr-api-layer\fonts so rc.exe
// finds each .ttf at exactly one location regardless of which binary
// is being built. (We learned the hard way: rc.exe does NOT concatenate
// adjacent string literals the way the C compiler does, so a macro-
// based "FONTS_PREFIX + basename" trick fails with RC2135 file-not-found
// errors. /I is the supported mechanism.)
//
// Scope: this file is **fonts only** (RT_BUNDLED_FONT entries). If a
// future feature needs to bundle a different resource type (icons,
// audio, …), it gets its OWN include file alongside this one —
// `bundled_icons.rc.inc`, etc. — with a matching /I path setup in the
// vcxprojs. The narrow scope keeps each include's purpose obvious from
// its name, and lets the lifecycle (subset / re-bundle) be managed
// independently per resource family.
//
// Adding a new bundled font:
//   1. Drop the TTF in openxr-api-layer/fonts/. Subset it first so the
//      DLL doesn't pay the full multi-script weight — see
//      docs/DEVELOPMENT.md for the pyftsubset invocation.
//   2. Add the #define for its ID below (≥ 200 — < 200 reserved for
//      icons / dialogs the DLL's .rc.in template may add later).
//   3. Add the resource declaration line. The file argument is a bare
//      basename; rc.exe resolves it via the /I include path.
//   4. List the .ttf in BOTH vcxprojs as <None Include="..."> for IDE
//      visibility and to trigger a rebuild when the file changes.
//   5. Regenerate the snapshot golden via the Update overlay snapshot
//      workflow (#15) since the new font will affect rendering.
//
// What this file MUST NOT contain:
//   - VERSIONINFO blocks (those are per-binary and live in each project's
//     own .rc).
//   - Path prefixes on the file arguments — those defeat the /I include
//     mechanism, hard-coding a single binary's view of the filesystem.
//   - Anything that requires expression evaluation against project
//     properties — those need to be in the includer .rc/.rc.in where
//     MSBuild's @VAR@ substitutions run.
// =============================================================================

// Custom resource type ID for embedded TTF font files. The Win32 RT_*
// constants (RT_BITMAP=2, RT_ICON=3, …) reserve 1..255; 256 is the first
// free integer for application-defined types and the convention this
// project uses. The matching numeric (MAKEINTRESOURCEW(256)) is what
// loadBundledFontCollection passes to FindResource at runtime.
#ifndef RT_BUNDLED_FONT
#define RT_BUNDLED_FONT 256
#endif

// Resource IDs. Stay ≥ 200 to leave headroom for future icons / dialogs
// in the layer's .rc.in (Windows-shell convention puts those at 100-199).
//
// Mixed bundle, two families, two purposes:
//   - Rajdhani SemiBold (600) by Indian Type Foundry — used UPRIGHT for
//     all labels, section titles, and the "Render" / "App" labels of
//     the CPU panel's compound string. Narrower, technical-feeling
//     grotesque; reads as crisp small-cap labels at 14-22 px.
//   - Barlow Medium Italic (500) by The Barlow Project Authors — used
//     ITALIC for the chiffres (FPS, frametimes, °C, %, ms values).
//     Wider geometric grotesque with a real italic cut (no synthetic
//     oblique), gives the chiffres a "motion / dynamic readout" feel
//     distinct from the static labels.
//
// Both fonts are SIL OFL licensed (see fonts/OFL.txt for the dual
// copyright notice).
//
// The renderer's `init()` builds two sets of IDWriteTextFormats: chiffres
// formats anchor to "Barlow" with weight MEDIUM + style ITALIC; label /
// title formats anchor to "Rajdhani" with weight SEMI_BOLD + style
// NORMAL. The compound CPU-panel string (which mixes label + chiffres
// in one rendered run) anchors to Rajdhani upright as its BASE and uses
// IDWriteTextLayout::SetFontFamilyName + SetFontWeight + SetFontStyle
// to override the chiffres ranges to Barlow Medium Italic at draw time.
#define IDR_FONT_RAJDHANI_SEMIBOLD     200
#define IDR_FONT_BARLOW_MEDIUM_ITALIC  201

// Bare basenames — rc.exe finds the actual TTFs via the /I include path
// configured in each project's vcxproj ResourceCompile section.
IDR_FONT_RAJDHANI_SEMIBOLD    RT_BUNDLED_FONT "Rajdhani-SemiBold.ttf"
IDR_FONT_BARLOW_MEDIUM_ITALIC RT_BUNDLED_FONT "Barlow-MediumItalic.ttf"
