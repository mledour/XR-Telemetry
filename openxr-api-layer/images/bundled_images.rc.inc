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
// bundled_images.rc.inc — single source of truth for the RT_BUNDLED_IMAGE
// resources embedded into BOTH the layer DLL and the test EXE.
//
// Symmetric to fonts/bundled_fonts.rc.inc — see that file's header for the
// full rationale on WHY a shared include rather than per-binary duplication
// (TL;DR: loadBundledBackground in overlay_renderer.cpp uses
// GetModuleHandleEx(FROM_ADDRESS, …) to find the module its FindResource
// targets, and overlay_renderer.cpp is linked into BOTH binaries — so the
// resource table that gets walked flips depending on which binary is
// running. Drift between the two .rc files would silently make the test
// EXE render with a different background than the layer DLL).
//
// Scope: this file is **images only** (RT_BUNDLED_IMAGE entries). Fonts
// live in fonts/bundled_fonts.rc.inc. The narrow scope keeps each
// include's purpose obvious from its name, and lets the lifecycle
// (re-export from Figma, swap reference image) be managed independently
// per resource family.
//
// How the include is used:
//
//   // <some-project>.rc:
//   #include "bundled_images.rc.inc"
//
// PNG basenames are bare — rc.exe resolves them via its /I include-search
// path. Both projects' vcxproj files have a <ResourceCompile>
// <AdditionalIncludeDirectories> entry pointing at
// $(SolutionDir)openxr-api-layer\images so rc.exe finds each .png at
// exactly one location regardless of which binary is being built. The
// fonts include uses the same /I mechanism for the same reason — see
// the bundled_fonts.rc.inc header for the RC2135 we hit when we tried
// a macro-based prefix trick instead.
//
// Adding a new bundled image:
//   1. Drop the PNG in openxr-api-layer/images/. Keep it small —
//      every byte ships in every OpenXR game process that loads the
//      layer DLL. Prefer indexed-palette or 8-bit PNGs for static UI
//      assets; 24-bit only if the source has photographic gradients.
//   2. Add the #define for its ID below (≥ 300 — < 300 reserved for
//      future icon resources if the layer ever grows a dialog).
//   3. Add the resource declaration line. The file argument is a bare
//      basename; rc.exe resolves it via the /I include path.
//   4. List the .png in BOTH vcxprojs as <None Include="..."> for IDE
//      visibility and to trigger a rebuild when the file changes.
//   5. Regenerate the snapshot golden via the Update overlay snapshot
//      workflow (#15) since the new background will affect rendering.
//
// What this file MUST NOT contain:
//   - VERSIONINFO blocks (per-binary, live in each project's own .rc).
//   - Path prefixes on the file arguments — those defeat the /I include
//     mechanism, hard-coding a single binary's view of the filesystem.
//   - Anything that requires expression evaluation against project
//     properties — those need to be in the includer .rc/.rc.in where
//     MSBuild's @VAR@ substitutions run.
// =============================================================================

// Custom resource type ID for embedded image files. The Win32 RT_*
// constants (RT_BITMAP=2, RT_ICON=3, …) reserve 1..255; 256+ is the
// app-defined range. RT_BUNDLED_FONT is 256 (see fonts include);
// RT_BUNDLED_IMAGE picks 257 — each asset family gets its own custom RT
// for clarity at the FindResource call site (loadBundledFontCollection
// passes 256, loadBundledBackground passes 257). The #ifndef guard makes
// the include order-independent vs. any future header that defines the
// same constant.
//
// Why NOT use the built-in RT_BITMAP (= 2)? RT_BITMAP expects a DIB
// (device-independent bitmap, raw BITMAPINFOHEADER + pixel data),
// not a PNG. Loading a DIB through RT_BITMAP would force us to either
// (a) ship the asset uncompressed (~1.3 MB at 720×452 BGRA8) instead
// of the ~120 KB PNG, or (b) bake a custom DIB→PNG packer. Shipping a
// PNG under a custom RT and decoding via WIC at runtime is the
// standard Windows-app idiom and keeps the binary small.
#ifndef RT_BUNDLED_IMAGE
#define RT_BUNDLED_IMAGE 257
#endif

// Resource IDs. Stay ≥ 300 to leave a 100-ID gap above the fonts (200-201)
// for any future font additions, and to keep the IDR_FONT_* / IDR_IMAGE_*
// ranges visually distinct at FindResource call sites.
//
// The overlay background is the alpha0 design reference image
// (overlay_performance_alpha0-size.png) with all dynamic content
// erased: chiffres (FPS, ms values, °C readings, percentages), the
// frametime histogram bars, and the bottom-row value/unit pairs. What
// remains is the full static decoration — the slot-cut chamferred
// frame, the section dividers, the labels ("FPS / FPS AVG / P95 / P99
// / P99.9", "GPU FRAMETIME MS", "CPU FRAMETIME MS", "App ms / Render
// ms", "GPU TEMP / GPU LOAD / VRAM / CPU TEMP / CPU LOAD"), and the
// dashed grid behind the histograms. The renderer blits this as the
// first paint step and then draws ONLY the dynamic content on top —
// the labels/decoration come "for free" pixel-perfect from the
// reference image, eliminating an entire class of D2D layout drift.
//
// Sized 720×452 to match kOverlayTexW×kOverlayTexH (overlay_renderer.h).
// A static_assert on the .cpp side guards the dimensions from drifting
// out of sync with the renderer's texture size.
#define IDR_IMAGE_OVERLAY_BACKGROUND  300

// Bare basename — rc.exe finds the actual PNG via the /I include path
// configured in each project's vcxproj ResourceCompile section.
IDR_IMAGE_OVERLAY_BACKGROUND  RT_BUNDLED_IMAGE  "overlay_background.png"
