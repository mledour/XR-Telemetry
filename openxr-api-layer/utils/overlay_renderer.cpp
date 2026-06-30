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

#include "pch.h"

#include "overlay_renderer.h"

#include "chrome_shape_renderer.h"
#include "glyph_atlas.h"
#include "glyph_atlas_renderer.h"
#include "histogram_ring.h"
#include "overlay_cadence.h"
#include "overlay_layout.h"

#include "framework/dispatch.gen.h"   // OpenXrApi (auto-generated at build)
#include "framework/log.h"            // Log / ErrorLog

// <d2d1.h> is included ONLY for the header-only D2D1_RECT_F / D2D1::RectF
// helper used as the layout-rect type — no D2D runtime call remains after
// the GPU migration, so d2d1.dll is never loaded into the host process.
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_3.h>     // IDWriteFactory5 / IDWriteInMemoryFontFileLoader (Win10 1703+)
// <d3d11_1.h>: ID3D11Device1 / ID3D11DeviceContext1 / ID3DDeviceContextState
// + CreateDeviceContextState / SwapDeviceContextState — the per-frame
// pipeline-state isolation on the app's shared immediate context (D3D11
// path). pch.h's <d3d11.h> only exposes the base interfaces.
#include <d3d11_1.h>

#include <xrprof.h>   // inline self-profiler (external/xrprof submodule)

#include <cmath>
#include <cstring>     // std::memcpy for constant-buffer uploads
#include <optional>
#include <cwchar>      // std::wcslen for the wide-literal degree-symbol path
#include <string>
#include <vector>

// Offline-compiled shader bytecode (FxCompile → $(IntDir), on the C++
// include path in both the layer and test vcxproj). Each header declares a
// `const BYTE g_<filename>[]` (internal linkage, so including them in this
// one TU is ODR-safe). HistogramBarRenderer builds the instanced-bar and
// solid-quad pipelines from these to paint the histogram region on the GPU.
#include "overlay_bars_vs.h"
#include "overlay_bars_ps.h"
#include "overlay_quad_vs.h"
#include "overlay_quad_ps.h"

// Pragma-link DirectWrite (the glyph atlas rasterises through it at init).
// dwrite.lib ships with the Windows SDK; no extra NuGet package needed.
// No d2d1.lib — D2D is header-type-only now (see the <d2d1.h> note above).
#pragma comment(lib, "dwrite.lib")

namespace openxr_api_layer::detail {

    // Log() / ErrorLog() live in openxr_api_layer::log. Pull them in with
    // EXPLICIT using-declarations (not a using-directive) so MSVC's name
    // lookup unambiguously routes the unqualified Log()/ErrorLog() calls
    // inside the anonymous namespace below to the framework helpers — a
    // using-directive in the same spot was tripping some MSVC builds
    // even though it should be equivalent per the standard.
    using ::openxr_api_layer::log::Log;
    using ::openxr_api_layer::log::ErrorLog;
    // Pull the ETW provider handle into this namespace so the unqualified
    // TraceLoggingWrite / IsTraceEnabled() macros (both name g_traceProvider)
    // resolve here — paintOverlay() emits the chrome-rebuild timing span
    // through it. Explicit using-declaration, same style as the Log /
    // ErrorLog lines above (a using-directive trips some MSVC builds; see the
    // note above).
    using ::openxr_api_layer::log::g_traceProvider;

    // Local aliases for the utils sub-namespaces. Without these,
    // unqualified `glyph_atlas::Renderer` / `chrome_shapes::Renderer`
    // references inside the anonymous namespace below resolve as if
    // those names were sibling sub-namespaces of `detail`, which they
    // aren't — both modules live under openxr_api_layer::utils.
    namespace glyph_atlas   = ::openxr_api_layer::utils::glyph_atlas;
    namespace chrome_shapes = ::openxr_api_layer::utils::chrome_shapes;

    namespace {

        // Write the per-frame space + pose + size into the quad layer.
        // Shared verbatim by the D3D11 and D3D12 renderAndCompose paths so
        // the anchor contract can't drift between the two backends:
        //   - anchorPose == null → head-locked: keep the identity orientation
        //     filled once in init(), use the geometry offset as the position.
        //   - anchorPose != null → world-locked: take the caller's frozen
        //     world pose verbatim (orientation included).
        // The quad SIZE always comes from the geometry in both modes.
        inline void applyQuadPose(XrCompositionLayerQuad& quad, XrSpace space,
                                  const XrPosef* anchorPose,
                                  const OverlayGeometry& geo) noexcept {
            quad.space = space;
            if (anchorPose) {
                quad.pose = *anchorPose;
            } else {
                quad.pose.position = {geo.pos_x, geo.pos_y, geo.pos_z};
            }
            quad.size = {geo.width_m, geo.height_m};
        }

        // -------- Layout constants (fpsVR redesign) -------------------------
        //
        // Texture stays at this fixed size regardless of `scale` — the
        // QUAD in 3D scales, not the resolution. kTexW × kTexH packs
        // the four sections of the redesigned HUD:
        //   - header bar (FPS / FPS AVG / P95 / P99 / P99.9, 5 cells)
        //   - GPU FRAMETIME panel with histogram + current value
        //   - CPU FRAMETIME panel with histogram + Render / App compound
        //   - bottom row split into two panels (60 / 40):
        //       GPU panel = TEMP / LOAD / VRAM (3 cells)
        //       CPU panel = TEMP / LOAD       (2 cells)
        //
        // The texture is shorter than the previous 720×540 (4:3) layout
        // because the frametime panels are now 90 px each (was 160→120
        // in earlier revisions). With less vertical real estate, the
        // histogram strip is ~54 px after the title row — short
        // enough that a normal frame at ~50 % budget fills most of
        // the strip visually, no empty top region.
        //
        // Vertical budget (top → bottom), all in pixels at native
        // texture resolution. The actual sum is documented as a check
        // for whoever tweaks the constants next:
        //   kOuterPad           (10)
        //   kFrameStroke         (2)
        //   inner padding        (4)   ← see kInnerT below
        //   kHeaderHeight       (90)
        //   kSectionGap          (4)
        //   kFrametimeHeight   (106) — GPU panel
        //   kSectionGap          (4)
        //   kFrametimeHeight   (106) — CPU panel
        //   kSectionGap          (4)
        //   kBottomHeight       (90)
        //   inner padding        (4)
        //   kFrameStroke         (2)
        //   kOuterPad           (10)
        //   Total = 436 = kTexH, leaving zero slack between the last
        //   panel and the inner bottom edge. Keeping the budget exact
        //   makes the top and bottom borders read at the same
        //   thickness in the HMD (4 px inner pad + 2 px stroke = 6 px
        //   on each side). `bottomY + kBottomHeight == kInnerB` by
        //   construction (kInnerB derives from kTexH below, so the two
        //   stay locked as long as the budget sums to kTexH). Bumping
        //   any of the heights past this budget will visually clip the
        //   bottom panel.
        constexpr int32_t kTexW = 720;
        constexpr int32_t kTexH = 436;  // 416 + 2×10: the two frametime panels
                                         // grew from 90 → 100 px to give the new
                                         // left-hand ms axis room (kFrametimeHeight
                                         // below). Keep this in lockstep with the
                                         // vertical-budget ledger above AND with W/H
                                         // in test_overlay_snapshot.cpp; the golden
                                         // PNG must be regenerated when it changes.

        // -------- Overlay supersampling -------------------------------------
        //
        // The HUD is AUTHORED entirely in the logical (kTexW × kTexH) "design
        // pixel" space — every layout constant, font size, inline offset and
        // the histogram bar geometry below stays in that space and is NEVER
        // scaled. We raise only the RENDER resolution: the swapchain image,
        // each renderer's viewport, and the glyph-atlas bake are multiplied
        // by kOverlaySupersample so the runtime composites from a sharper
        // source. The quad's WORLD size (overlay_layout.h) is unchanged, so
        // the HUD subtends the same angle in the HMD — it does NOT get bigger
        // on screen, only crisper.
        //
        // Why this helps: a Quad composition layer is resampled by the
        // runtime with a single bilinear tap through the lens-distortion /
        // reprojection mesh, and the quad's texel grid never aligns to the
        // HMD pixel grid — that softens crisp text by ~1 px no matter what.
        // Feeding the compositor ~kOverlaySupersample² the texels is what
        // recovers the sharpness that single tap eats. The glyph atlas is
        // baked at the matching physical sizes so the extra texels carry real
        // glyph detail (not an upscale of the logical-size raster).
        //
        // 1.0 == legacy 720×436. Production passes 2.0×. Earlier HMD tuning
        // pushed this to 3.0× chasing off-axis thin-line aliasing — but that
        // turned out to be carried by the line thickening (kChromeLineW), the
        // unconditional quad AA, and the dim budget colour, all of which are
        // resolution-independent. So the factor is really just the FONT
        // sharpness / cost dial: glyphs are textured, only more source texels
        // sharpen them (AA + thickening don't touch them).
        //
        // 2.0× (1440×872, ≈ 5 MB per swapchain image) is the balance —
        // noticeably sharper text than 1.5×, and the 2048×4096 atlas packs
        // comfortably, whereas 3.0× pushed a 3072×6144 atlas (3× glyphs) with a
        // real shelf-pack-overflow risk (overflow fails closed = HUD disabled).
        // Render cost is sub-ms (cadenced paint) at any of these. Change ONLY
        // this constant — physical extents, atlas, bar/line scaling and the
        // snapshot all derive from it: drop to 1.5× if a GPU is bound, raise
        // for crisper text.
        constexpr float   kOverlaySupersample = 2.0f;   // requested factor
        // Physical pixel extents, rounded through the SAME helper the atlas
        // bake and glyph lookup use (glyph_atlas::supersampledSizePx) — one
        // rounding rule, no open-coded (int)(x*ss+0.5f) drift.
        constexpr int32_t kTexWPhys = glyph_atlas::supersampledSizePx(
            static_cast<uint16_t>(kTexW), kOverlaySupersample);
        constexpr int32_t kTexHPhys = glyph_atlas::supersampledSizePx(
            static_cast<uint16_t>(kTexH), kOverlaySupersample);
        // EFFECTIVE factor after pixel rounding = exactly the ratio each
        // renderer derives as renderW/dstW (kTexWPhys/kTexW). The atlas bake
        // and the histogram bars take THIS value (not the requested factor) so
        // the bake size, the glyph-lookup size and the bar viewport agree. At
        // the shipped 2.0x it equals kOverlaySupersample.
        //
        // This single scalar is WIDTH-derived, and the bars + glyph passes also
        // scale Y by it — correct only when width and height round to the SAME
        // ratio. The assert below enforces that at compile time, so a future
        // tweak to kOverlaySupersample can't pick a factor (e.g. 1.3x:
        // 936/720 != 567/436) that would misalign the bars + glyph vertical
        // metrics by a fraction of a pixel. 1.5x and 2.0x both pass.
        constexpr float kEffectiveSupersample =
            static_cast<float>(kTexWPhys) / static_cast<float>(kTexW);
        static_assert(static_cast<int64_t>(kTexWPhys) * kTexH ==
                          static_cast<int64_t>(kTexHPhys) * kTexW,
                      "kOverlaySupersample must scale width and height by the "
                      "same ratio (kTexWPhys/kTexW == kTexHPhys/kTexH) — use a "
                      "factor like 1.5 or 2.0; one like 1.3 rounds W and H "
                      "differently and would misalign the bars + glyph metrics");

        constexpr float kOuterPad       = 10.0f;
        constexpr float kFrameStroke    = 2.0f;
        constexpr float kSectionGap     =  4.0f;  // gap between every framed box; set
                                                  // equal to the outer frame's inner
                                                  // padding (the +4 in kInnerL/T below)
                                                  // so the inter-box gaps and the frame-
                                                  // to-content margin read identically.
                                                  // The 12 px reclaimed from the old 8 px
                                                  // (3 vertical band gaps) is absorbed by
                                                  // the two frametime panels (see
                                                  // kFrametimeHeight) so the budget still
                                                  // sums to kTexH.
        constexpr float kSectionInnerPad = 12.0f;  // padding INSIDE each panel

        // Thin structural line width — panel borders, column separators, AND
        // the histogram grid + budget lines — in LOGICAL px. Bumped 1.0 -> 1.5
        // after HMD testing: a 1-px line is too thin to survive the runtime's
        // quad-layer resample once the HUD sits off-axis, where it hatches at
        // the lens periphery. 1.5px (~3 physical at 2.0x) holds as a
        // continuous line there; marginally heavier when centred, where 1px
        // already looked clean. (The histogram lines were missed in the first
        // pass and so alone still hatched until matched up here.) The outer
        // frame keeps kFrameStroke (load-bearing — kInner* derive from it — and
        // already 2px). Tune here.
        constexpr float kChromeLineW    = 1.5f;

        // Inner content rectangle — single source of truth for the
        // layout inset. Both paint() and drawHistoRegion() derive
        // their left/right bounds from these; previously each
        // function recomputed (kOuterPad + kFrameStroke + 4.0f),
        // which was a drift risk if one site got tweaked and the
        // other didn't. The 4.0f is the extra inner padding that
        // separates the chamfered frame stroke from the content.
        constexpr float kInnerL =
            kOuterPad + kFrameStroke + 4.0f;
        constexpr float kInnerT =
            kOuterPad + kFrameStroke + 4.0f;
        constexpr float kInnerR =
            static_cast<float>(kTexW) - kOuterPad - kFrameStroke - 4.0f;
        constexpr float kInnerB =
            static_cast<float>(kTexH) - kOuterPad - kFrameStroke - 4.0f;

        // Bumped from 66 → 90 to fit the new kFontBigNumber (52 px)
        // FPS value AND the kFontTinyLabel (17 px) label above it,
        // plus paragraph-centring margins on both. Without the bump,
        // the FPS glyph's descender clipped at the panel bottom.
        constexpr float kHeaderHeight     = 90.0f;
        constexpr float kFrametimeHeight  = 106.0f;  // strip height ≈ 58 px after the
                                                      // title row (106 − 36 title − 12
                                                      // bottom pad). Bumped 90 → 100 to
                                                      // give the left-hand ms axis room
                                                      // (up to 5 tick labels, e.g. 0/2/4/
                                                      // 6/8 at 144 Hz, at ~10 px spacing),
                                                      // then → 106 to absorb the 12 px
                                                      // reclaimed when kSectionGap
                                                      // tightened 8 → 4 (two panels × 6).
                                                      // kTexH stays 436, budget exact.
        constexpr float kBottomHeight     = 90.0f;   // matches kHeaderHeight: the
                                                      // bottom TEMP/LOAD/VRAM row now
                                                      // reads at the same box height
                                                      // as the FPS header. 4 + label
                                                      // (22), then metric(43) centred
                                                      // in the region BELOW the label
                                                      // (the value rect is anchored at
                                                      // labelY+22 in drawBottomCell,
                                                      // not the full cell) so the
                                                      // 43-px digit stays clear of the
                                                      // caption at this reduced height.

        // Histogram strip metrics — sits inside the frametime panel,
        // below the title row.
        constexpr float kHistoTitleH     = 24.0f;
        // Vertical paddings around the title row inside a frametime
        // panel. drawFrametimePanel() positions the title at
        // (t + kPanelTitleTopPad) and drawHistoRegion() positions
        // the histogram strip at (titleB + kHistoTitleGap). Sharing
        // the constants keeps the histogram aligned under the
        // title — historically these were duplicated 6.0f magic
        // numbers in two functions, with the second one's "any
        // drift here would silently misalign the grid" comment
        // documenting the fragility.
        constexpr float kPanelTitleTopPad = 6.0f;
        constexpr float kHistoTitleGap    = 6.0f;
        constexpr float kHistoBarGap     = 2.0f;
        // Left gutter inside the histogram strip, reserved for the ms-axis
        // tick labels ("0 ms", "5 ms", "10 ms", …). The bg / gridlines /
        // bars all start at histoL + kAxisGutter; the labels are RIGHT-
        // aligned within it so the "ms" suffixes line up and the widest
        // label's leading digit sits at histoL, under the panel title.
        // 44 px holds "XX ms" at 13 px plus a gap before the first bar. The
        // bars keep their fixed 5-px / no-gap geometry, so the gutter costs
        // the oldest ~9 of the 133 samples on the left via the strip's
        // negative-slack clip (see drawPanel) — ≈1.4 s of history instead of
        // 1.5 s.
        constexpr float kAxisGutter      = 44.0f;
        // 133 bars at the fixed 5-px-bar / no-gap layout span 133×5 = 665 px.
        // Since the ms-axis gutter narrows the plot below that, the run no
        // longer fits flush: the negative-slack path in drawPanel keeps the
        // NEWEST bars against the right edge and the scissor clips the oldest
        // few (every bar still pixel-aligned at its 5-px step). Window
        // covered ≈ 1.4 s @ 90 Hz / 0.9 s @ 144 Hz — still short enough to
        // react within ~a second.
        constexpr std::size_t kRingSize  = openxr_api_layer::detail::kOverlayHistoRingSize;
        static_assert(kRingSize == 133,
                       "kOverlayHistoRingSize must match the in-engine ring size; "
                       "bump both in lockstep if tuning the histogram length");

        // Font sizes — calibrated against the design spec's
        // hierarchy. Sizes in pixels at the native texture resolution;
        // rendered to the head-locked quad they read at their natural
        // visual weight in the HMD.
        constexpr float kFontTinyLabel    = 17.0f;  // "FPS", "P95", "TEMP", "VRAM"
        constexpr float kFontSectionTitle = 18.0f;  // "GPU FRAMETIME"
        // Labels and section titles render in Rajdhani SemiBold.
        constexpr float kFontMs           = 18.0f;  // GPU panel "6.7 ms" current value
        constexpr float kFontBigNumber    = 52.0f;  // "142" FPS number — the
                                                     // single biggest text on
                                                     // the HUD, primary anchor.
        constexpr float kFontAccentNumber = 32.0f;  // "138", "124", "108", "98"
        constexpr float kFontTemp         = 43.0f;  // bottom panel TEMP / LOAD /
                                                     // VRAM values
        constexpr float kFontTempUnit     = 19.0f;  // " °C" / " %" / " GB" unit
                                                     // suffix in the bottom panel —
                                                     // a touch larger than the
                                                     // 17 px tiny label sitting
                                                     // above it, so the unit reads
                                                     // as part of the value rather
                                                     // than fading into the caption.

        // Vertical inset for the 1-px column separator lines inside the
        // header's percentile box (the 3 dividers between AVG/P95/P99/
        // P99.9). The footer no longer carries internal separators — each
        // metric is its own framed box now — so only the header inset
        // remains.
        constexpr float kHeaderSepInsetY      =  8.0f;

        // Single source of truth for the overlay's swapchain/RTV formats.
        // Each row pairs an accepted swapchain format with the RTV format we
        // paint through. The GPU shader path writes a logical float4(r,g,b,a)
        // and the output-merger swizzles to the RTV's memory layout, so BGRA8
        // and RGBA8 render identical colours.
        //
        // The RTV must NOT re-encode our already-sRGB UI colours: when the
        // runtime only offers an sRGB swapchain (SteamVR advertises the _SRGB
        // variants and returns a TYPELESS resource), an sRGB RTV would apply
        // linear->sRGB on write, leaving the overlay washed out / light grey.
        // So each sRGB row maps to its UNORM sibling for the RTV — the shader
        // output is stored verbatim and the runtime does the correct sRGB
        // decode when compositing. A UNORM view over the TYPELESS resource is
        // valid; UNORM picks (Pimax, WMR, Oculus, Varjo) map to themselves and
        // keep their exact original path.
        //
        // Rows are in priority order (most-preferred first): linear UNORM
        // ahead of sRGB so we only take an sRGB swapchain when the runtime
        // advertises nothing linear. Keeping the accept list and the RTV map
        // in ONE table means adding a format is one row — they cannot drift.
        struct OverlayFormat {
            int64_t     swapchain;  // an xrEnumerateSwapchainFormats value
            DXGI_FORMAT rtv;        // RTV format we paint through (never sRGB)
        };
        constexpr OverlayFormat kOverlayFormats[] = {
            { static_cast<int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM),      DXGI_FORMAT_B8G8R8A8_UNORM },
            { static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM),      DXGI_FORMAT_R8G8B8A8_UNORM },
            { static_cast<int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB), DXGI_FORMAT_B8G8R8A8_UNORM },
            { static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), DXGI_FORMAT_R8G8B8A8_UNORM },
        };

        // True when the picked swapchain is an sRGB format — i.e. the runtime
        // will sRGB-decode the quad at composite. Drives the RTV-fallback and
        // the diagnostic logs; the text shader's gamma direction also depends
        // on it (see overlay_text_ps.hlsl's DIRECTION CAVEAT).
        bool isSrgbSwapchainFormat(int64_t f) {
            return f == static_cast<int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ||
                   f == static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        }

        // RTV format for a picked swapchain format. Looks the pick up in
        // kOverlayFormats so the sRGB->UNORM mapping has one definition; an
        // unlisted format (shouldn't happen — pick only returns listed ones)
        // falls back to the format itself.
        DXGI_FORMAT rtvFormatForSwapchain(int64_t swapchainFormat) {
            for (const OverlayFormat& e : kOverlayFormats) {
                if (e.swapchain == swapchainFormat) return e.rtv;
            }
            return static_cast<DXGI_FORMAT>(swapchainFormat);
        }

        // Pick the highest-priority format from kOverlayFormats that the
        // runtime advertises. Returns that format on success, 0 on failure
        // (caller logs and degrades). On failure we dump the advertised list
        // so a future unsupported-runtime report is one log line, not a guess.
        int64_t pickSwapchainFormat(OpenXrApi* api, XrSession session) {
            uint32_t count = 0;
            if (XR_FAILED(api->xrEnumerateSwapchainFormats(session, 0, &count, nullptr)) ||
                count == 0) {
                return 0;
            }
            std::vector<int64_t> formats(count);
            if (XR_FAILED(api->xrEnumerateSwapchainFormats(
                    session, count, &count, formats.data()))) {
                return 0;
            }
            // The second call writes `count` entries; trust it over the vector's
            // initial size so the diagnostic below never lists stale trailing 0s.
            formats.resize(count);
            for (const OverlayFormat& want : kOverlayFormats) {
                for (const int64_t f : formats) {
                    if (f == want.swapchain) return want.swapchain;
                }
            }
            // Nothing usable — log what the runtime actually offered so the
            // gate can be widened from a real list rather than a guess.
            std::string advertised;
            for (const int64_t f : formats) {
                if (!advertised.empty()) advertised += ", ";
                advertised += std::to_string(f);
            }
            Log(fmt::format(
                "xr_telemetry: overlay — runtime advertised {} swapchain "
                "format(s): [{}]; none are an accepted BGRA8/RGBA8 (UNORM or "
                "sRGB) target\n",
                formats.size(), advertised));
            return 0;
        }

        // Create the per-image RTV, with a fallback for runtimes that hand
        // back a fully-TYPED sRGB resource instead of TYPELESS. Normally the
        // UNORM `primary` view is what we want (no colour re-encode). But a
        // UNORM view is only legal over a TYPELESS or UNORM resource; a runtime
        // that allocates a typed _SRGB backing would reject it with
        // E_INVALIDARG. In that case retry with the sRGB `fallback` so the
        // overlay still shows (colours may look washed out — logged once) and
        // degrade only if even that fails. Shared by the D3D11 and D3D11On12
        // paths so the two can't drift.
        bool createOverlayImageRtv(ID3D11Device* device, ID3D11Texture2D* tex,
                                   DXGI_FORMAT primary, DXGI_FORMAT fallback,
                                   ID3D11RenderTargetView** out) {
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rtvDesc.Texture2D.MipSlice = 0;
            rtvDesc.Format = primary;
            if (SUCCEEDED(device->CreateRenderTargetView(tex, &rtvDesc, out)))
                return true;
            if (fallback != primary) {
                rtvDesc.Format = fallback;
                if (SUCCEEDED(device->CreateRenderTargetView(tex, &rtvDesc, out))) {
                    Log(fmt::format(
                        "xr_telemetry: overlay — UNORM RTV (format={}) rejected; "
                        "fell back to sRGB RTV (format={}). Resource is typed "
                        "sRGB, not typeless — overlay colours may look washed "
                        "out on this runtime\n",
                        static_cast<int>(primary), static_cast<int>(fallback)));
                    return true;
                }
            }
            return false;
        }

        // -------- GPU text formats ----------------------------------------
        //
        // One GpuTextFormat per IDWriteTextFormat the D2D path uses
        // (face + size + alignment). The atlas builder bakes every
        // (face, sizePx) pair listed here at session init; the
        // GPU-text helpers below resolve glyphs through the same
        // packed key.
        //
        // Alignment is resolved at call time inside drawTextGpu /
        // drawValueTextGpu — the caller passes the rect, the helper
        // measures the run + anchors the pen accordingly. Matches the
        // semantics of IDWriteTextFormat::SetTextAlignment so call
        // sites swap 1:1 once the gpu pointer is wired.
        struct GpuTextFormat {
            enum class Alignment : uint8_t {
                Leading,    // pen anchors at rect.left
                Center,     // run is centred between rect.left / rect.right
                Trailing,   // pen anchors so the run ends at rect.right
            };
            glyph_atlas::GlyphFace face;
            uint16_t               sizePx;
            Alignment              alignment;
        };

        // Mirror the m_fmt* IDWriteTextFormat lineup from
        // CoreRenderer::init. Kept at namespace scope (no member-class
        // qualification needed at the leaf call sites).
        constexpr GpuTextFormat kFmtBigNumberGpu    {glyph_atlas::GlyphFace::RajdhaniUpright, 52, GpuTextFormat::Alignment::Center  };
        constexpr GpuTextFormat kFmtAccentNumberGpu {glyph_atlas::GlyphFace::RajdhaniUpright, 32, GpuTextFormat::Alignment::Center  };
        constexpr GpuTextFormat kFmtTempGpu        {glyph_atlas::GlyphFace::RajdhaniUpright, 43, GpuTextFormat::Alignment::Center  };
        constexpr GpuTextFormat kFmtMsValueGpu     {glyph_atlas::GlyphFace::RajdhaniUpright, 18, GpuTextFormat::Alignment::Trailing };
        constexpr GpuTextFormat kFmtTinyLabelGpu   {glyph_atlas::GlyphFace::RajdhaniUpright, 17, GpuTextFormat::Alignment::Center  };
        constexpr GpuTextFormat kFmtSectionTitleGpu{glyph_atlas::GlyphFace::RajdhaniUpright, 18, GpuTextFormat::Alignment::Leading  };
        // CPU frametime panel: Render + App (the per-cycle total) as one
        // right-aligned compound. 17 px — baked in kSizes (Rajdhani). The
        // two "X.X ms" terms fit the 360 px value rect (see
        // drawFrametimePanel's CPU branch).
        constexpr GpuTextFormat kFmtCpuBreakdownGpu{glyph_atlas::GlyphFace::RajdhaniUpright, 17, GpuTextFormat::Alignment::Trailing };
        // Histogram ms-axis tick labels ("0 ms", "5 ms", "10 ms", …) —
        // small, RIGHT-aligned so the "ms" suffixes line up and the widest
        // label's leading digit lands at histoL, under the section title.
        // Drawn in the dynamic tier (they rescale with target_fps), not the
        // static tier.
        constexpr GpuTextFormat kFmtAxisLabelGpu   {glyph_atlas::GlyphFace::RajdhaniUpright, 13, GpuTextFormat::Alignment::Trailing };

        // -------- GPU text colours ----------------------------------------
        //
        // Pulled from the initBrushes() ColorF table in CoreRenderer so the
        // GPU output reads identically to the D2D output. RGBA arrays match
        // the renderer's TextInstance.color packing (straight-alpha).
        // Pointer-equality on the constant arrays is what lets the GPU text
        // segmentation merge contiguous same-colour runs into one drawRun.
        constexpr float kColorTextWhite[4]  = {0.969f, 0.969f, 0.969f, 1.00f};  // m_brushTextWhite
        constexpr float kColorAccentCyan[4] = {0.098f, 0.820f, 0.851f, 1.00f};  // m_brushAccentCyan
        constexpr float kColorOrange[4]     = {1.000f, 0.553f, 0.000f, 1.00f};  // m_brushOrange
        constexpr float kColorTierRed[4]    = {1.000f, 0.196f, 0.235f, 1.00f};  // red warning tier
        // Muted slate for the histogram ms-axis tick labels — readable but
        // subdued so the numbers sit behind the bars in the visual
        // hierarchy (brighter than the kGridLine lines they label).
        constexpr float kColorAxisLabel[4]  = {0.490f, 0.541f, 0.561f, 0.92f};

        // -------- GPU chrome-shape colours ---------------------------------
        //
        // Same source-of-truth as the brush colours in initBrushes(); the
        // ChromeShapeRenderer reads these as straight-alpha RGBA.
        constexpr float kColorBg[4]        = {0.020f, 0.024f, 0.024f, 0.94f};  // m_brushBg (frame fill)
        constexpr float kColorPanelBg[4]   = {0.035f, 0.039f, 0.039f, 1.00f};  // m_brushPanelBg
        constexpr float kColorFrameLine[4] = {0.122f, 0.133f, 0.133f, 1.00f};  // m_brushFrameLine (frame stroke)
        constexpr float kColorSeparator[4] = {0.184f, 0.200f, 0.204f, 1.00f};  // m_brushSeparator (panel & cell strokes)

        // -------- CoreRenderer: DirectWrite + glyph-atlas owner -------------
        //
        // Shared between the D3D11 and D3D12 paths. Owns the DirectWrite
        // factory + the bundled-font collection, and bakes the glyph atlas
        // once at init (the GPU renderers copy from its BuildResult). It
        // also holds drawChrome + the chrome sub-helpers, which emit onto
        // the per-frame GPU renderers (m_textRenderer / m_chromeShapes)
        // the caller stashes around a chrome rebuild. No D2D, no per-frame
        // GPU resources of its own.
        class CoreRenderer {
          public:
            // -------- Public lifecycle --------------------------------------
            //
            // Destructor unregisters the bundled-font loader before the
            // ComPtr drops the last reference, so the shared
            // IDWriteFactory's internal loader table never holds a
            // dangling pointer across sessions. Order of destruction:
            // m_customFontCollection → m_bundledFontFiles → loader
            // (here, explicit unregister) → m_dwriteFactory.
            ~CoreRenderer() {
                if (m_inMemoryFontLoader && m_dwriteFactory) {
                    m_dwriteFactory->UnregisterFontFileLoader(
                        m_inMemoryFontLoader.Get());
                }
            }

            // `ss` is the overlay supersample factor (see kOverlaySupersample).
            // It only affects the glyph-atlas bake size — the atlas is
            // rasterized at the PHYSICAL (ss-scaled) sizes so the GPU text
            // renderer can sample it 1:1 at the higher render resolution.
            // Defaults to 1.0 so the snapshot/golden path (which calls init()
            // with no argument) bakes the legacy sizes and stays byte-stable.
            bool init(float ss = 1.0f) {
                if (FAILED(::DWriteCreateFactory(
                        DWRITE_FACTORY_TYPE_SHARED,
                        __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
                    return false;
                }

                // Load the BUNDLED Rajdhani font (SemiBold upright; the
                // atlas bakes the printable-ASCII set, which covers both
                // the labels and the value digits). The TTF lives as an
                // RT_BUNDLED_FONT resource in the DLL (see
                // fonts/bundled_fonts.rc.inc); we feed its bytes into an
                // IDWriteInMemoryFontFileLoader and build a custom
                // IDWriteFontCollection around it. The glyph atlas builder
                // (buildGlyphAtlas below) resolves the face through this
                // collection by family name, never falling back to system
                // fonts.
                //
                // One typeface for everything: the value digits (FPS,
                // frametimes, °C, %, ms) and the labels / section titles
                // all render in Rajdhani SemiBold UPRIGHT, with hierarchy
                // from size (52 px big number vs 17 px labels) and colour,
                // not a second font. The atlas bakes a single GlyphFace
                // (RajdhaniUpright) — there is no separate digit face.
                //
                // On failure (resource missing, factory QI fails,
                // collection creation fails), we proceed without the custom
                // collection and let the atlas builder resolve against the
                // system default — Bahnschrift on Win10+, also upright.
                // Graceful degrade — never crash the host process for a
                // cosmetic font.
                const wchar_t* kFamilyLabels = L"Rajdhani";  // labels, titles, digits
                IDWriteFontCollection* customCollection = nullptr;
                if (!loadBundledFontCollection(m_dwriteFactory.Get(),
                                                 m_customFontCollection)) {
                    // Fallback: leave m_customFontCollection null and let
                    // DirectWrite resolve against system fonts — Bahnschrift
                    // on Win10+, upright like Rajdhani. The face requests
                    // SEMI_BOLD upright, so the fallback is a clean upright
                    // substitute. Graceful degrade — a cosmetic font miss
                    // never crashes the host.
                    kFamilyLabels = L"Bahnschrift";
                } else {
                    customCollection = m_customFontCollection.Get();
                }

                // Bake the glyph atlas. Soft-failure path: on a build
                // miss m_atlasReady stays false and the callers (D3D11
                // / D3D12 renderers, snapshot test) fail-close — the
                // overlay is disabled rather than crashing the host.
                // There is no D2D fallback anymore (Task 17 removed it).
                if (!buildGlyphAtlas(kFamilyLabels, customCollection, ss)) {
                    Log("xr_telemetry: glyph atlas build failed — "
                        "overlay will be disabled\n");
                    m_atlasReady = false;
                } else {
                    m_atlasReady = true;
                }
                return true;
            }

            // -------- Glyph-atlas accessors ---------------------------------
            //
            // The atlas is built once at init from the same
            // IDWriteFactory + custom collection the IDWriteTextFormat
            // pipeline uses, so it shares the exact font cuts with the
            // D2D path. Both overlay renderers (D3D11 + D3D12) copy
            // from this BuildResult into their own ID3D11Device-bound
            // atlas texture via glyph_atlas::Renderer::init.
            const glyph_atlas::BuildResult& atlas() const noexcept { return m_atlas; }
            bool atlasReady() const noexcept { return m_atlasReady; }

            // Free the CPU-side atlas bitmap once a renderer has uploaded it to
            // its GPU texture (CreateTexture2D copies the bytes). Only the glyph
            // renderer consumes the bitmap; the glyph + metrics tables it also
            // needs are copied separately and stay intact. At 2.0× the bitmap
            // is an 8 MB R8 buffer that would otherwise sit resident in every
            // host game process for its lifetime — drop it. Idempotent.
            void releaseAtlasBitmap() noexcept {
                m_atlas.bitmap.clear();
                m_atlas.bitmap.shrink_to_fit();
            }

            // Rebuild the chrome instance batches from the current
            // snapshot. Pure CPU work (NO GPU submission): populates the
            // renderers' scratch vectors via drawChrome, in up to two
            // tiers gated by m_tier (see the m_tier member doc):
            //   * Static  (labels / titles + chrome shapes) — laid out
            //     once and cached, re-baked only when the structure key
            //     changes. The ~50-glyph label layout + the shape emission
            //     happen here, off the per-bump path.
            //   * Dynamic (the changing values) — re-laid-out every call,
            //     into the glyph renderer's separate dynamic scratch.
            // The caller flushes the combined scratches into the target
            // RTV later, every frame, even between chrome-cadence ticks.
            // That's what makes direct-to-swapchain affordable: the value
            // rebuild stays cadenced (~10 Hz) and now skips the cached
            // static work, while the cheap map+DrawInstanced fires per
            // frame against whichever of the N swapchain images we get.
            //
            // Both gpuText / gpuShapes must be non-null on the D3D11
            // direct-to-swapchain path (Task 15) — D2D fallback was
            // retired with the shim. Returns true on success; false
            // only if the renderer pointers are null.
            bool paintChromeOnly(glyph_atlas::Renderer*       gpuText,
                                  chrome_shapes::Renderer*     gpuShapes,
                                  const OverlaySnapshot& snap) {
                if (!gpuText || !gpuShapes) return false;
                m_textRenderer  = gpuText;
                m_chromeShapes  = gpuShapes;

                // RAII guard: nulls the transient renderer pointers on
                // every exit path, including stack unwind through a
                // std::bad_alloc inside drawChrome (drawValueTextGpu
                // allocates a vector<Segment>, etc.). Without the
                // guard a throw would leave the pointers dangling at
                // member values until the next paint reassigned them
                // — not a UAF since the pointed-to objects outlive
                // the paint, but sloppy in a "never crash the host"
                // layer.
                struct PaintGuard {
                    CoreRenderer* core;
                    ~PaintGuard() {
                        if (core) {
                            core->m_textRenderer = nullptr;
                            core->m_chromeShapes = nullptr;
                        }
                    }
                } _guard{this};

                // Format the display strings once; both tiers read them
                // (the static tier ignores the values, the dynamic tier
                // lays them out).
                m_cachedValues = formatOverlayDisplayValues(snap);

                // Re-bake the static tier only when its structural
                // signature changes (see structureSignature). vram_pct is
                // presently never empty ("--" placeholder), so in practice
                // this bakes exactly once; the check is cheap insurance.
                const bool structure = structureSignature(m_cachedValues);
                const bool rebakeStatic =
                    !m_staticBaked || structure != m_staticStructure;

                // ---- Static tier (labels + chrome shapes) ----------------
                // Laid out once and cached: drawChrome's label / title /
                // panel-background / separator emitters are gated to this
                // tier, so re-running the full traversal here populates ONLY
                // the static scratch of the glyph renderer and (re)builds
                // the chrome-shape batch. Both then persist untouched across
                // subsequent version bumps — flush() re-uploads them every
                // frame for free, with no re-layout.
                if (rebakeStatic) {
                    m_tier = PaintTier::Static;
                    gpuText->beginStaticBatch();
                    gpuShapes->beginBatch();
                    drawChrome(m_cachedValues);
                    m_staticBaked     = true;
                    m_staticStructure = structure;
                }

                // ---- Dynamic tier (the changing values) ------------------
                // Runs every version bump. Only the value emitters fire
                // (the static leaves no-op under PaintTier::Dynamic), so we
                // re-lay-out just the digits — the ~10 Hz spike this split
                // exists to shrink. The chrome-shape batch is left alone:
                // its scratch persists from the last bake.
                m_tier = PaintTier::Dynamic;
                gpuText->beginDynamicBatch();
                drawChrome(m_cachedValues);
                return true;
            }

          private:
            // The static-tier drawing: outer frame, header bar, both
            // frametime-panel chromes (background + title + current
            // value, NO histogram — the bars renderer owns the histo
            // region), and the bottom TEMP/LOAD/VRAM row. All emitted
            // onto the m_chromeShapes / m_textRenderer batches. Panel
            // Ys derive from the namespace-scope layout constants so
            // they can't drift from the bar renderer's geometry.
            void drawChrome(const OverlayDisplayValues& v) {
                const float headerY   = kInnerT;
                const float gpuPanelY = headerY + kHeaderHeight + kSectionGap;
                const float cpuPanelY = gpuPanelY + kFrametimeHeight + kSectionGap;
                const float bottomY   = cpuPanelY + kFrametimeHeight + kSectionGap;

                // Outer frame: a rounded filled rect + 1-px rounded
                // border (the 12-px chamfer was dropped in the D2D→GPU
                // migration; the quad shader's SDF path now rounds the
                // corners — see chrome_shape_renderer.h). drawChamferedRect
                // keeps the historical name + signature so the call still
                // reads as "the outer frame".
                const D2D1_RECT_F frameOuter = D2D1::RectF(
                    kOuterPad, kOuterPad,
                    static_cast<float>(kTexW) - kOuterPad,
                    static_cast<float>(kTexH) - kOuterPad);
                drawChamferedRect(frameOuter, kFrameStroke);

                drawHeaderBar(kInnerL, headerY, kInnerR,
                               headerY + kHeaderHeight, v);
                // Histogram content is owned by HistogramBarRenderer;
                // these panel calls only paint the chrome (bg + title +
                // current value).
                drawFrametimePanel(kInnerL, gpuPanelY, kInnerR,
                                    gpuPanelY + kFrametimeHeight,
                                    L"GPU FRAMETIME",
                                    v.gpu_frametime_ms,
                                    /*breakdown=*/std::string{},
                                    v.target_fps);
                // CPU panel: build the Render term here; drawFrametimePanel
                // appends the per-cycle total (labelled "App") and renders
                // the pair as one right-aligned compound.
                const std::string cpuBreakdown =
                    "Render " + v.cpu_render_ms + " ms";
                drawFrametimePanel(kInnerL, cpuPanelY, kInnerR,
                                    cpuPanelY + kFrametimeHeight,
                                    L"CPU FRAMETIME",
                                    v.cpu_frametime_ms,
                                    cpuBreakdown,
                                    v.target_fps);

                // Bottom row: 5 individually-framed cells (was two
                // multi-cell panels split 60/40, GPU-side + CPU-side). Each
                // metric now gets its own framed box, all five divided by
                // kSectionGap — the same gap the frametime panels use
                // vertically. Order stays GPU-side first (TEMP / LOAD /
                // VRAM) then CPU-side (CPU LOAD / CPUs LOAD). Tier colours
                // follow tierForUtilisation; the TEMP cell stays white (no
                // thermal tier). See BottomCell / drawBottomCell.
                using Cell = BottomCell;
                constexpr int kBottomCellCount = 5;
                const Cell bottomCells[kBottomCellCount] = {
                    Cell{L"GPU TEMP",  v.gpu_temp_c,   BottomCellKind::Temp,    0.0f},
                    Cell{L"GPU LOAD",  v.gpu_util_pct, BottomCellKind::Percent, v.gpu_util_fraction},
                    Cell{L"VRAM",      v.vram_pct,     BottomCellKind::Percent, v.vram_fraction},
                    Cell{L"CPU LOAD",  v.cpu_util_pct, BottomCellKind::Percent, v.cpu_util_fraction},
                    Cell{L"CPUs LOAD", v.cpus_max_pct, BottomCellKind::Percent, v.cpus_max_fraction},
                };
                // 5 boxes + 4 inter-box gaps span the inner width.
                const float bottomGapTotal =
                    kSectionGap * static_cast<float>(kBottomCellCount - 1);
                const float bottomCellW =
                    (kInnerR - kInnerL - bottomGapTotal) /
                    static_cast<float>(kBottomCellCount);
                for (int i = 0; i < kBottomCellCount; ++i) {
                    const float cellL = kInnerL +
                        static_cast<float>(i) * (bottomCellW + kSectionGap);
                    // Last box clamps to the inner-right edge so FP rounding
                    // of bottomCellW can't leave a seam at the frame border.
                    const float cellR = (i + 1 == kBottomCellCount)
                        ? kInnerR
                        : cellL + bottomCellW;
                    drawBottomCell(cellL, bottomY, cellR,
                                    bottomY + kBottomHeight, bottomCells[i]);
                }
            }

            // -------- end static-tier helpers --------

          private:

            // -------- Glyph-atlas BuildSpec assembly + bake -----------------
            //
            // Bakes Rajdhani SemiBold + Rajdhani SemiBold (with system
            // Bahnschrift fallback) at every pixel size any IDWriteTextFormat
            // above uses. The charset is wide enough to cover everything the
            // overlay ever renders — see findValueRuns / drawChrome /
            // drawBottomCell for the actual call sites:
            //
            //   Rajdhani upright (kFamilyLabels): ASCII 0x20..0x7E (the
            //     97-char printable range, conservatively wide so we don't
            //     re-bake every time a label string changes) + ° (U+00B0)
            //     for the temperature unit suffix. The printable range
            //     already covers the value digits (0-9), '.' (appears
            //     between digits in "12.34" / "6.7 ms"), and the leading
            //     '-', so the numbers bake from the same set as the labels
            //     — no separate digit face.
            //
            // Sizes are the union of every kFont* constant used in
            // makeFormat() above: 17 (tiny label), 18 (ms / section title),
            // 32 (accent number), 43 (temp), 52 (big FPS) — plus 19
            // (kFontTempUnit) for the GPU-text path. The D2D path
            // pulls 19 via an IDWriteTextLayout::SetFontSize override
            // on the unit range, but the GPU renderer can't override
            // size per-glyph: each (face, sizePx) lives in the atlas
            // as its own raster, so the unit suffix run draws at 19
            // through a separate drawRun call. Baking the size adds
            // ~3 KB of atlas pixels and removes a layout-divergence
            // gotcha between paths.
            bool buildGlyphAtlas(const wchar_t*         familyLabels,
                                  IDWriteFontCollection* collection,
                                  float                  ss) {
                // LOGICAL design-pixel sizes. 13 px is the histogram ms-axis
                // tick labels (small, dim, right-aligned in the left gutter);
                // the rest are the pre-existing HUD sizes (the union of every
                // kFont* / kFmt*Gpu constant). RajdhaniUpright 13 carries the
                // digits the axis needs (and the full ASCII set, like the
                // others). Each size is baked at its PHYSICAL size (logical ×
                // ss, see supersampledSizePx) so the GPU text renderer — which
                // maps the same logical size to the same physical size at
                // lookup — samples each glyph 1:1 at the render resolution. At
                // ss == 1 these bake unchanged.
                static constexpr uint16_t kSizes[] = {13, 17, 18, 19, 32, 43, 52};

                std::vector<wchar_t> rajdhaniSet;
                rajdhaniSet.reserve(97 + 1);
                for (wchar_t c = 0x20; c <= 0x7E; ++c) rajdhaniSet.push_back(c);
                rajdhaniSet.push_back(static_cast<wchar_t>(0x00B0));  // °

                glyph_atlas::BuildSpec spec{};
                spec.dwriteFactory  = m_dwriteFactory;
                // Custom collection may be null on the system-fallback
                // path; resolveFace inside glyph_atlas::build handles
                // both cases.
                spec.fontCollection = collection;
                spec.familyLabels   = familyLabels;
                // 1024×2048 R8 = 2 MB at ss == 1 — fits all 7 sizes of the
                // full ASCII set (one face, Rajdhani) with comfortable slack
                // (1024×1024 overflowed shelf-pack back when a second face was
                // baked alongside this one; one face leaves ample room now).
                // Scale BOTH dims by the factor so the budget tracks glyph
                // area (which grows ~ss²) and the shelf-pack keeps the same
                // slack ratio: 1.5x → 1536×3072, 2.0x → 2048×4096 (~4× area),
                // through the same rounding helper as everything else. The
                // bitmap lives only until the GPU texture is created from it —
                // CoreRenderer::releaseAtlasBitmap() frees it once the glyph
                // renderer has uploaded it, so the R8 buffer (8 MB at 2.0×)
                // isn't resident for the host process's lifetime; zero
                // per-frame cost. ss == 1 keeps the legacy 1024×2048
                // (golden-stable).
                //
                // NOTE: shelf-pack success at the shipped factor still needs a
                // Windows build to confirm (build() returns false on overflow
                // → m_atlasReady=false → HUD disabled); the proportional
                // budget makes overflow unlikely but isn't a static proof.
                spec.atlasWidthPx   = glyph_atlas::supersampledSizePx(1024, ss);
                spec.atlasHeightPx  = glyph_atlas::supersampledSizePx(2048, ss);
                spec.padding        = 1;

                spec.requests.reserve(_countof(kSizes));
                for (uint16_t sz : kSizes) {
                    const uint16_t physSz =
                        glyph_atlas::supersampledSizePx(sz, ss);
                    spec.requests.push_back({glyph_atlas::GlyphFace::RajdhaniUpright,
                                              physSz, rajdhaniSet});
                }

                if (!glyph_atlas::build(spec, m_atlas)) return false;
                if (m_atlas.missingGlyphs > 0) {
                    Log(fmt::format(
                        "xr_telemetry: glyph atlas built with {} missing "
                        "glyphs — those characters will fall back to "
                        "their face's space advance at draw time\n",
                        m_atlas.missingGlyphs));
                }
                return true;
            }

            // Loads the bundled font resource (RT_BUNDLED_FONT, ID
            // 200 in fonts/bundled_fonts.rc.inc — the shared include
            // compiled into both the layer DLL and the test EXE so
            // loadBundledFontCollection finds the same bytes in either
            // binary) into a custom IDWriteFontCollection.
            // The collection holds one family:
            //   ID 200 → Rajdhani SemiBold (labels, titles, AND the value
            //            chiffres — one font for everything)
            // DirectWrite reads the family + style from each TTF's
            // `name` table, so the loader code below is family-
            // agnostic and just adds the two files to the same set.
            //
            // Uses IDWriteFactory5's in-memory font-file-loader API (Windows
            // 10 1703+) to avoid the boilerplate of implementing
            // IDWriteFontFileLoader / IDWriteFontFileStream /
            // IDWriteFontCollectionLoader by hand.
            //
            // Returns true on success and writes the collection into
            // `outCollection`. False = caller falls back to system fonts.
            // Never throws. Never crashes the host process if any DirectWrite
            // call fails — the layer is loaded into VR games and the renderer
            // is best-effort.
            bool loadBundledFontCollection(IDWriteFactory* dwriteFactory,
                                             ComPtr<IDWriteFontCollection>& outCollection) {
                if (!dwriteFactory) return false;

                // IDWriteFactory5 was added in the Windows 10 Creators
                // Update (1703). The QueryInterface returns S_OK on
                // Win10 1703+ and E_NOINTERFACE on older platforms.
                ComPtr<IDWriteFactory5> factory5;
                if (FAILED(dwriteFactory->QueryInterface(
                        __uuidof(IDWriteFactory5),
                        reinterpret_cast<void**>(factory5.GetAddressOf())))) {
                    return false;
                }

                // Find the font resource in the DLL. The trick
                // is using GetModuleHandleEx with the ADDRESS of a
                // function INSIDE our own DLL — that resolves to
                // our DLL's HMODULE (not the host EXE's). Without
                // this we'd accidentally look for resources in the
                // game's executable. pickSwapchainFormat is a free
                // function in this TU's anonymous namespace; taking
                // its address gives us a guaranteed in-DLL pointer.
                HMODULE hMod = nullptr;
                if (!::GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(&pickSwapchainFormat),
                        &hMod)) {
                    return false;
                }

                // Create an in-memory font file loader and register it.
                ComPtr<IDWriteInMemoryFontFileLoader> inMemoryLoader;
                if (FAILED(factory5->CreateInMemoryFontFileLoader(
                        inMemoryLoader.GetAddressOf()))) {
                    return false;
                }
                if (FAILED(factory5->RegisterFontFileLoader(inMemoryLoader.Get()))) {
                    return false;
                }
                // Cache the loader so we can unregister it at
                // destruction (avoids a process-wide leak across
                // many overlay-renderer create/destroy cycles).
                m_inMemoryFontLoader = inMemoryLoader;

                // Helper: pull a single RT_BUNDLED_FONT resource into the loader.
                constexpr WORD RT_BUNDLED_FONT_W = 256;
                auto addResourceFont = [&](WORD resourceId) -> bool {
                    HRSRC res = ::FindResourceW(
                        hMod, MAKEINTRESOURCEW(resourceId),
                        MAKEINTRESOURCEW(RT_BUNDLED_FONT_W));
                    if (!res) return false;
                    HGLOBAL global = ::LoadResource(hMod, res);
                    if (!global) return false;
                    const void* data = ::LockResource(global);
                    const DWORD size = ::SizeofResource(hMod, res);
                    if (!data || size == 0) return false;

                    ComPtr<IDWriteFontFile> fontFile;
                    if (FAILED(inMemoryLoader->CreateInMemoryFontFileReference(
                            factory5.Get(), data, size,
                            /*ownerObject=*/nullptr,
                            fontFile.GetAddressOf()))) {
                        return false;
                    }
                    m_bundledFontFiles.push_back(std::move(fontFile));
                    return true;
                };

                // 200 = Rajdhani SemiBold — the only bundled font now
                // (serves both the labels and the value chiffres).
                if (!addResourceFont(200)) return false;

                // Build the font set from the loaded files. We use
                // IDWriteFontSetBuilder1 (the extended interface)
                // because AddFontFile lives there — the base
                // IDWriteFontSetBuilder only exposes
                // AddFontFaceReference, which would require us to
                // create a face reference per file via the factory
                // (more boilerplate). IDWriteFactory5::CreateFontSet
                // Builder returns the v1 builder directly (the
                // override hides the inherited v0 signature).
                ComPtr<IDWriteFontSetBuilder1> setBuilder;
                if (FAILED(factory5->CreateFontSetBuilder(setBuilder.GetAddressOf()))) {
                    return false;
                }
                for (auto& f : m_bundledFontFiles) {
                    // Analyze validates the file is a recognised
                    // font format before we add it to the set;
                    // AddFontFile internally handles all faces in
                    // the file (single face per file for our subset —
                    // Rajdhani-SemiBold.ttf is one upright face in the
                    // Rajdhani family — but the
                    // API is robust either way; DirectWrite reads
                    // each file's `name` table to know which family /
                    // weight / style face it contains).
                    BOOL supported = FALSE;
                    DWRITE_FONT_FILE_TYPE fileType = DWRITE_FONT_FILE_TYPE_UNKNOWN;
                    DWRITE_FONT_FACE_TYPE faceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
                    UINT32 numFaces = 0;
                    if (SUCCEEDED(f->Analyze(&supported, &fileType, &faceType, &numFaces)) &&
                        supported && numFaces > 0) {
                        setBuilder->AddFontFile(f.Get());
                    }
                }
                ComPtr<IDWriteFontSet> fontSet;
                if (FAILED(setBuilder->CreateFontSet(fontSet.GetAddressOf()))) {
                    return false;
                }
                ComPtr<IDWriteFontCollection1> collection1;
                if (FAILED(factory5->CreateFontCollectionFromFontSet(
                        fontSet.Get(), collection1.GetAddressOf()))) {
                    return false;
                }
                // QI down to IDWriteFontCollection — that's the type
                // CreateTextFormat accepts.
                if (FAILED(collection1.As(&outCollection))) {
                    return false;
                }
                return true;
            }

            // -------- GPU text leaf helpers -------------------------------
            //
            // Thin forwarders onto the glyph-atlas renderer. The chrome
            // path is GPU-only now (the D2D fallback was retired with the
            // shim in Task 15), so each helper takes the GpuTextFormat +
            // straight-alpha colour directly — no IDWriteTextFormat /
            // ID2D1Brush pointer indirection, no D2D DrawText. drawChrome
            // only runs inside paintChromeOnly, which guarantees
            // m_textRenderer is set; the guard is belt-and-braces.
            // ===== Chrome leaf emitters — TIER CONTRACT ===================
            // drawChrome is walked once per tier (see paintChromeOnly); each
            // leaf emits ONLY in its owning tier and no-ops in the other:
            //   * Static  : labels / titles (drawLabel) + shapes
            //               (drawPanelBg / drawChamferedRect /
            //                drawColumnSeparators)
            //   * Dynamic : values (drawAscii / drawValueWide / drawValueAscii)
            // There is intentionally NO ungated wide-text primitive: every
            // text path runs through one of the gated entries below, which
            // forward to the low-level drawTextGpu / drawValueTextGpu GPU
            // emitters. A new chrome element therefore can't silently land
            // in both tiers (or the wrong one) — it MUST pick a tier-tagged
            // entry here, and that choice states its tier in exactly one
            // place.

            // Static-tier label / title emitter. Fires ONLY during the
            // cached static bake (PaintTier::Static), never during a
            // per-bump value rebuild — label strings are compile-time
            // literals (cell captions, panel titles) that never change,
            // so their glyph layout is computed once and reused.
            void drawLabel(const GpuTextFormat& fmt, const wchar_t* s,
                            const D2D1_RECT_F& rect, const float color[4]) const {
                if (m_tier != PaintTier::Static || !s) return;
                drawTextGpu(m_textRenderer, fmt, s, std::wcslen(s), rect, color);
            }

            // Dynamic-tier single-style value emitter (ASCII widened to
            // wide). VALUE text — re-laid-out every version bump.
            void drawAscii(const GpuTextFormat& fmt, const std::string& s,
                            const D2D1_RECT_F& rect, const float color[4]) const {
                if (m_tier != PaintTier::Dynamic || s.empty()) return;
                const std::wstring wide(s.begin(), s.end());
                drawTextGpu(m_textRenderer, fmt, wide.c_str(), wide.size(),
                             rect, color);
            }

            // A "value run" inside a rendered string: a value-shaped
            // prefix (e.g. "67", "4.3", "--", "--.-") followed by an
            // optional unit suffix (e.g. " ms", " °C", " %", " GB").
            // Detected by findValueRuns and consumed by drawValueWide /
            // drawValueAscii to apply per-range font / brush / size
            // overrides.
            //
            // Two independently-applied sub-ranges:
            //   unit[Start,Len]    leading-space + unit chars; receives
            //                      the optional `unitFontSize` override.
            //                      Empty when the prefix has no unit
            //                      suffix (pure-digit "142" FPS string).
            //   brush[Start,Len]   whole value (prefix + unit); receives
            //                      the optional accent brush. This is
            //                      what lets the CPU compound paint "X.X
            //                      ms" entirely in cyan including the
            //                      upright " ms", while "Render" / " /
            //                      App " labels stay in the base
            //                      brush (white).
            struct ValueRun {
                UINT32 brushStart;
                UINT32 brushLen;
                UINT32 unitStart;
                UINT32 unitLen;
            };

            // Detect value runs inside `s`. A value run's prefix is a
            // maximal contiguous span of [-0-9.] — digits, the decimal
            // dot, and the dash used by the "--" / "--.-" placeholders.
            // The optional unit suffix is " " followed by 1+ unit-shape
            // characters: letters, '%', or '°' (the latter survives in
            // wstring; ASCII strings never carry it). A prefix-only
            // span (no unit) is emitted as a value run only when it
            // contains at least one digit — otherwise stray dashes or
            // dots in label copy ("Render", " / App ") would falsely
            // pull into a value run. A unit-only span never happens
            // because we walk the prefix first.
            //
            // The chiffres sub-range is the smallest range covering all
            // digits inside the prefix (digits + interior dots, but
            // not surrounding dashes). For "-5.2" the chiffres range is
            // "5.2" — the leading dash stays upright Rajdhani.
            template <typename CharT>
            static std::vector<ValueRun>
            findValueRuns(const std::basic_string<CharT>& s) {
                auto isDigit = [](CharT c) {
                    return c >= CharT('0') && c <= CharT('9');
                };
                auto isValuePrefix = [&](CharT c) {
                    return isDigit(c)
                        || c == CharT('.')
                        || c == CharT('-');
                };
                auto isUnitChar = [](CharT c) {
                    return (c >= CharT('a') && c <= CharT('z'))
                        || (c >= CharT('A') && c <= CharT('Z'))
                        ||  c == CharT('%')
                        ||  c == static_cast<CharT>(0xB0); // °
                };

                std::vector<ValueRun> runs;
                const UINT32 n = static_cast<UINT32>(s.size());
                UINT32 i = 0;
                while (i < n) {
                    while (i < n && !isValuePrefix(s[i])) ++i;
                    if (i >= n) break;

                    const UINT32 prefixStart = i;
                    bool hasDigit = false;
                    while (i < n && isValuePrefix(s[i])) {
                        if (isDigit(s[i])) hasDigit = true;
                        ++i;
                    }
                    const UINT32 prefixEnd = i;

                    // Optional unit: " " + 1+ unit chars.
                    UINT32 unitStart = prefixEnd;
                    UINT32 unitLen   = 0;
                    if (i < n && s[i] == CharT(' ')) {
                        UINT32 j = i + 1;
                        while (j < n && isUnitChar(s[j])) ++j;
                        if (j > i + 1) {
                            unitStart = i;
                            unitLen   = j - i;
                            i = j;
                        }
                    }

                    if (hasDigit || unitLen > 0) {
                        ValueRun run{};
                        run.brushStart  = prefixStart;
                        run.brushLen    = (unitStart + unitLen) - prefixStart;
                        run.unitStart   = unitStart;
                        run.unitLen     = unitLen;
                        runs.push_back(run);
                    }
                }
                return runs;
            }

            // Render a value string with mixed per-range styling.
            //
            // Digit characters flip to Rajdhani SemiBold; the rest
            // (including unit suffixes like " ms" / " °C" / " %") stays
            // in `baseFmt`'s family + style — typically Rajdhani
            // SemiBold upright. Auto-detects digit and unit ranges from
            // the string content via findValueRuns.
            //
            // The base `color` paints non-value characters. When
            // `chiffresColor` is non-null, BOTH the digit and unit
            // portions of every value run flip to it — used by the CPU
            // compound so "X.X ms" reads in cyan while "Render" / " /
            // App " labels stay in white. When `unitFontSize > 0`,
            // the unit portion of every value run shrinks to that size
            // while the digit portion stays at the base size — used by
            // the bottom panel to render " °C" / " %" at the small
            // label size next to the big-number digit prefix.
            //
            // Mixed-style value rendering — thin forwarder onto the GPU
            // value emitter. Walks findValueRuns inside drawValueTextGpu
            // to flip digit ranges to Rajdhani, optionally shrink
            // the unit suffix, and optionally recolour the value runs
            // (chiffresColor). `unitFontSize` stays float at the call
            // sites (it's a kFont* constant); the atlas is keyed on
            // uint16_t pixel sizes so we round here.
            void drawValueWide(const GpuTextFormat& fmt,
                                const std::wstring& wide,
                                const D2D1_RECT_F& rect,
                                const float color[4],
                                const float* chiffresColor = nullptr,
                                float unitFontSize = 0.0f) const {
                if (m_tier != PaintTier::Dynamic) return;   // VALUE text
                if (!m_textRenderer || wide.empty()) return;
                const uint16_t usize = static_cast<uint16_t>(unitFontSize + 0.5f);
                drawValueTextGpu(m_textRenderer, fmt, wide.c_str(), wide.size(),
                                  rect, color, chiffresColor, usize);
            }

            // ASCII overload — byte-widens to wstring and forwards.
            // Caller contract: `s` is ASCII-only (same as drawAscii).
            void drawValueAscii(const GpuTextFormat& fmt,
                                 const std::string& s,
                                 const D2D1_RECT_F& rect,
                                 const float color[4],
                                 const float* chiffresColor = nullptr,
                                 float unitFontSize = 0.0f) const {
                if (m_tier != PaintTier::Dynamic) return;   // VALUE text
                if (s.empty()) return;
                drawValueWide(fmt, std::wstring(s.begin(), s.end()), rect,
                               color, chiffresColor, unitFontSize);
            }

            // -------- Low-level GPU text emitters -------------------------
            //
            // The leaves drawLabel / drawAscii / drawValueWide / drawValue
            // Ascii forward here; these own the anchor + baseline math.
            //
            // Anchor + baseline-positioning math matches D2D's
            // ParagraphAlignment(CENTER) + TextAlignment(LEADING/CENTER/
            // TRAILING) so the GPU and D2D paths render the same string
            // at the same screen position (modulo subpixel AA). The
            // single-face helper is a special case of the value helper
            // (zero value runs); they share the baseline math.
            //
            // Baseline derivation: D2D positions a 1-line block so the
            // block's vertical centre aligns with the rect's centre.
            // Block extends from (baseline - ascent) to (baseline +
            // descent), so the centre is at baseline + (descent -
            // ascent)/2. Setting that equal to rect_centre_y gives
            // baseline = rect_centre + (ascent - descent)/2.
            void drawTextGpu(glyph_atlas::Renderer* r,
                              const GpuTextFormat& fmt,
                              const wchar_t* s, std::size_t n,
                              const D2D1_RECT_F& rect,
                              const float color[4]) const {
                if (!r || n == 0 || !s) return;
                const float totalW = r->measure(fmt.face, fmt.sizePx, s, n);
                float penX = rect.left;
                switch (fmt.alignment) {
                case GpuTextFormat::Alignment::Leading:
                    penX = rect.left; break;
                case GpuTextFormat::Alignment::Center:
                    penX = (rect.left + rect.right - totalW) * 0.5f; break;
                case GpuTextFormat::Alignment::Trailing:
                    penX = rect.right - totalW; break;
                }
                const auto* m = r->metrics(fmt.face, fmt.sizePx);
                const float ascent  = m ? m->ascent  : static_cast<float>(fmt.sizePx) * 0.75f;
                const float descent = m ? m->descent : static_cast<float>(fmt.sizePx) * 0.25f;
                const float baselineY = (rect.top + rect.bottom + ascent - descent) * 0.5f;
                r->drawRun(fmt.face, fmt.sizePx, penX, baselineY, s, n, color);
            }

            // Mixed-style value rendering — the GPU equivalent of
            // drawValueWide. Walks findValueRuns to identify the unit
            // range and the value extent per run, then segments the
            // string into maximal runs of constant (face, sizePx,
            // color) attributes. One drawRun call per segment. (Face is
            // uniform today — a single Rajdhani cut — so segments split
            // only on size / colour.)
            //
            // `chiffresColor` (optional): when non-null, every value
            // run's brush range (digits + unit) flips to this colour.
            // Used by the CPU compound "Render {x} ms / App {y} ms"
            // so labels stay white, values cyan.
            //
            // `unitSizePx` (optional): when > 0, the unit range of
            // every value run renders at this size; the digits and
            // surrounding base text stay at baseFmt.sizePx. Used by
            // the bottom panel to render "67 °C" / "92 %" with a
            // smaller °C / % than the digit prefix.
            //
            // Baseline uses the BASE size's ascent/descent so all
            // sub-segments sit on the same line (matches D2D's
            // SetFontSize-on-range behaviour where the per-range
            // size changes the glyph rasters but the layout
            // baseline stays put).
            void drawValueTextGpu(glyph_atlas::Renderer* r,
                                   const GpuTextFormat& baseFmt,
                                   const wchar_t* s, std::size_t n,
                                   const D2D1_RECT_F& rect,
                                   const float baseColor[4],
                                   const float* chiffresColor = nullptr,
                                   uint16_t unitSizePx = 0) const {
                if (!r || n == 0 || !s) return;
                const std::wstring wide(s, n);
                const auto runs = findValueRuns(wide);

                // Sanity-check the caller's unitSizePx against the
                // atlas — only sizes that were baked at init have
                // glyphs to sample. Today the bottom-panel call sites
                // pass 19 (which IS baked at init like every value size),
                // but if a future caller hands in an unbaked size every
                // glyph in the unit range misses m_glyphs and silently
                // collapses to fallbackAdvance(). Clamp to the base
                // size so the unit suffix at least renders — same
                // visual class as the digits, just no size-shrink.
                // One-shot log so the divergence is visible if it
                // ever happens.
                if (unitSizePx != 0 &&
                    r->metrics(baseFmt.face, unitSizePx) == nullptr) {
                    static bool s_loggedOnce = false;
                    if (!s_loggedOnce) {
                        s_loggedOnce = true;
                        Log(fmt::format(
                            "xr_telemetry: drawValueTextGpu unit size {} "
                            "not baked in atlas — clamping to base {}\n",
                            unitSizePx, baseFmt.sizePx));
                    }
                    unitSizePx = 0;   // falls through to baseFmt.sizePx
                                       // in the attrAt() body below
                }

                // Per-character attribute: (face, sizePx, colorPtr).
                struct Attr {
                    glyph_atlas::GlyphFace face;
                    uint16_t               size;
                    const float*           color;
                    bool operator==(const Attr& o) const noexcept {
                        return face == o.face && size == o.size && color == o.color;
                    }
                };
                auto attrAt = [&](UINT32 i) {
                    Attr a{baseFmt.face, baseFmt.sizePx, baseColor};
                    for (const auto& run : runs) {
                        if (i < run.brushStart) continue;
                        if (i >= run.brushStart + run.brushLen) continue;
                        if (chiffresColor) a.color = chiffresColor;
                        if (unitSizePx &&
                            i >= run.unitStart &&
                            i <  run.unitStart + run.unitLen) {
                            a.size = unitSizePx;
                        }
                        break;  // value runs don't overlap, first hit wins
                    }
                    return a;
                };

                // Build maximal segments.
                struct Segment {
                    Attr        attr;
                    UINT32      start;
                    UINT32      len;
                };
                std::vector<Segment> segs;
                segs.reserve(8);
                const UINT32 N = static_cast<UINT32>(n);
                UINT32 i = 0;
                while (i < N) {
                    const Attr a = attrAt(i);
                    UINT32 j = i + 1;
                    while (j < N && attrAt(j) == a) ++j;
                    segs.push_back({a, i, j - i});
                    i = j;
                }

                // Total advance width — sum of measure() across segments.
                float totalW = 0.0f;
                for (const auto& seg : segs) {
                    totalW += r->measure(seg.attr.face, seg.attr.size,
                                          wide.c_str() + seg.start, seg.len);
                }

                // Anchor.
                float penX = rect.left;
                switch (baseFmt.alignment) {
                case GpuTextFormat::Alignment::Leading:
                    penX = rect.left; break;
                case GpuTextFormat::Alignment::Center:
                    penX = (rect.left + rect.right - totalW) * 0.5f; break;
                case GpuTextFormat::Alignment::Trailing:
                    penX = rect.right - totalW; break;
                }

                // Baseline — anchored on the BASE size so the smaller
                // unit suffix sits on the same line as the bigger
                // digits (mimics D2D's per-range size override which
                // doesn't shift the baseline).
                const auto* m = r->metrics(baseFmt.face, baseFmt.sizePx);
                const float ascent  = m ? m->ascent  : static_cast<float>(baseFmt.sizePx) * 0.75f;
                const float descent = m ? m->descent : static_cast<float>(baseFmt.sizePx) * 0.25f;
                const float baselineY = (rect.top + rect.bottom + ascent - descent) * 0.5f;

                // Emit.
                float pen = penX;
                for (const auto& seg : segs) {
                    pen = r->drawRun(seg.attr.face, seg.attr.size,
                                      pen, baselineY,
                                      wide.c_str() + seg.start, seg.len,
                                      seg.attr.color);
                }
            }

            // -------- Header bar --------------------------------------------
            //
            // Layout: TWO separately-framed boxes, divided by kSectionGap
            // (the same gap the panels below use):
            //
            //   ┌─────────┐   ┌──────────────────────────────────────┐
            //   │  FPS    │   │ FPS AVG │  P95  │  P99  │   P99.9     │
            //   │  142    │   │  138    │  124  │  108  │    98       │
            //   └─────────┘   └──────────────────────────────────────┘
            //    FPS box        percentile box (4 cells, 3 separators)
            //
            //   * FPS box   — its own panel, single cell, big white hero
            //                 number (kFontBigNumber=52 px on "142").
            //   * stats box — its own panel; the four percentile cells
            //                 (AVG / P95 / P99 / P99.9) in cyan, divided
            //                 by 3 thin vertical separators.
            //
            // Width split, after carving out the one inter-box gap: the
            // FPS box is kHeaderFpsCellUnits (1.25) percentile-cells wide
            // — a touch wider than one stat cell since it carries the hero
            // number — and the four stat cells share the rest equally.
            // Each cell is ~130 px on the 720-wide texture (kInnerR -
            // kInnerL ≈ 688). Rajdhani SemiBold is a narrow grotesque, so
            // 52 px on "142" sits comfortably inside the FPS box; the
            // kFontTinyLabel=17 px caption stays under 50 px even for
            // "P99.9". On systems without the bundled font we fall back to
            // upright Bahnschrift.
            void drawHeaderBar(float l, float t,
                                float r, float b,
                                const OverlayDisplayValues& v) const {
                // FPS box is 1.25 stat-cells wide; the 4 stat cells take
                // 1 unit each. Carve out the single inter-box gap first,
                // then split the remainder into (1.25 + 4) units.
                constexpr float kHeaderFpsCellUnits = 1.25f;
                const float availW    = (r - l) - kSectionGap;
                const float statCellW = availW / (kHeaderFpsCellUnits + 4.0f);
                const float fpsR      = l + statCellW * kHeaderFpsCellUnits;
                const float statsL    = fpsR + kSectionGap;

                const float labelH = 22.0f;
                const float labelY = t + 4.0f;
                const float valueY = labelY + labelH;

                // --- FPS box: its own framed panel, single cell ----------
                drawPanelBg(l, t, fpsR, b);
                drawHeaderCell(l, labelY, fpsR, valueY,
                                L"FPS", v.fps_instant,
                                kFmtBigNumberGpu, kColorTextWhite);

                // --- Percentile box: own framed panel, 4 cells -----------
                drawPanelBg(statsL, t, r, b);
                // 3 vertical separators between the 4 percentile cells.
                drawColumnSeparators(statsL, t, b, statCellW, /*count=*/3,
                                      kHeaderSepInsetY);
                drawHeaderCell(statsL + statCellW * 0.0f, labelY, statsL + statCellW * 1.0f, valueY,
                                L"FPS AVG", v.fps_avg,
                                kFmtAccentNumberGpu, kColorAccentCyan);
                drawHeaderCell(statsL + statCellW * 1.0f, labelY, statsL + statCellW * 2.0f, valueY,
                                L"P95", v.fps_p95,
                                kFmtAccentNumberGpu, kColorAccentCyan);
                drawHeaderCell(statsL + statCellW * 2.0f, labelY, statsL + statCellW * 3.0f, valueY,
                                L"P99", v.fps_p99,
                                kFmtAccentNumberGpu, kColorAccentCyan);
                drawHeaderCell(statsL + statCellW * 3.0f, labelY, r, valueY,
                                L"P99.9", v.fps_p99_9,
                                kFmtAccentNumberGpu, kColorAccentCyan);
            }

            void drawHeaderCell(float l, float t,
                                 float r, float valueY,
                                 const wchar_t* label,
                                 const std::string& value,
                                 const GpuTextFormat& valueFormat,
                                 const float valueColor[4]) const {
                const D2D1_RECT_F labelRect = D2D1::RectF(l, t, r, valueY);
                drawLabel(kFmtTinyLabelGpu, label, labelRect, kColorTextWhite);
                const D2D1_RECT_F valueRect = D2D1::RectF(
                    l, valueY - 2.0f, r,
                    valueY + kFontBigNumber + 6.0f);
                drawAscii(valueFormat, value, valueRect, valueColor);
            }

            // -------- Frametime panel ---------------------------------------
            //
            // Layout:
            //   - title "GPU FRAMETIME" (top-left, ~14 px line)
            //   - current value "6.7" + cyan "ms" suffix (top-right)
            //   - 4 horizontal dashed grid lines + a dashed left ms-axis
            //   - histogram bars filling the remaining vertical space
            // Draws only the "chrome" of a frametime panel: panel
            // background, title, and the top-right current value. The
            // histogram region itself (background fill, dashed grid + axis,
            // bars, budget line) is owned by drawHistoRegion() and is
            // refreshed every frame so the ring buffer's scroll stays
            // smooth — see paint()'s static/dynamic dispatch.
            void drawFrametimePanel(float l, float t,
                                     float r, float b,
                                     const wchar_t* title,
                                     const std::string& currentValue,
                                     const std::string& breakdown,
                                     float targetFps) const {
                drawPanelBg(l, t, r, b);

                // Title bar — top inner padding. Title row paddings
                // shared with the bar renderer's histo geometry (see
                // kPanelTitleTopPad / kHistoTitleGap).
                const float titleT = t + kPanelTitleTopPad;
                const float titleB = titleT + kHistoTitleH;
                const D2D1_RECT_F titleRect = D2D1::RectF(
                    l + kSectionInnerPad, titleT,
                    r - kSectionInnerPad, titleB);
                drawLabel(kFmtSectionTitleGpu, title, titleRect, kColorTextWhite);

                // Current value(s) on the title row, right-aligned.
                // Two shapes, selected by whether `breakdown` is empty:
                //
                //   GPU panel (breakdown == "") : just "6.7 ms".
                //   CPU panel (breakdown != "") : one compound,
                //       "Render X ms / App Y ms" — the Render term, then
                //       the per-cycle total labelled "App" (currentValue,
                //       = OXRT "app CPU").
                //
                // Both go through drawValueAscii: every "X.X ms" run
                // picks up the cyan accent brush, while label copy
                // ("Render", " / App", …) stays white. All of it renders
                // in one upright Rajdhani cut — digits and labels alike. The
                // value rect shares the title rect's vertical bounds
                // (titleT → titleB) so the baseline lands on the title
                // line.
                if (breakdown.empty()) {
                    // GPU panel: short "6.7 ms" primary frametime
                    // read-out. drawValueAscii auto-detects the "6.7
                    // ms" value run; the whole thing renders upright
                    // Rajdhani in cyan.
                    const std::string s = currentValue + " ms";
                    const D2D1_RECT_F valueRect = D2D1::RectF(
                        r - kSectionInnerPad - 160.0f, titleT,
                        r - kSectionInnerPad,          titleB);
                    drawValueAscii(kFmtMsValueGpu, s, valueRect,
                                    kColorAccentCyan);
                } else {
                    // CPU panel: the Render term (the app's render-recording,
                    // = OXRT "render CPU") then the per-cycle total labelled
                    // "App" (= OXRT "app CPU"). One right-aligned compound;
                    // each "X.X ms" run renders cyan, the labels and the " / "
                    // separator white. Render is a sub-part of App; the gap
                    // (Wait→Begin work + runtime begin/end calls + between-
                    // frames) is not broken out.
                    //
                    // Width: 360 px (left edge ~332 px) comfortably fits the
                    // two terms even at double-digit ms — e.g. "Render 22.1 ms
                    // / App 41.5 ms" ≈ 250 px — well clear of the title.
                    const std::string compound =
                        breakdown + " / App " + currentValue + " ms";
                    const D2D1_RECT_F valueRect = D2D1::RectF(
                        r - kSectionInnerPad - 360.0f, titleT,
                        r - kSectionInnerPad,          titleB);
                    drawValueAscii(kFmtCpuBreakdownGpu, compound, valueRect,
                                    kColorTextWhite,     // "Render" / " / App" labels
                                    kColorAccentCyan);   // the two value runs
                }

                // --- left-hand ms axis ---------------------------------
                // Tick LABELS for the histogram strip ("0", "5", "10", …),
                // drawn in the DYNAMIC tier: they rescale with the refresh
                // rate (computeMsAxis(targetFps)) so they can't be baked
                // into the static cache. The matching gridlines are emitted
                // by HistogramBarRenderer::drawPanel from the SAME axis and
                // the SAME strip geometry (recomputed identically below),
                // so numbers and lines stay pixel-aligned. Each number is
                // LEFT-aligned at histoL — the same x as the section title —
                // so the axis column lines up under the panel label; it lives
                // in the gutter [histoL, plotL], clear of the first bar.
                // drawAscii → drawTextGpu vertically-centres it on the tick
                // Y. heightFrac is measured from the bottom, hence the
                // (1 − heightFrac) for the top-down Y.
                //
                // The bar HISTOGRAM region itself is still owned by
                // HistogramBarRenderer (bg + bars + gridlines + budget),
                // refreshed every frame so the bars scroll at the host rate
                // while this chrome only repaints on snap.version ticks.
                const float histoL = l + kSectionInnerPad;
                const float histoT =
                    t + kPanelTitleTopPad + kHistoTitleH + kHistoTitleGap;
                const float histoB = b - kSectionInnerPad;
                const float stripH = histoB - histoT;
                const MsAxis axis = computeMsAxis(targetFps);
                if (axis.valid && stripH > 0.0f) {
                    // Labels read "<n> ms" and RIGHT-align to a shared right
                    // edge = histoL + the widest label's width. That puts the
                    // widest label's leading digit at histoL (under the panel
                    // title) and lines the "ms" suffixes up, with the narrower
                    // labels right-aligned beneath the units column — the
                    // reference look ("16 ms / 11 ms / 6 ms / 0 ms").
                    //
                    // Vertically drawTextGpu centres each glyph on the rect's
                    // mid-Y, so centre that on the gridline's VISUAL centre: the
                    // line is a kChromeLineW-tall quad whose TOP sits at the tick
                    // Y, so its centre is half a line-width lower (without this
                    // the number rode ~0.75 px above its dash). The centre is
                    // then clamped DOWN only enough to keep the glyph clear of
                    // the TITLE text — its top may sit in the title gap (empty
                    // space, kHistoTitleGap above histoT). The old histoT+halfH
                    // threshold kept the WHOLE glyph below histoT, which yanked a
                    // near-top tick's label off its line (12 ms @ 90 Hz sits 90 %
                    // up the strip). The "0" tick now gets a baseline too
                    // (bottom-aligned at histoB), so its label takes the
                    // half-line offset UPWARD; it never reaches the clamp.
                    const auto* lm = m_textRenderer
                        ? m_textRenderer->metrics(kFmtAxisLabelGpu.face,
                                                   kFmtAxisLabelGpu.sizePx)
                        : nullptr;
                    const float halfH = lm
                        ? 0.5f * (lm->ascent + lm->descent)
                        : 0.5f * static_cast<float>(kFmtAxisLabelGpu.sizePx);

                    float maxW = 0.0f;
                    if (m_textRenderer) {
                        for (int i = 0; i < axis.tickCount; ++i) {
                            const std::string s =
                                std::to_string(axis.ticks[i].ms) + " ms";
                            const std::wstring w(s.begin(), s.end());
                            maxW = std::max(maxW, m_textRenderer->measure(
                                kFmtAxisLabelGpu.face,
                                kFmtAxisLabelGpu.sizePx, w.c_str(), w.size()));
                        }
                    }
                    const float labelRight = histoL + maxW;
                    // Lowest centre that still clears the title text: the glyph
                    // top (centre − halfH) may reach the title gap but not the
                    // title row, which ends kHistoTitleGap above histoT.
                    const float minCenterY = (histoT - kHistoTitleGap) + halfH;
                    for (int i = 0; i < axis.tickCount; ++i) {
                        const float yTick = histoT + stripH *
                            (1.0f - axis.ticks[i].heightFrac);
                        // Centre on the line's mid-line: interior ticks have
                        // their line TOP at the tick Y (+ half the width); the
                        // bottom-edge baseline is bottom-aligned at histoB (= its
                        // tick Y), so − half the width. atBottomEdge is the single
                        // source of this split (it also drives the renderer's
                        // gridline-skip), so the line and its label can't drift.
                        const float gridCenterY = !axis.ticks[i].atBottomEdge
                            ? yTick + kChromeLineW * 0.5f
                            : yTick - kChromeLineW * 0.5f;
                        const float y = std::max(gridCenterY, minCenterY);
                        const D2D1_RECT_F labelRect = D2D1::RectF(
                            histoL, y, labelRight, y);
                        drawAscii(kFmtAxisLabelGpu,
                                   std::to_string(axis.ticks[i].ms) + " ms",
                                   labelRect, kColorAxisLabel);
                    }
                }
            }

            // -------- Bottom panel (TEMP / LOAD [/ VRAM], text-only) --------
            //
            // 2 or 3 equally-sized columns (CPU = 2, GPU = 3 because
            // VRAM lives on the GPU side). Each column carries a
            // small uppercase label at the top and a big numeric
            // value below. The LOAD and VRAM values are tier-
            // coloured (cyan / orange / red) so the user gets the
            // headroom signal at a glance — same palette and same
            // tierForUtilisation thresholds as the histogram
            // bars.
            //
            //   GPU panel (3 cells):                CPU panel (2 cells):
            //   ┌─────────┬──────────┬──────┐      ┌──────────┬──────────┐
            //   │GPU TEMP │GPU LOAD  │VRAM  │      │CPU LOAD  │CPUs LOAD │
            //   │ 67 °C   │ 92 %     │ 76 % │      │ 78 %     │ 98 %     │
            //   └─────────┴──────────┴──────┘      └──────────┴──────────┘
            //
            // Each cell is described by a {label, value, kind, fraction}
            // BottomCell; labels are wide literals (L"GPU TEMP" /
            // L"GPU LOAD" / L"VRAM" / L"CPU LOAD" / L"CPUs LOAD"). The number
            // of cells the caller passes sets the column count (3 on the GPU
            // panel, 2 on the CPU panel); `kind` selects temperature vs
            // percentage styling per cell.
            //
            // One cell of a bottom panel. `kind` selects the value styling:
            //   Temp    -> degrees-C suffix, white, wide-glyph path (the GPU
            //              panel's temperature cell; `fraction` ignored).
            //   Percent -> " %" suffix, tier-coloured by `fraction`, ASCII
            //              (GPU LOAD / VRAM / CPU LOAD / CPUs LOAD).
            enum class BottomCellKind { Temp, Percent };
            struct BottomCell {
                const wchar_t* label;
                std::string    value;
                BottomCellKind kind;
                float          fraction;  // tier input; ignored when kind == Temp
            };

            // Draw a single framed footer cell across [l,r] x [t,b]: its own
            // panel background, the caption, and the kind-styled value.
            // drawChrome sizes the box and the inter-box gaps; this helper
            // owns everything inside one box. (Was drawBottomPanel, which
            // grouped 2-3 cells with internal separators into one panel.)
            void drawBottomCell(float l, float t, float r, float b,
                                 const BottomCell& cell) const {
                drawPanelBg(l, t, r, b);

                // Tier-colour helper: same logic for LOAD, VRAM and both
                // CPU-panel cells ("CPU LOAD" + "CPUs LOAD"). < 80 % = cyan
                // default, 80–89 % = orange warning, ≥ 90 % = red critical.
                // A TEMP value
                // (GPU panel cell 0) stays white instead — no thermal-tier
                // contract yet (would require knowing TjMax per SKU).
                auto tierColor = [&](float fraction) -> const float* {
                    const BarTier tier = tierForUtilisation(fraction);
                    if (tier == BarTier::Red)    return kColorTierRed;
                    if (tier == BarTier::Orange) return kColorOrange;
                    return kColorAccentCyan;
                };

                // No internal separators: each metric is its own framed box
                // now (drawChrome lays the five out with kSectionGap between
                // them), so there are no columns to rule off here.

                // Cell labels arrive as wide string literals in each
                // descriptor (see the call site in paint()). VRAM is a
                // singleton label ("VRAM", not "GPU VRAM") because it's
                // implicitly GPU-side and the extra word would waste
                // horizontal space.

                // Cell layout — label anchored near the top, metric
                // paragraph-centred in the region BELOW the label (not
                // the whole cell). The value rect starts at labelY+22
                // (the label's bottom edge), so paragraph CENTER
                // (inherited from m_fmtTemp) places the 43-px digit in
                // the space under the caption — it can't ride up into
                // the label even though kBottomHeight (90, matched to
                // the FPS header) is tight. Label rect = 22 px from
                // t+4 → t+26; the digit centres in [t+26, b] and clears
                // the caption with room to spare. (Centring in the FULL
                // cell, as an earlier revision did, needed the old
                // 109-px height to keep the same clearance.)
                const float labelY = t + 4.0f;

                auto drawCell = [&](float cellL, float cellR,
                                      const wchar_t* label,
                                      const std::string& asciiValue,
                                      const wchar_t* unitSuffix,
                                      const float* valueColor,
                                      bool useWideValue) {
                    drawLabel(kFmtTinyLabelGpu, label,
                               D2D1::RectF(cellL, labelY, cellR,
                                            labelY + 22.0f),
                               kColorTextWhite);
                    // m_fmtTemp's BASE is Rajdhani upright; the digit
                    // prefix flips to Rajdhani via drawValueWide /
                    // drawValueAscii's auto-detected ranges, while the
                    // unit suffix (" °C" / " %" / " GB") keeps the base
                    // family/style AND shrinks to kFontTempUnit — a
                    // touch larger than the "GPU TEMP" / "LOAD" / "VRAM"
                    // caption sitting just above it, so the unit reads
                    // as an annotation on the big-number digit without
                    // disappearing into the caption row. Single brush —
                    // the whole value shares the per-tier colour (white
                    // / cyan / orange / red), only the font face and
                    // size change between digit and unit.
                    // Anchor the value region at the label's bottom
                    // (labelY + 22) rather than the cell top, so the
                    // paragraph-centred digit sits in the space under
                    // the caption — keeps the 43-px metric clear of the
                    // label now that the cell is only kBottomHeight (90)
                    // tall. labelY + 22 == the label rect's bottom above.
                    const D2D1_RECT_F valueRect = D2D1::RectF(cellL, labelY + 22.0f, cellR, b);
                    if (useWideValue) {
                        // For the °C suffix the whole string must be
                        // wide. tempValue is ASCII, so byte-widening
                        // it and concatenating L" °C" works.
                        std::wstring wide(asciiValue.begin(),
                                           asciiValue.end());
                        wide += unitSuffix;
                        drawValueWide(kFmtTempGpu, wide, valueRect,
                                       valueColor,
                                       /*chiffresColor=*/nullptr,
                                       /*unitFontSize=*/kFontTempUnit);
                    } else {
                        // % and other ASCII-safe suffixes — single
                        // ASCII drawValueAscii call, no wide conversion.
                        std::string full = asciiValue;
                        // Append the suffix as narrow chars. Caller
                        // contract: !useWideValue means unitSuffix is
                        // ASCII-only (`%`, ` ms`, ` x`, …). The assert
                        // guards against future drift — if someone
                        // passes ` °C` through this branch (the byte
                        // 0xB0 alone, without the wide-conversion
                        // path), the narrow cast silently produces
                        // mojibake. Same family of bug as the
                        // CP1252-source mishap we caught with the
                        // first snapshot artifact.
                        for (const wchar_t* p = unitSuffix; *p; ++p) {
                            assert(static_cast<unsigned>(*p) < 0x80 &&
                                   "ASCII-only suffix expected on the "
                                   "!useWideValue path");
                            full.push_back(static_cast<char>(*p));
                        }
                        drawValueAscii(kFmtTempGpu, full, valueRect,
                                        valueColor,
                                        /*chiffresColor=*/nullptr,
                                        /*unitFontSize=*/kFontTempUnit);
                    }
                };

                // Cell 0 — <prefix> TEMP / "67 °C"
                //
                // Use a ° escape rather than a literal ° byte:
                // MSVC without /utf-8 treats source files as the
                // system codepage (CP1252 on EN-US Windows), so a
                // literal ° in the source (UTF-8 bytes 0xC2 0xB0)
                // gets read as two CP1252 chars and the resulting
                // wide literal is L"Â°", which renders as the
                // mojibake we saw in the first snapshot artifact.
                // ° sidesteps the entire source-encoding
                // question — the wide literal is exactly L"°"
                // regardless of how MSVC parses the surrounding
                // bytes.
                // Single framed cell: the box spans the full [l, r]. (Was a
                // left-to-right loop over 2-3 columns; drawChrome now owns the
                // multi-box layout.)
                const float cellL = l;
                const float cellR = r;
                if (cell.kind == BottomCellKind::Temp) {
                    // Wide-glyph path, white. The L" \u00B0C" escape (not a
                    // literal degree byte) sidesteps MSVC's CP1252 source
                    // reading \u2014 see the encoding note above.
                    drawCell(cellL, cellR, cell.label, cell.value,
                              L" \u00B0C", kColorTextWhite,
                              /*useWideValue=*/true);
                } else {
                    // Percent: ASCII " %" suffix, tier-coloured.
                    drawCell(cellL, cellR, cell.label, cell.value,
                              L" %", tierColor(cell.fraction),
                              /*useWideValue=*/false);
                }
            }

            // Rounded corner radii (texture pixels) for the chrome. The
            // shader clamps each to half the shorter side, so one value is
            // safe on the tall frametime panels and the short bottom cells
            // alike. Set to 0 to restore the old sharp corners.
            static constexpr float kPanelCornerRadius = 10.0f;
            static constexpr float kFrameCornerRadius = 16.0f;

            // Panel background — flat dark-blue panel with a 1-px separator
            // border and rounded corners. Used for the header, both
            // frametime panels, and the bottom row pair. Drawn as a single
            // stroked rounded rect (addRoundedRect) — fill + a 1-px border
            // ring in one quad via the shader SDF, so a translucent fill
            // would keep its alpha (nothing opaque behind it). (Earlier iterations
            // tried bevels / a carbon-fibre hatch; dropped for a flat
            // panel. Corners were sharp after the D2D→GPU migration until
            // the quad shader gained a rounded-rect SDF path.)
            void drawPanelBg(float l, float t, float r, float b) const {
                if (m_tier != PaintTier::Static) return;   // chrome shape
                m_chromeShapes->addRoundedRect(l, t, r - l, b - t,
                                                /*fill=*/kColorPanelBg,
                                                /*border=*/kColorSeparator,
                                                /*borderWidth=*/kChromeLineW,
                                                /*radius=*/kPanelCornerRadius);
            }

            // Outer frame. Historically a chamfered (cut-corner) octagon,
            // then a plain rect after the GPU migration; now a rounded rect
            // via the quad-shader SDF — one stroked rounded rect
            // (addRoundedRect): translucent bg fill + an opaque frame-line
            // border ring in a single quad. Kept the name + signature so the
            // drawChrome call site still reads as "the outer frame". The
            // target is cleared transparent, so the rounded outer corners
            // fall to alpha 0 — the overlay's corners are see-through (the
            // VR scene shows through), not black.
            void drawChamferedRect(const D2D1_RECT_F& rect,
                                     float strokeWidth) const {
                if (m_tier != PaintTier::Static) return;   // chrome shape
                m_chromeShapes->addRoundedRect(rect.left, rect.top,
                                                rect.right - rect.left,
                                                rect.bottom - rect.top,
                                                /*fill=*/kColorBg,
                                                /*border=*/kColorFrameLine,
                                                /*borderWidth=*/strokeWidth,
                                                /*radius=*/kFrameCornerRadius);
            }

            // Static-tier column separators: `count` thin vertical rules
            // (kChromeLineW wide) at l + colWidth*i (i = 1..count), inset by
            // insetY top and bottom. Now used only by the header's percentile
            // box (3 rules between AVG/P95/P99/P99.9); the footer cells are
            // individually framed, so they no longer rule columns. Kept as a
            // shared helper with one consistent tier gate (inside the helper,
            // matching drawPanelBg / drawChamferedRect) instead of an inline
            // if at the call site.
            void drawColumnSeparators(float l, float t, float b,
                                       float colWidth, int count,
                                       float insetY) const {
                if (m_tier != PaintTier::Static) return;   // chrome shape
                for (int i = 1; i <= count; ++i) {
                    const float x = l + colWidth * static_cast<float>(i);
                    m_chromeShapes->addRect(
                        x, t + insetY, kChromeLineW, (b - insetY) - (t + insetY),
                        kColorSeparator);
                }
            }

            // -------- Members -----------------------------------------------
            ComPtr<IDWriteFactory>    m_dwriteFactory;
            // Bundled-font state. Held for the lifetime of the
            // CoreRenderer so the IDWriteInMemoryFontFileLoader stays
            // registered with the factory and the font files stay
            // valid for as long as text formats reference them.
            // m_inMemoryFontLoader is the registered loader (we
            // unregister it in the destructor to keep the factory
            // clean across many overlay-renderer create/destroy
            // cycles).
            ComPtr<IDWriteInMemoryFontFileLoader> m_inMemoryFontLoader;
            std::vector<ComPtr<IDWriteFontFile>>  m_bundledFontFiles;
            ComPtr<IDWriteFontCollection>         m_customFontCollection;

            OverlayDisplayValues m_cachedValues;

            // Glyph atlas — baked once at init from the same DirectWrite
            // factory + custom collection above. Held as CPU-side data
            // (bitmap + glyph table + per-face metrics); each overlay
            // renderer copies what it needs into its own D3D11 atlas
            // texture during its own init. Stays valid for CoreRenderer's
            // lifetime so the renderers don't have to clone the bitmap.
            glyph_atlas::BuildResult m_atlas;
            bool                     m_atlasReady = false;

            // GPU text renderer, stashed transiently by paintChromeOnly
            // for the duration of one chrome paint. drawLabel / drawAscii /
            // drawValueWide / drawValueAscii emit drawRun calls onto its
            // batch. paintChromeOnly sets it before drawChrome and clears
            // it (via a scope guard) on exit, so it's non-null for the
            // whole chrome rebuild and null otherwise.
            glyph_atlas::Renderer*   m_textRenderer = nullptr;

            // GPU chrome-shape renderer, stashed the same way. drawPanelBg /
            // drawChamferedRect / the column-separator loops in
            // drawHeaderBar / drawBottomCell append rects to its batch.
            chrome_shapes::Renderer* m_chromeShapes = nullptr;

            // -------- Two-tier static/dynamic paint state -----------------
            //
            // drawChrome runs once per static bake AND once per value bump;
            // m_tier selects which leaves emit during a traversal (Static =
            // labels + shapes, Dynamic = values — see the TIER CONTRACT
            // comment by drawLabel). The static tier is cached in the glyph
            // renderer's static scratch + the chrome-shape renderer's
            // persistent scratch; only the dynamic tier is re-laid-out each
            // version bump — the point of the split, shrinking the ~10 Hz
            // chrome-rebuild CPU spike.

            // Single source of truth for the STATIC structural signature:
            // any input that changes the chrome's fixed geometry or label
            // set. Today that's only whether the GPU bottom panel carries a
            // VRAM column (drawBottomCell shows it iff its vram value is
            // non-empty; drawChrome feeds it v.vram_pct). A new optional
            // column / panel / second GPU must be folded in HERE (widen the
            // return to a bitmask if more than one independent input ever
            // appears) so the cached static tier re-bakes when it changes.
            static bool structureSignature(const OverlayDisplayValues& v) {
                return !v.vram_pct.empty();
            }

            // m_staticBaked: whether the cached static tier is populated.
            // m_staticStructure: the structureSignature() the cache was
            // baked against; a mismatch forces a re-bake so cached labels /
            // shapes can't go stale against the live structure.
            enum class PaintTier { Static, Dynamic };
            PaintTier m_tier            = PaintTier::Dynamic;
            bool      m_staticBaked     = false;
            bool      m_staticStructure = false;
        };

        // -------- HistogramBarRenderer: D3D11 instanced histogram region -----
        //
        // Paints the histogram region (background + grid + left axis + bars +
        // budget line) with a handful of instanced GPU draws. Owns two
        // pipelines — solid-colour quads for the bg / grid / axis / budget, and
        // gradient bars for the samples — plus the dynamic instance + constant
        // buffers refilled per panel per frame. The render target is passed
        // per drawPanel(rtv, …) call — the acquired swapchain image (the
        // D3D11 path renders into it directly; the D3D12 path via its
        // D3D11On12-wrapped handle).
        //
        // Caller contract: the GPU pipeline state is fully owned per draw —
        // drawPanel() (and the chrome-shape / glyph-atlas flushes that run
        // alongside it) each set ALL the state they need, since they
        // clobber each other's bindings on the shared context. Ordering on
        // that context is chrome shapes → text → bars, so the bars land on
        // top of the chrome.
        //
        // Colours are the same straight-alpha palette as the chrome
        // constants. The histogram rect is fully opaque after the background
        // fill, so the runtime composites the quad layer correctly.
        //
        // NOTE: deliberately NOT folded onto utils::InstancedBatchBuffer
        // (the shared batch buffer the chrome-shape / glyph-atlas renderers
        // use). This renderer's instance buffers are FIXED-size (no power-
        // of-two growth) and there are several of them (bars + quads), so it
        // would share only the create+upload half, not the growth half the
        // shared buffer is built around. Its per-Map-failure handling does
        // follow the same contract, though: a failed buffer Map suppresses
        // the affected draw pass rather than drawing over stale /
        // uninitialized data (see drawPanel's quadOk / barCount gates).
        class HistogramBarRenderer {
          public:
            // RTV is supplied per-draw by the caller (Task 15 — D3D11
            // path paints directly into one of N swapchain images,
            // so the pipeline state stays renderer-owned while the
            // target rotates each frame). Target dimensions (kTexW,
            // kTexH) are constant for the renderer's lifetime so we
            // bake them into the immutable cbuffer at init.
            // `ss` is the overlay supersample factor (kOverlaySupersample).
            // The cbuffers keep LOGICAL texSize (kTexW × kTexH); only the
            // flush-time viewport + scissor scale by ss to cover the physical
            // render target. Defaults to 1.0 so the snapshot/golden path
            // (which calls init with two args) renders at the legacy size.
            bool init(ID3D11Device* device, ID3D11DeviceContext* ctx,
                       float ss = 1.0f) {
                if (!device || !ctx) return false;
                m_device = device;
                m_ctx = ctx;
                m_ss  = ss;
                // Reset the per-panel cbuffer cache: the bar cbuffers are
                // filled-once-and-cached (and now carry m_ss + the tier
                // colours), so a re-init at a different factor must refill them
                // rather than bind the stale cache. No in-place re-init exists
                // today (each renderer is constructed fresh), but this keeps
                // init() correct if one is ever added (device-loss, pooling).
                m_barCBFilled[0] = m_barCBFilled[1] = false;

                // Per-step failure logging. Without it, an HRESULT lost
                // somewhere in this chain just disabled the GPU path with
                // no clue about which API call broke — making "the bars
                // don't render in GPU mode" reports hard to triage.
                auto fail = [](const char* step) {
                    Log(fmt::format(
                        "xr_telemetry: HistogramBarRenderer init failed "
                        "at step: {}\n",
                        step));
                    return false;
                };

                // Shaders from embedded bytecode.
                if (FAILED(m_device->CreateVertexShader(
                        g_overlay_bars_vs, sizeof(g_overlay_bars_vs),
                        nullptr, m_barsVS.GetAddressOf())))
                    return fail("CreateVertexShader (bars)");
                if (FAILED(m_device->CreatePixelShader(
                        g_overlay_bars_ps, sizeof(g_overlay_bars_ps),
                        nullptr, m_barsPS.GetAddressOf())))
                    return fail("CreatePixelShader (bars)");
                if (FAILED(m_device->CreateVertexShader(
                        g_overlay_quad_vs, sizeof(g_overlay_quad_vs),
                        nullptr, m_quadVS.GetAddressOf())))
                    return fail("CreateVertexShader (quad)");
                if (FAILED(m_device->CreatePixelShader(
                        g_overlay_quad_ps, sizeof(g_overlay_quad_ps),
                        nullptr, m_quadPS.GetAddressOf())))
                    return fail("CreatePixelShader (quad)");

                // Input layouts. Slot 0 = per-vertex unit-quad corner;
                // slot 1 = per-instance data. Offsets match the structs
                // below and the HLSL semantic names exactly.
                const D3D11_INPUT_ELEMENT_DESC barsIL[] = {
                    {"POSITION",   0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                     D3D11_INPUT_PER_VERTEX_DATA,   0},
                    {"BAR_XLEFT",  0, DXGI_FORMAT_R32_FLOAT,    1, 0,
                     D3D11_INPUT_PER_INSTANCE_DATA, 1},
                    {"BAR_HEIGHT", 0, DXGI_FORMAT_R32_FLOAT,    1, 4,
                     D3D11_INPUT_PER_INSTANCE_DATA, 1},
                    {"BAR_TIER",   0, DXGI_FORMAT_R32_UINT,     1, 8,
                     D3D11_INPUT_PER_INSTANCE_DATA, 1},
                };
                if (FAILED(m_device->CreateInputLayout(
                        barsIL, _countof(barsIL),
                        g_overlay_bars_vs, sizeof(g_overlay_bars_vs),
                        m_barsLayout.GetAddressOf())))
                    return fail("CreateInputLayout (bars)");

                // Shared layout — see overlay_quad_layout in
                // chrome_shape_renderer.h. Same instance struct + element
                // descs ChromeShapeRenderer uses, so the two overlay_quad
                // pipelines stay in lockstep.
                if (FAILED(m_device->CreateInputLayout(
                        chrome_shapes::kQuadInputLayout,
                        _countof(chrome_shapes::kQuadInputLayout),
                        g_overlay_quad_vs, sizeof(g_overlay_quad_vs),
                        m_quadLayout.GetAddressOf())))
                    return fail("CreateInputLayout (quad)");

                // Static unit-quad vertex buffer: 4 corners for a triangle
                // strip. corner.x ∈ {0,1} → left/right, corner.y ∈ {0,1} →
                // top/bottom (each VS applies its own lerp).
                const float quadVerts[] = {
                    0.0f, 0.0f,  1.0f, 0.0f,
                    0.0f, 1.0f,  1.0f, 1.0f,
                };
                if (!createBuffer(quadVerts, sizeof(quadVerts),
                                   D3D11_BIND_VERTEX_BUFFER,
                                   D3D11_USAGE_IMMUTABLE,
                                   m_quadVB))
                    return fail("CreateBuffer (unit quad VB)");

                // Dynamic instance buffers — refilled from the ring each
                // frame (the histogram scrolls at the host frame rate).
                if (!createDynamic(kRingSize * sizeof(BarInstance),
                                    D3D11_BIND_VERTEX_BUFFER, m_barInstances))
                    return fail("CreateBuffer (bar instances)");
                if (!createDynamic(kQuadSlots * sizeof(QuadInstance),
                                    D3D11_BIND_VERTEX_BUFFER, m_quadInstances))
                    return fail("CreateBuffer (quad instances)");
                // Two bars constant buffers, one per panel (CPU / GPU). Their
                // contents are invariant for the renderer's life (panel rect +
                // isGpu colours never change), so each is filled ONCE on first
                // use (see drawPanel) instead of re-mapped every frame — same
                // intent as the IMMUTABLE m_quadCB below.
                if (!createDynamic(sizeof(BarConstants),
                                    D3D11_BIND_CONSTANT_BUFFER, m_barCB[0]) ||
                    !createDynamic(sizeof(BarConstants),
                                    D3D11_BIND_CONSTANT_BUFFER, m_barCB[1]))
                    return fail("CreateBuffer (bar constants)");

                // The quad constant buffer carries only texSize, which is
                // invariant — initialise it IMMUTABLE so drawPanel doesn't
                // re-map+memcpy 16 bytes per panel per frame (2× per frame
                // total in the host's hot path).
                const QuadConstants quadCBInit{
                    { static_cast<float>(kTexW), static_cast<float>(kTexH) },
                    { 0.0f, 0.0f }
                };
                if (!createBuffer(&quadCBInit, sizeof(quadCBInit),
                                   D3D11_BIND_CONSTANT_BUFFER,
                                   D3D11_USAGE_IMMUTABLE,
                                   m_quadCB))
                    return fail("CreateBuffer (quad constants, immutable)");

                // Straight alpha-over blend.
                D3D11_BLEND_DESC bd{};
                bd.RenderTarget[0].BlendEnable = TRUE;
                bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
                bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
                bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
                bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
                bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
                bd.RenderTarget[0].RenderTargetWriteMask =
                    D3D11_COLOR_WRITE_ENABLE_ALL;
                if (FAILED(m_device->CreateBlendState(
                        &bd, m_blend.GetAddressOf())))
                    return fail("CreateBlendState");

                // No culling, scissor ON (confines every draw to the panel's
                // histo rect so nothing bleeds into the chrome).
                D3D11_RASTERIZER_DESC rd{};
                rd.FillMode = D3D11_FILL_SOLID;
                rd.CullMode = D3D11_CULL_NONE;
                rd.ScissorEnable = TRUE;
                rd.DepthClipEnable = TRUE;
                if (FAILED(m_device->CreateRasterizerState(
                        &rd, m_raster.GetAddressOf())))
                    return fail("CreateRasterizerState");

                m_ready = true;
                return true;
            }

            // Paint one panel's histogram region (bg + grid + left axis +
            // bars + budget) into the target image. Rect is in target pixels;
            // isGpu picks the teal vs blue gradient. budgetNs <= 0 (no
            // display-period info yet from the runtime — the first few frames
            // before xrWaitFrame's predicted period lands) still paints the bg,
            // the left axis, and the budget line, and skips the bars (each has
            // no reference height, so barVisualForSample returns heightFraction
            // 0 and the loop emits no instances). The horizontal ms-axis
            // GRIDLINES are the only part absent during that window:
            // computeMsAxis needs a valid target_fps, so with no fps there's no
            // ms scale to mark and the grid slots collapse to zero-area quads
            // (the vertical axis spans the strip height and needs no fps). That
            // keeps the histo region from freezing on stale target content
            // during the warm-up.
            void drawPanel(ID3D11RenderTargetView* rtv,
                            const HistogramRing<kRingSize>& ring,
                            int64_t budgetNs, float targetFps,
                            float histoL, float histoT,
                            float histoR, float histoB,
                            bool isGpu) {
                if (!m_ready || !rtv) return;
                // The ms-axis tick labels occupy the left gutter; the plot
                // (bg / gridlines / bars / budget line) starts after it.
                // plotL is the single left edge every draw below uses, so
                // the labels (drawn by drawFrametimePanel into [histoL,
                // plotL]) never collide with a bar.
                const float plotL  = histoL + kAxisGutter;
                const float fullW  = histoR - plotL;
                const float stripH = histoB - histoT;
                if (fullW <= 0.0f || stripH <= 0.0f) return;

                const std::size_t n = ring.size();
                // Fixed integer bar geometry: 5-px bars at a 5-px step, so
                // adjacent bars touch (no inter-bar gap). Integer width AND
                // integer positions make every bar pixel-identical — no
                // sub-pixel width variation, no AA needed. kRingSize bars
                // span n*5 px; the histo region is wider, so the run is
                // centred and the slack splits into equal left/right
                // margins. (The strip width can be adapted later if the bars
                // should sit flush to the panel edges.)
                constexpr float kBarPx  = 5.0f;
                constexpr float kStepPx = 5.0f;   // bar fills the step — no gap
                // (n−1) full steps + one final bar; reduces to n·step when the
                // bar fills the step (gap 0), i.e. the current geometry.
                const float usedW =
                    static_cast<float>(n) * kStepPx - (kStepPx - kBarPx);
                const float slack = fullW - usedW;
                // Positive slack → centre the run (equal L/R margins).
                // Negative slack (strip narrower than the run, e.g. if a
                // future layout tweak shrinks fullW): shift the start
                // LEFT by the deficit so the rightmost bars stay flush
                // with histoR. The scissor then clips the leftmost
                // (oldest) bars rather than the rightmost (newest) —
                // the newest sample carries the most signal.
                const float startX = plotL + (slack >= 0.0f
                    ? std::floor(slack * 0.5f + 0.5f)
                    : slack);

                // --- refill bar instances from the ring ---
                UINT barCount = 0;
                {
                    D3D11_MAPPED_SUBRESOURCE map{};
                    if (SUCCEEDED(m_ctx->Map(m_barInstances.Get(), 0,
                            D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                        auto* inst = static_cast<BarInstance*>(map.pData);
                        for (std::size_t i = 0; i < n; ++i) {
                            const float x =
                                startX + static_cast<float>(i) * kStepPx;
                            const int64_t sample = ring.at(i);
                            if (sample <= 0) {
                                inst[barCount++] = {x, 0.0f, 3u};  // empty dash
                                continue;
                            }
                            const auto vis =
                                barVisualForSample(sample, budgetNs);
                            if (vis.heightFraction <= 0.0f) continue;
                            uint32_t tier = 0;  // Green → gradient
                            if (vis.tier == BarTier::Red)         tier = 2;
                            else if (vis.tier == BarTier::Orange) tier = 1;
                            inst[barCount++] =
                                {x, vis.heightFraction, tier};
                        }
                        m_ctx->Unmap(m_barInstances.Get(), 0);
                    }
                }

                // --- refill quads. ALL composite UNDER the bars: [0]=bg,
                //     [1..4]=grid, [5]=budget, [6]=0-ms baseline, [7]=axis
                //     (see kSlot*) ---
                bool quadOk = false;
                {
                    D3D11_MAPPED_SUBRESOURCE map{};
                    if (SUCCEEDED(m_ctx->Map(m_quadInstances.Get(), 0,
                            D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                        auto* q = static_cast<QuadInstance*>(map.pData);
                        // overlay_quad_ps anti-aliases every quad (box SDF +
                        // fwidth), so the thin grid / axis / budget lines below
                        // need no explicit corner radius to get clean edges.
                        q[0] = makeQuad(plotL, histoT, fullW, stripH, kPanelBg);
                        // Dashed gridlines at the round-ms axis ticks (INTERIOR
                        // ticks; the 0 tick gets its own baseline below, q[7]).
                        // The axis is derived from the refresh rate (see
                        // computeMsAxis); the matching LABELS are painted by
                        // drawFrametimePanel at these same Ys. At most 4
                        // interior lines occur (the 5-tick case has ticks
                        // 0/2/4/6/8 → 4 interior), which fits the 4 grid
                        // slots [1..4]; any unused slot collapses to a
                        // zero-area quad so the fixed bg+grid draw paints
                        // nothing for it. heightFrac is measured from the
                        // bottom, so the top-down Y is histoT + stripH·(1 −
                        // heightFrac). The dash pattern (kGridDashPeriod/On)
                        // runs along X; every line shares plotL + fullW so the
                        // dashes line up column-to-column. kGridLine matches the
                        // panel-border chrome (weight + tone).
                        const MsAxis axis = computeMsAxis(targetFps);
                        UINT slot = kSlotGridFirst;
                        if (axis.valid) {
                            for (int i = 0;
                                 i < axis.tickCount && slot < kSlotGridEnd; ++i) {
                                if (axis.ticks[i].atBottomEdge) continue;
                                const float y = histoT + stripH *
                                    (1.0f - axis.ticks[i].heightFrac);
                                q[slot++] =
                                    makeQuad(plotL, y, fullW, kChromeLineW,
                                              kGridLine,
                                              kGridDashPeriod, kGridDashOn);
                            }
                        }
                        for (; slot < kSlotGridEnd; ++slot) {
                            q[slot] =
                                makeQuad(plotL, histoT, 0.0f, 0.0f, kGridLine);
                        }
                        // Vertical ms-axis down the left edge (x = plotL),
                        // dashed to match the horizontal grid. The PS auto-
                        // orients the dash along the long axis, so this runs
                        // along Y. Drawn UNDER the bars, like the rest of the
                        // chrome: the negative-slack run puts bar[0] flush
                        // against plotL, so the leftmost bar occludes the axis up
                        // to its own height — the axis shows above that bar (and
                        // wherever the left column is empty), and the bars read
                        // in front of it, matching the budget + baseline.
                        q[kSlotAxis] = makeQuad(plotL, histoT, kChromeLineW,
                                                 stripH, kGridLine,
                                                 kGridDashPeriod, kGridDashOn);
                        const float by =
                            histoT + stripH * budgetLineFraction();
                        // Budget reference line — drawn UNDER the bars (like all
                        // the chrome now) so a bar that exceeds the budget draws
                        // IN FRONT of the line instead of the line cutting across
                        // the bar top.
                        // Same kChromeLineW as the panel borders / separators /
                        // grid: LOGICAL px → constant angular thickness across
                        // supersample factors, while a higher factor gives more
                        // physical texels = better off-axis warp survival.
                        // SOLID (no dash args) — now that the grid is opaque, the
                        // budget line is told apart by being unbroken vs dashed.
                        q[kSlotBudget] = makeQuad(plotL, by, fullW, kChromeLineW,
                                                   kBudgetLine);
                        // 0-ms baseline: dashed line flush with the strip bottom,
                        // same weight/tone/dash as the grid so it matches the
                        // vertical axis. Drawn UNDER the bars (with the budget +
                        // axis): every bar starts at the baseline, so wherever a bar (or
                        // an empty-slot dash) sits the line is occluded — by
                        // request the bars read in front of it, same as the
                        // budget; it stays visible only in any truly empty column.
                        // Bottom-aligned (top at histoB −
                        // kChromeLineW) to sit inside the strip scissor.
                        q[kSlotBaseline] = makeQuad(plotL, histoB - kChromeLineW,
                                                     fullW, kChromeLineW,
                                                     kGridLine, kGridDashPeriod,
                                                     kGridDashOn);
                        m_ctx->Unmap(m_quadInstances.Get(), 0);
                        quadOk = true;
                    }
                }

                // --- constant buffer (one per panel, filled once) ---
                // Every BarConstants field is invariant for the renderer's
                // life — texSize, the panel rect, the bar/dash sizes, and the
                // isGpu-keyed colours never change frame to frame. So fill the
                // panel's buffer the first time it's drawn and just bind it
                // after, instead of re-mapping 112 B per panel every frame.
                // (m_quadCB is IMMUTABLE for the same reason.)
                const int cbIdx = isGpu ? 1 : 0;
                if (!m_barCBFilled[cbIdx]) {
                    BarConstants bc{};
                    bc.texSize[0] = static_cast<float>(kTexW);
                    bc.texSize[1] = static_cast<float>(kTexH);
                    // .x is the PLOT left (after the ms-axis gutter) — the
                    // bar VS only reads .y, but keep .x honest so the
                    // cbuffer describes the actual plot rect.
                    bc.histoTL[0] = plotL;  bc.histoTL[1] = histoT;
                    bc.histoBR[0] = histoR; bc.histoBR[1] = histoB;
                    bc.barWidth = kBarPx;   // fixed 5-px width — fills the step
                    bc.dashHeight = kDashPlaceholderH;  // tier-3 "no data" dash
                    bc.supersample = m_ss;  // logical→physical for the PS edge AA
                    copyColor(bc.gradTop,    isGpu ? kGpuGradTop : kCpuGradTop);
                    copyColor(bc.gradBottom, isGpu ? kGpuGradBot : kCpuGradBot);
                    copyColor(bc.orange, kOrange);
                    copyColor(bc.red,    kRed);
                    copyColor(bc.dash,   kEmptyDash);
                    if (upload(m_barCB[cbIdx], &bc, sizeof(bc)))
                        m_barCBFilled[cbIdx] = true;
                }

                // --- pipeline state (prior passes clobbered it; set all) ---
                m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
                // Viewport spans the PHYSICAL render target (logical × ss).
                // texSize in the cbuffers stays logical, so the bars' float
                // rects stretch losslessly onto the supersampled image.
                D3D11_VIEWPORT vp{0.0f, 0.0f,
                                  static_cast<float>(kTexW) * m_ss,
                                  static_cast<float>(kTexH) * m_ss,
                                  0.0f, 1.0f};
                m_ctx->RSSetViewports(1, &vp);
                // Clip to the PLOT rect (left = plotL, after the ms-axis
                // gutter), not the full histo rect: under negative slack
                // the oldest bars start left of plotL, and this scissor
                // keeps them out of the gutter where the tick labels sit.
                // (The glyph + chrome passes run earlier with ScissorEnable
                // = FALSE, so the labels themselves are never clipped.)
                // The scissor is in PHYSICAL render-target pixels, so the
                // logical bounds scale by m_ss too (else the clip would land
                // at 2/3 of the strip on a supersampled target).
                const D3D11_RECT sc{
                    static_cast<LONG>(std::floor(plotL  * m_ss)),
                    static_cast<LONG>(std::floor(histoT * m_ss)),
                    static_cast<LONG>(std::ceil(histoR * m_ss)),
                    static_cast<LONG>(std::ceil(histoB * m_ss))};
                m_ctx->RSSetScissorRects(1, &sc);
                m_ctx->RSSetState(m_raster.Get());
                const float bf[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                m_ctx->OMSetBlendState(m_blend.Get(), bf, 0xffffffffu);
                m_ctx->IASetPrimitiveTopology(
                    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

                // Two draw passes per panel. Layering relies on D3D11's
                // documented in-order primitive submission: ALL the chrome —
                // bg + 4 grid lines + budget + 0-ms baseline + left axis
                // (alpha-over) → opaque bars on top. Without that guarantee the
                // alpha-blended chrome could land on the wrong side of the bars.
                //
                // The quad pass is skipped if the quad buffer didn't map this
                // frame (quadOk == false) — never draw over its stale /
                // uninitialized instance data. Mirrors the chrome-shape /
                // glyph-atlas flushes, which suppress on a failed Map rather
                // than compositing garbage. The bar pass is already gated by
                // barCount (which stays 0 if the bar Map failed). A failed
                // DISCARD map of these small fixed buffers is rare (≈ device-
                // removed), so dropping the panel for that frame is the right
                // trade.
                //
                // quad pass — ALL chrome composites UNDER the bars: bg (0) +
                // interior grid lines [1..4] + budget (5) + 0-ms baseline (6) +
                // left axis (7). Drawn before the bars so the bars are the top
                // layer everywhere; count = kQuadSlots.
                if (quadOk) {
                    bindQuadPipeline();
                    m_ctx->DrawInstanced(4, kQuadSlots, 0, 0);
                }

                // bar pass — samples on top of ALL the chrome (instances
                // 0..barCount).
                if (barCount > 0) {
                    bindBarPipeline(m_barCB[cbIdx].GetAddressOf());
                    m_ctx->DrawInstanced(4, barCount, 0, 0);
                }
            }

          private:
            // Per-instance / constant layouts — must match the HLSL byte for
            // byte (see overlay_bars.hlsli / overlay_quad.hlsli). The
            // static_asserts below freeze the C++ side's sizeof so a future
            // member insertion (e.g. a stray float between dashHeight and
            // gradTop) can't silently shift every colour off by 8 bytes.
            // Mirror cbuffer layout: 8×float4 registers (128 B) for bars,
            // 1×float4 register (16 B) for quads.
            struct BarInstance  { float xLeft; float height; uint32_t tier; };
            // The quad instance layout is shared with ChromeShapeRenderer —
            // see overlay_quad_layout (QuadInstance + kQuadInputLayout) in
            // chrome_shape_renderer.h — so the two overlay_quad pipelines
            // (chrome shapes + this bg/grid/axis/budget pass) can't drift.
            using QuadInstance = chrome_shapes::QuadInstance;
            struct BarConstants {
                float texSize[2]; float histoTL[2]; float histoBR[2];
                float barWidth; float dashHeight;
                float gradTop[4]; float gradBottom[4];
                float orange[4]; float red[4]; float dash[4];
                // reg7: supersample factor (= renderer m_ss). The bars VS
                // scales rectPx by it so the PS's analytic edge coverage —
                // computed against SV_Position, which is in PHYSICAL pixels
                // under the supersampled viewport — compares like-for-like.
                // pad keeps the register 16-byte aligned. ss==1 → no-op.
                float supersample; float pad[3];
            };
            struct QuadConstants { float texSize[2]; float pad[2]; };
            static_assert(sizeof(BarConstants)  == 128,
                          "BarConstants must mirror HLSL cbuffer packing "
                          "(8 × float4 = 128 B) — see overlay_bars.hlsli");
            static_assert(sizeof(QuadConstants) == 16,
                          "QuadConstants must mirror HLSL cbuffer packing "
                          "(1 × float4 = 16 B) — see overlay_quad.hlsli");
            // QuadInstance's size assert lives with the shared definition
            // in chrome_shape_renderer.h (overlay_quad_layout).

            // Quad slot layout for the histogram region — ALL of it composites
            // UNDER the bars now (the bars are the top layer). q[0] = bg, then
            // the interior gridlines [kSlotGridFirst, kSlotGridEnd), the budget
            // line, the 0-ms baseline, and the left vertical axis. A bar
            // OCCLUDES any chrome it overlaps, so the bars read in FRONT of every
            // line. drawPanel draws all kQuadSlots quads in one pass before the
            // bars; the count derives from kQuadSlots so adding a slot can't
            // silently desync it. kSlotGridEnd (one past the grid run) stays
            // separate from the budget slot that immediately follows it.
            static constexpr UINT kSlotGridFirst = 1;  // interior gridlines [kSlotGridFirst, kSlotGridEnd)
            static constexpr UINT kSlotGridEnd   = 5;  // one past the last grid slot
            // Trailing single-quad slots derive sequentially from kSlotGridEnd
            // so they can never silently collide with the grid run: bump
            // kSlotGridEnd (e.g. to add an interior gridline) and budget /
            // baseline / axis / total all shift up in lockstep instead of one
            // quietly overwriting a grid slot. Values are unchanged (5/6/7/8).
            static constexpr UINT kSlotBudget    = kSlotGridEnd;       // budget line
            static constexpr UINT kSlotBaseline  = kSlotBudget + 1;    // 0-ms baseline
            static constexpr UINT kSlotAxis      = kSlotBaseline + 1;  // left vertical ms-axis
            static constexpr UINT kQuadSlots     = kSlotAxis + 1;      // total
            // The [kSlotGridFirst, kSlotGridEnd) gridline slots must cover every
            // non-zero tick (kMaxMsAxisTicks counts the 0 tick, drawn as the
            // baseline rather than an interior line). Catches a kMaxMsAxisTicks
            // bump that would otherwise overflow the grid slots at runtime.
            static_assert(kSlotGridEnd - kSlotGridFirst >=
                              static_cast<UINT>(kMaxMsAxisTicks - 1),
                          "too few interior-gridline slots for kMaxMsAxisTicks");

            // Dash pattern for the grid lines + the left ms-axis, in LOGICAL
            // px (constant angular size across supersample factors, like
            // kChromeLineW). ~4 px lit + ~4 px gap reads as a clean dotted grid
            // at the 1.5-px line weight without shimmering through the lens
            // resample. The budget line is drawn WITHOUT these (solid).
            static constexpr float kGridDashOn     = 4.0f;
            static constexpr float kGridDashPeriod = 8.0f;  // on + gap

            // Colours (linear RGBA), copied from initBrushes().
            static constexpr float kPanelBg[4]    = {0.035f, 0.039f, 0.039f, 1.00f};
            // Dashed grid lines + left ms-axis + 0-ms baseline. Mirrors
            // kColorSeparator (the panel-border / cell-separator chrome) at FULL
            // opacity, so the grid reads at the same weight + tone as the box
            // outlines. Replaces a dim 0.30-alpha colour that was barely
            // visible; the kChromeLineW thickness already matched the borders.
            static constexpr float kGridLine[4]   = {0.184f, 0.200f, 0.204f, 1.00f};
            // Dim placeholder dash for an empty (no-sample-yet) histogram slot —
            // kept subtle (low alpha) so warm-up empties don't shout.
            static constexpr float kEmptyDash[4]  = {0.353f, 0.431f, 0.451f, 0.30f};
            // Budget reference line. Two findings drive its colour:
            //  (1) HMD: a bright near-white thin line CA-fringes (violet/pink
            //      magenta) and aliases off-axis, while a dim low-contrast line
            //      renders CLEAN at the same thickness + position — the cause is
            //      BRIGHTNESS, not geometry. So it stays in the grid's clean
            //      low-contrast regime.
            //  (2) The grid is now OPAQUE (kGridLine ≈ the separator chrome), so
            //      this line no longer outshines it: at {0.50,0.52,0.55,0.35} it
            //      sits at roughly the SAME luma as the grid (~0.20 over the
            //      panel bg). The budget is therefore told apart by being SOLID
            //      vs the dashed grid, NOT by brightness — it used to be tuned to
            //      ~1.5× the old translucent (0.30-alpha) grid, a relationship
            //      the grid brightening retired.
            // Tune here: brighter = more prominent but more off-axis aliasing;
            // dimmer = cleaner but closer to the ticks. Bump it if the
            // solid-vs-dashed cue reads too weak on the HMD.
            static constexpr float kBudgetLine[4] = {0.50f, 0.52f, 0.55f, 0.35f};
            static constexpr float kGpuGradTop[4] = {0.157f, 0.878f, 0.898f, 1.00f};
            static constexpr float kGpuGradBot[4] = {0.075f, 0.682f, 0.710f, 1.00f};
            static constexpr float kCpuGradTop[4] = {0.157f, 0.686f, 1.000f, 1.00f};
            static constexpr float kCpuGradBot[4] = {0.047f, 0.561f, 0.847f, 1.00f};
            static constexpr float kOrange[4]     = {1.000f, 0.553f, 0.000f, 1.00f};
            static constexpr float kRed[4]        = {1.000f, 0.196f, 0.235f, 1.00f};

            static void copyColor(float dst[4], const float src[4]) noexcept {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
            }
            static QuadInstance makeQuad(float x, float y, float w, float h,
                                          const float c[4],
                                          float dashPeriod = 0.0f,
                                          float dashOn     = 0.0f) noexcept {
                QuadInstance q{};
                q.rect[0] = x; q.rect[1] = y; q.rect[2] = w; q.rect[3] = h;
                copyColor(q.color, c);
                // radius / borderWidth stay zero-initialised: overlay_quad_ps
                // anti-aliases every quad via the box SDF, so no per-quad
                // radius is needed to soften the thin lines.
                // dash → QUAD_PARAMS.zw: the grid + left-axis lines pass a
                // non-zero period so the PS breaks them into dashes; everything
                // else (bg, budget line) keeps the 0/0 default and stays solid.
                q.dash[0] = dashPeriod;
                q.dash[1] = dashOn;
                return q;
            }

            bool createBuffer(const void* data, UINT bytes, UINT bind,
                               D3D11_USAGE usage, ComPtr<ID3D11Buffer>& out) {
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = bytes;
                bd.Usage = usage;
                bd.BindFlags = bind;
                D3D11_SUBRESOURCE_DATA srd{};
                srd.pSysMem = data;
                return SUCCEEDED(m_device->CreateBuffer(
                    &bd, data ? &srd : nullptr, out.GetAddressOf()));
            }
            bool createDynamic(UINT bytes, UINT bind,
                                ComPtr<ID3D11Buffer>& out) {
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = bytes;
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = bind;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                return SUCCEEDED(m_device->CreateBuffer(
                    &bd, nullptr, out.GetAddressOf()));
            }
            bool upload(ComPtr<ID3D11Buffer>& cb, const void* src, UINT bytes) {
                D3D11_MAPPED_SUBRESOURCE map{};
                if (SUCCEEDED(m_ctx->Map(cb.Get(), 0,
                        D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                    std::memcpy(map.pData, src, bytes);
                    m_ctx->Unmap(cb.Get(), 0);
                    return true;
                }
                return false;
            }
            void bindQuadPipeline() {
                ID3D11Buffer* vbs[2] = {m_quadVB.Get(), m_quadInstances.Get()};
                const UINT strides[2] = {sizeof(float) * 2, sizeof(QuadInstance)};
                const UINT offsets[2] = {0, 0};
                m_ctx->IASetInputLayout(m_quadLayout.Get());
                m_ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
                m_ctx->VSSetShader(m_quadVS.Get(), nullptr, 0);
                m_ctx->PSSetShader(m_quadPS.Get(), nullptr, 0);
                m_ctx->VSSetConstantBuffers(0, 1, m_quadCB.GetAddressOf());
            }
            void bindBarPipeline(ID3D11Buffer* const* barCB) {
                ID3D11Buffer* vbs[2] = {m_quadVB.Get(), m_barInstances.Get()};
                const UINT strides[2] = {sizeof(float) * 2, sizeof(BarInstance)};
                const UINT offsets[2] = {0, 0};
                m_ctx->IASetInputLayout(m_barsLayout.Get());
                m_ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
                m_ctx->VSSetShader(m_barsVS.Get(), nullptr, 0);
                m_ctx->PSSetShader(m_barsPS.Get(), nullptr, 0);
                // BarConstants (b0) feeds BOTH stages: the VS reads the
                // rect/barWidth, the PS reads the tier colours + gradient
                // stops. Binding it only to the VS left the PS's b0 null,
                // so every bar shaded as (0,0,0,0) — transparent, hence
                // "no bars" despite the geometry drawing correctly.
                m_ctx->VSSetConstantBuffers(0, 1, barCB);
                m_ctx->PSSetConstantBuffers(0, 1, barCB);
            }

            bool m_ready = false;
            // Overlay supersample factor. Scales ONLY the flush-time
            // viewport + scissor (physical render target); the cbuffers'
            // texSize stays logical so the bar/grid float rects stretch
            // losslessly. 1.0 == legacy.
            float m_ss = 1.0f;
            ComPtr<ID3D11Device>           m_device;
            ComPtr<ID3D11DeviceContext>    m_ctx;
            // RTV is supplied per drawPanel call (Task 15) so the
            // renderer can paint into any of the N swapchain images.
            ComPtr<ID3D11VertexShader>     m_barsVS;
            ComPtr<ID3D11PixelShader>      m_barsPS;
            ComPtr<ID3D11VertexShader>     m_quadVS;
            ComPtr<ID3D11PixelShader>      m_quadPS;
            ComPtr<ID3D11InputLayout>      m_barsLayout;
            ComPtr<ID3D11InputLayout>      m_quadLayout;
            ComPtr<ID3D11Buffer>           m_quadVB;
            ComPtr<ID3D11Buffer>           m_barInstances;
            ComPtr<ID3D11Buffer>           m_quadInstances;
            ComPtr<ID3D11Buffer>           m_barCB[2];   // [0]=CPU, [1]=GPU panel
            bool                           m_barCBFilled[2] = {false, false};
            ComPtr<ID3D11Buffer>           m_quadCB;
            ComPtr<ID3D11BlendState>       m_blend;
            ComPtr<ID3D11RasterizerState>  m_raster;
        };

        // Watchdog: max frames between forced chrome repaints on the
        // shader path. K=30 (~0.33 s @ 90 Hz, ~0.21 s @ 144 Hz) only
        // fires if the aggregator's snap.version stops ticking — purely
        // defensive. Shared by both renderers so a tweak lands once.
        constexpr int kChromeWatchdogFrames = 30;

        // -------- Shared all-GPU overlay paint --------------------------------
        //
        // Paints the full overlay composite into `rtv` for one frame:
        // chrome shapes (frame / panels / separators), text (labels +
        // values), then the two histogram panels on top.
        //
        // Two paint tiers, gated by needStaticPaint:
        //   * Static REBUILD (cadence-gated, ~10 Hz): paintChromeOnly
        //     rebuilds the chrome-shape + text scratches (string formatting +
        //     the value glyph layout). Heavier than the per-frame flush below,
        //     so it is gated to the publish cadence — though profiling
        //     (xr_telemetry_chrome_rebuild) ranked it the SMALLEST of the
        //     three cadenced contributors to the target_us spike, well behind
        //     the aggregator's percentile sort.
        //   * Per-frame FLUSH + bars: the cached scratches are re-uploaded
        //     and the bars redrawn every frame (cheap — Map-DISCARD + a few
        //     DrawInstanced; the static-label cache keeps the chrome scratch
        //     stable between rebuilds).
        //
        // Both render paths (D3D11 into the acquired swapchain image; D3D12
        // into the D3D11On12-wrapped image) target a ROTATING swapchain
        // image whose contents OpenXR does NOT guarantee across acquire, so
        // every frame ClearRTVs and re-flushes the cached chrome, then the
        // bars. Only the cheap flush is per-frame — the chrome REBUILD stays
        // cadence-gated (~10 Hz).
        //
        // Ordering on the immediate context (static frame): ClearRTV →
        // chrome shapes → text → bars (bottom-to-top).
        //
        // Returns false on a hard chrome rebuild failure (null renderer
        // pointer) or a flush failure; the cadence then retries next frame.
        inline bool paintOverlay(
                CoreRenderer&                       core,
                HistogramBarRenderer&               bars,
                PaintCadence&                       cadence,
                ID3D11DeviceContext*                ctx,
                ID3D11RenderTargetView*             rtv,
                glyph_atlas::Renderer&              gpuText,
                chrome_shapes::Renderer&            gpuShapes,
                const HistogramRing<kRingSize>&     cpuRing,
                const HistogramRing<kRingSize>&     gpuRing,
                const OverlaySnapshot&              snap) {
            if (!ctx || !rtv) return false;

            const bool needStatic = needStaticPaint(
                cadence, snap.version, kChromeWatchdogFrames);

            // Chrome rebuild: string formatting + value-glyph layout, always
            // cadence-gated to ~10 Hz regardless of target. Per the tier note
            // above it is the lightest of the three cadenced spike
            // contributors — not the hotspot the old "heavy tier" name implied.
            bool ok = true;
            if (needStatic) {
                // Spike decomposition (perf/overlay-spike-instrumentation):
                // time the rebuild as a discrete ETW span so the bench harness
                // can split the ~10 Hz target_us spike into its three
                // contributors — {chrome rebuild | aggregator publish | GPU
                // poll}. ScopedQpcSpan is a no-op unless a trace session is
                // listening (no QPC reads, no event emitted).
                log::ScopedQpcSpan span;
                ok = core.paintChromeOnly(&gpuText, &gpuShapes, snap);
                if (span.enabled()) {
                    TraceLoggingWrite(g_traceProvider,
                                      "xr_telemetry_chrome_rebuild",
                                      TLArg(span.elapsedNs(), "duration_ns"),
                                      TLArg(snap.version, "snapshot_version"));
                }
            }

            // Chrome FLUSH (ClearRTV + re-upload the cached chrome scratch):
            // every frame, because the swapchain image rotates and OpenXR
            // doesn't guarantee its contents across acquire — the freshly-
            // acquired image must carry the current chrome. Cheap: the cached
            // scratch is re-uploaded, NOT the ~10 Hz rebuild above. Gated
            // only on `ok` so a failed rebuild doesn't flush a half-built
            // scratch.
            const bool flushChrome = ok;
            if (flushChrome) {
                const float kClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                ctx->ClearRenderTargetView(rtv, kClear);
                // If a chrome flush fails (transient Map/grow under
                // memory pressure) AFTER we've cleared the target,
                // suppress the whole frame — otherwise we'd composite
                // bars + whatever-drew over a blank background. The
                // cadence keeps needStatic set so the next frame retries
                // the rebuild. (Both flushes run; we don't short-circuit
                // so a partial draw still lands consistently before we
                // bail.)
                const bool shapesOk = gpuShapes.flush(rtv);
                const bool textOk   = gpuText.flush(rtv);
                if (!shapesOk || !textOk) {
                    commitPaint(cadence, needStatic, /*ok=*/false, snap.version);
                    return false;
                }
            }

            // Dynamic tier — bars every frame (the histogram scrolls at
            // the host rate). Skipped only if a static rebuild failed
            // mid-frame, to avoid drawing bars over a half-cleared target.
            if (ok) {
                // Panel Ys + histo rects from the shared layout constants;
                // identical geometry to drawChrome so the bars line up
                // under the panel titles.
                const float gpuPanelY =
                    kInnerT + kHeaderHeight + kSectionGap;
                const float cpuPanelY =
                    gpuPanelY + kFrametimeHeight + kSectionGap;
                const float histoL = kInnerL + kSectionInnerPad;
                const float histoR = kInnerR - kSectionInnerPad;
                const int64_t budgetNs = snap.target_fps > 0.0f
                    ? static_cast<int64_t>(1.0e9f / snap.target_fps)
                    : 0;
                const float gpuT = gpuPanelY + kPanelTitleTopPad +
                                    kHistoTitleH + kHistoTitleGap;
                const float gpuB = gpuPanelY + kFrametimeHeight -
                                    kSectionInnerPad;
                const float cpuT = cpuPanelY + kPanelTitleTopPad +
                                    kHistoTitleH + kHistoTitleGap;
                const float cpuB = cpuPanelY + kFrametimeHeight -
                                    kSectionInnerPad;
                bars.drawPanel(rtv, gpuRing, budgetNs, snap.target_fps,
                                histoL, gpuT, histoR, gpuB,
                                /*isGpu=*/true);
                bars.drawPanel(rtv, cpuRing, budgetNs, snap.target_fps,
                                histoL, cpuT, histoR, cpuB,
                                /*isGpu=*/false);
            }

            commitPaint(cadence, needStatic, ok, snap.version);
            return ok;
        }

        // Bring up the three GPU render pipelines (bars + glyph atlas + chrome
        // shapes) against a device/context at the production supersample, then
        // free the CPU atlas bitmap. Shared by BOTH backends' init() so the
        // dst/physical dimensions and the bring-up order can't drift between
        // D3D11 and D3D12 (the swapchain + RTV plumbing around it genuinely
        // differs and stays per-class). Fail-closed: logs + returns false on
        // any failure so the caller disables the overlay.
        inline bool initOverlayRenderers(ID3D11Device*            dev,
                                          ID3D11DeviceContext*     ctx,
                                          CoreRenderer&            core,
                                          HistogramBarRenderer&    bars,
                                          glyph_atlas::Renderer&   gpuText,
                                          chrome_shapes::Renderer& gpuShapes) {
            if (!bars.init(dev, ctx, kEffectiveSupersample)) {
                Log("xr_telemetry: overlay disabled — bars init failed\n");
                return false;
            }
            if (!core.atlasReady()) {
                Log("xr_telemetry: overlay disabled — glyph atlas not built "
                    "(DirectWrite face resolution failed)\n");
                return false;
            }
            // dst = LOGICAL design space (cbuffer texSize); render = PHYSICAL
            // swapchain-image extent (viewport). They differ only when
            // supersampled — the renderer maps between them.
            if (!gpuText.init(dev, ctx, static_cast<UINT>(kTexW),
                               static_cast<UINT>(kTexH), core.atlas(),
                               static_cast<UINT>(kTexWPhys),
                               static_cast<UINT>(kTexHPhys))) {
                Log("xr_telemetry: overlay disabled — glyph renderer init "
                    "failed\n");
                return false;
            }
            if (!gpuShapes.init(dev, ctx, static_cast<UINT>(kTexW),
                                 static_cast<UINT>(kTexH),
                                 static_cast<UINT>(kTexWPhys),
                                 static_cast<UINT>(kTexHPhys))) {
                Log("xr_telemetry: overlay disabled — chrome shapes renderer "
                    "init failed\n");
                return false;
            }
            // Glyph texture uploaded — free the CPU atlas bitmap (8 MB at 2.0×)
            // so it isn't resident for the host process's lifetime.
            core.releaseAtlasBitmap();
            return true;
        }

        // -------- D3D11 native renderer --------------------------------------
        //
        // App uses D3D11 directly. Each swapchain image is an ID3D11Texture2D
        // we obtain via xrEnumerateSwapchainImages, and we create one D3D11
        // render-target view per image (cached for the swapchain's lifetime);
        // the shaders paint straight into the acquired image.
        class D3D11OverlayRenderer final : public OverlayRenderer {
          public:
            D3D11OverlayRenderer(OpenXrApi* api, XrSession session,
                                  ID3D11Device* device, xrprof::Probe* probe)
                : m_api(api), m_session(session), m_device(device) {
                m_probe = probe;  // assigned (not init-list) to avoid -Wreorder
                if (m_device) m_device->GetImmediateContext(m_context.GetAddressOf());
                m_ready = init();
                // Attach the inline GPU timer to the device we paint on (the
                // app's D3D11 device, shared via SwapDeviceContextState). No-op
                // if profiling is off / device null.
                if (m_ready && m_probe && m_device) {
                    m_probe->attachD3D11(m_device.Get());
                }
            }

            ~D3D11OverlayRenderer() override {
                // Release the per-image RTVs + texture handles (which alias
                // the swapchain's images) BEFORE destroying the swapchain —
                // a runtime that frees image backing at xrDestroySwapchain
                // would otherwise leave us holding dangling views.
                m_imageRtvs.clear();
                m_images.clear();
                if (m_swapchain != XR_NULL_HANDLE && m_api) {
                    m_api->xrDestroySwapchain(m_swapchain);
                }
            }

            bool isReady() const noexcept override { return m_ready; }

            void pushFrameSample(int64_t cpu_per_cycle_ns,
                                  int64_t gpu_time_ns) override {
                m_cpuRing.push(cpu_per_cycle_ns);
                m_gpuRing.push(gpu_time_ns);
            }

            const XrCompositionLayerBaseHeader* renderAndCompose(
                XrSpace space,
                const XrPosef* anchorPose,
                const OverlaySnapshot& snap,
                const OverlayGeometry& geo) override {
                if (!m_ready || !snap.valid) return nullptr;

                // 1. Acquire + wait the next swapchain image.
                uint32_t imageIdx = 0;
                XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if (XR_FAILED(m_api->xrAcquireSwapchainImage(
                        m_swapchain, &acquireInfo, &imageIdx))) {
                    return nullptr;
                }
                XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                waitInfo.timeout = XR_INFINITE_DURATION;
                if (XR_FAILED(m_api->xrWaitSwapchainImage(m_swapchain, &waitInfo))) {
                    m_api->xrReleaseSwapchainImage(m_swapchain, nullptr);
                    return nullptr;
                }

                // 2. Paint chrome + text + bars DIRECTLY into the
                //    acquired swapchain image's RTV — no intermediate
                //    texture, no CopyResource, no Flush.
                //
                //    History: the green-band / vanishing-cubes bug that
                //    plagued the first direct-to-swapchain attempt was a
                //    pipeline-STATE LEAK (we share the app's immediate
                //    context; xrEndFrame runs on the app's render thread,
                //    so state we leave bound corrupts the app's next
                //    frame). The intermediate+CopyResource and the Flush
                //    were added chasing that symptom on wrong hypotheses
                //    — they never fixed it. SwapDeviceContextState (the
                //    state isolation below) did. With the leak fixed,
                //    direct-to-swapchain renders correctly, so the copy
                //    and flush are gone.
                //
                //    SwapDeviceContextState swaps in a private,
                //    default-initialised state block for our draws and
                //    hands the app's full pipeline state back after. The
                //    RAII guard restores it on every exit path (incl. a
                //    std::bad_alloc from paintOverlay's allocations); the
                //    catch(...) keeps the throw from crossing the
                //    xrEndFrame C-ABI boundary. Both satisfy "never
                //    crash / never corrupt the host".
                //
                //    OpenXR doesn't guarantee swapchain image contents
                //    across acquire, so the chrome is re-flushed every
                //    frame (the cheap cached-scratch upload; the 10 Hz
                //    chrome REBUILD is still gated).
                //
                //    LIMITATION: this only isolates the app's IMMEDIATE
                //    context. An app that submits its frame from a
                //    DEFERRED context, or from another thread racing
                //    xrEndFrame, isn't serialised by our swap — on such
                //    an engine the state-leak corruption could resurface.
                //    Not observed on the tested runtimes (Pimax + Dirt /
                //    LMU / HelloXR, all immediate-context within the
                //    xrEndFrame thread), and the overlay's
                //    disable_environment escape hatch lets a user kill
                //    the layer if it ever does. The fully-robust
                //    alternative is a dedicated private device + per-frame
                //    CopyResource — the model both paths used originally —
                //    but that's the per-frame copy we removed for a ~6x CPU
                //    win (D3D11 here via state isolation; D3D12 by rendering
                //    into the D3D11On12-wrapped image directly), so we take
                //    the immediate-context bet and document it.
                bool painted = false;
                {
                    m_context1->SwapDeviceContextState(
                        m_overlayState.Get(),
                        m_savedState.ReleaseAndGetAddressOf());
                    struct StateRestoreGuard {
                        ID3D11DeviceContext1*   ctx;
                        ID3DDeviceContextState* appState;
                        ~StateRestoreGuard() {
                            if (ctx && appState)
                                ctx->SwapDeviceContextState(appState, nullptr);
                        }
                    } stateGuard{m_context1.Get(), m_savedState.Get()};

                    try {
                        if (imageIdx < m_imageRtvs.size()) {
                            // GPU self-profiling: bracket the overlay paint with
                            // an inline GPU span on our context. No-op if the
                            // probe is absent / profiling off. endFrame (layer.cpp)
                            // commits the matching row; gpu_ns resolves a few
                            // frames later.
                            std::optional<xrprof::Probe::GpuScope> gpu;
                            if (m_probe) {
                                gpu.emplace(*m_probe, m_context.Get());
                            }
                            painted =
                                paintOverlay(m_core, m_bars, m_barsCadence,
                                              m_context.Get(),
                                              m_imageRtvs[imageIdx].Get(),
                                              m_glyphRenderer,
                                              m_chromeShapeRenderer,
                                              m_cpuRing, m_gpuRing, snap);
                            gpu.reset();  // record the close timestamp after the draws
                            // Unbind our RTV so the swapchain image isn't
                            // left OM-bound when the runtime composites it.
                            ID3D11RenderTargetView* nullRtv = nullptr;
                            m_context->OMSetRenderTargets(1, &nullRtv, nullptr);
                        }
                    } catch (...) {
                        // Paint-time throw (OOM etc.): skip the overlay
                        // this frame. stateGuard still restores the
                        // app's context state on unwind out of this block.
                        painted = false;
                    }
                }   // stateGuard restores the app's pipeline state here

                // 4. Release regardless of paint success — the runtime
                //    needs the image released to keep the swapchain
                //    cycling.
                XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                m_api->xrReleaseSwapchainImage(m_swapchain, &releaseInfo);

                if (!painted) return nullptr;

                // 4. Build the composition layer pose. The immutable
                //    fields (type, layerFlags, swapchain ref, imageRect)
                //    were filled once in init(); here we write the space,
                //    pose, and size that vary per frame from the caller-
                //    supplied geometry.
                //
                //    Two anchor modes share this path (see OverlayAnchor):
                //      - head-locked  → `space` is the VIEW space, pose =
                //        identity orientation + geo's position offset.
                //      - world-locked → `space` is the LOCAL space and
                //        `anchorPose` carries the world pose the caller
                //        froze at activation; we take it verbatim and geo
                //        only contributes the quad size.
                applyQuadPose(m_quadLayer, space, anchorPose, geo);

                return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_quadLayer);
            }

          private:
            bool init() {
                if (!m_device || !m_api || m_session == XR_NULL_HANDLE) return false;
                if (!m_core.init(kEffectiveSupersample)) {
                    Log("xr_telemetry: overlay DirectWrite / glyph-atlas init failed; HUD disabled\n");
                    return false;
                }
                const int64_t format = pickSwapchainFormat(m_api, m_session);
                if (format == 0) {
                    Log("xr_telemetry: overlay disabled — runtime advertises no "
                        "accepted BGRA8/RGBA8 swapchain format (see list above)\n");
                    return false;
                }

                XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                  XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                sci.format = format;
                sci.sampleCount = 1;
                sci.width = static_cast<uint32_t>(kTexWPhys);
                sci.height = static_cast<uint32_t>(kTexHPhys);
                sci.faceCount = 1;
                sci.arraySize = 1;
                sci.mipCount = 1;
                if (XR_FAILED(m_api->xrCreateSwapchain(m_session, &sci, &m_swapchain))) {
                    Log("xr_telemetry: overlay disabled — xrCreateSwapchain failed\n");
                    return false;
                }

                uint32_t imgCount = 0;
                if (XR_FAILED(m_api->xrEnumerateSwapchainImages(
                        m_swapchain, 0, &imgCount, nullptr)) || imgCount == 0) {
                    Log("xr_telemetry: overlay disabled — xrEnumerateSwapchainImages "
                        "returned 0 images\n");
                    return false;
                }
                std::vector<XrSwapchainImageD3D11KHR> raw(
                    imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                if (XR_FAILED(m_api->xrEnumerateSwapchainImages(
                        m_swapchain, imgCount, &imgCount,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(raw.data())))) {
                    Log("xr_telemetry: overlay disabled — xrEnumerateSwapchainImages "
                        "second pass failed\n");
                    return false;
                }

                // Stash the OpenXR swapchain images; the per-image RTVs
                // are created below with an explicit typed view desc (the
                // resource comes back typeless — BGRA8_TYPELESS on Pimax,
                // sRGB-typeless on SteamVR — so the view format is chosen by
                // rtvFormatForSwapchain, not the resource). The diagnostic log
                // on image 0 stays around for future format / bindFlags
                // regressions.
                m_images.resize(imgCount);
                for (uint32_t i = 0; i < imgCount; ++i) {
                    m_images[i] = raw[i].texture;
                    if (i == 0) {
                        D3D11_TEXTURE2D_DESC desc{};
                        raw[i].texture->GetDesc(&desc);
                        Log(fmt::format(
                            "xr_telemetry: overlay swapchain image[0] format={}, "
                            "bindFlags={:#x}, usage={}, sampleCount={}, mipLevels={}\n",
                            static_cast<int>(desc.Format),
                            static_cast<unsigned int>(desc.BindFlags),
                            static_cast<int>(desc.Usage),
                            static_cast<int>(desc.SampleDesc.Count),
                            static_cast<int>(desc.MipLevels)));
                    }
                }

                // Diagnostic for the app's D3D11 device. Task 15 made
                // direct-to-swapchain painting possible, so the
                // BGRA_SUPPORT flag is no longer load-bearing for the
                // hot path — the GPU shaders (overlay_quad / overlay_
                // bars / overlay_text) work on any device. We still
                // log it for triage.
                const UINT appFlags = m_device->GetCreationFlags();
                const D3D_FEATURE_LEVEL appLevel = m_device->GetFeatureLevel();
                const bool appHasBgra =
                    (appFlags & D3D11_CREATE_DEVICE_BGRA_SUPPORT) != 0;
                Log(fmt::format(
                    "xr_telemetry: app D3D11 device flags={:#x} "
                    "(BGRA_SUPPORT={}), feature_level={:#x}\n",
                    static_cast<unsigned int>(appFlags),
                    appHasBgra ? "yes" : "no",
                    static_cast<unsigned int>(appLevel)));

                // m_context was captured by the constructor; sanity-
                // check we didn't somehow land here without it.
                if (!m_context) {
                    Log("xr_telemetry: overlay disabled — app device "
                        "context not captured\n");
                    return false;
                }

                // D3D11.1 state isolation. We render on the app's
                // immediate context, so we MUST NOT leak pipeline
                // state into the app's own rendering. CreateDevice
                // ContextState gives us a private, default-init state
                // block; SwapDeviceContextState (per frame) swaps it
                // in for our draws and hands back the app's state to
                // restore afterwards. Without this the app inherits
                // our shaders / blend / RTV and renders garbage.
                if (FAILED(m_device.As(&m_device1))) {
                    Log("xr_telemetry: overlay disabled — app device "
                        "QueryInterface ID3D11Device1 failed (need "
                        "D3D11.1 for state isolation)\n");
                    return false;
                }
                if (FAILED(m_context.As(&m_context1))) {
                    Log("xr_telemetry: overlay disabled — app context "
                        "QueryInterface ID3D11DeviceContext1 failed\n");
                    return false;
                }
                // Mirror the app device's threading mode into the
                // context-state object. MS docs: if the device was
                // created with D3D11_CREATE_DEVICE_SINGLETHREADED, every
                // context-state object created from it MUST carry
                // D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED —
                // otherwise CreateDeviceContextState fails (overlay then
                // silently disabled) or the locking contract is violated.
                const UINT ctxStateFlags =
                    (appFlags & D3D11_CREATE_DEVICE_SINGLETHREADED)
                        ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                        : 0u;
                const D3D_FEATURE_LEVEL ctxStateLevels[] = { appLevel };
                D3D_FEATURE_LEVEL chosenLevel{};
                if (FAILED(m_device1->CreateDeviceContextState(
                        ctxStateFlags,
                        ctxStateLevels, 1,
                        D3D11_SDK_VERSION,
                        __uuidof(ID3D11Device1),
                        &chosenLevel,
                        m_overlayState.GetAddressOf()))) {
                    Log("xr_telemetry: overlay disabled — "
                        "CreateDeviceContextState failed\n");
                    return false;
                }

                // One RTV per swapchain image — we paint chrome + text +
                // bars DIRECTLY into the acquired image each frame (no
                // intermediate texture, no CopyResource). The runtime's
                // images can come back typeless (e.g. Pimax/SteamVR), so we
                // give the RTV an explicit typed desc. rtvFormatForSwapchain
                // maps an sRGB swapchain to its UNORM sibling so we don't
                // double-encode the colours; createOverlayImageRtv retries
                // with the sRGB view if the resource is typed-sRGB not typeless.
                const DXGI_FORMAT rtvFormat = rtvFormatForSwapchain(format);
                const DXGI_FORMAT rtvFallback = static_cast<DXGI_FORMAT>(format);
                m_imageRtvs.resize(m_images.size());
                for (size_t i = 0; i < m_images.size(); ++i) {
                    if (!createOverlayImageRtv(m_device.Get(), m_images[i].Get(),
                                               rtvFormat, rtvFallback,
                                               m_imageRtvs[i].GetAddressOf())) {
                        Log(fmt::format(
                            "xr_telemetry: overlay disabled — "
                            "CreateRenderTargetView (swapchain image[{}]) "
                            "failed\n", i));
                        return false;
                    }
                }
                // GPU pipelines (bars + glyph atlas + chrome shapes). All run
                // on the APP's device + context, isolated per-frame via
                // SwapDeviceContextState. Fail-closed (no D2D fallback). Shared
                // with the D3D12 backend so the two bring-ups can't drift.
                if (!initOverlayRenderers(m_device.Get(), m_context.Get(),
                                           m_core, m_bars, m_glyphRenderer,
                                           m_chromeShapeRenderer))
                    return false;

                // Fill the immutable XrCompositionLayerQuad fields
                // now that m_swapchain is alive. SOURCE_ALPHA blending
                // + identity orientation are documented in the long
                // comment up in renderAndCompose; everything else
                // (space, pose.position, size) is per-frame.
                initQuadLayerConstants();

                Log(fmt::format(
                    "xr_telemetry: overlay D3D11 renderer ready ({} swapchain "
                    "images, swapchain format={}{}, RT format={}, "
                    "feature_level={:#x})\n",
                    imgCount, format,
                    isSrgbSwapchainFormat(format) ? " (sRGB)" : "",
                    static_cast<int>(rtvFormat),
                    static_cast<unsigned int>(appLevel)));
                return true;
            }

            // Shader-path paint is the free function paintOverlay()
            // declared above the class — same body served both
            // D3D11OverlayRenderer and D3D12OverlayRenderer, so the
            // duplicate per-class definitions are gone.

            // One-time fill of the XrCompositionLayerQuad fields that
            // never change frame-to-frame. SOURCE_ALPHA without
            // UNPREMULTIPLIED_ALPHA_BIT matches the D2D RT's
            // premultiplied alpha; identity orientation displays the
            // quad facing the user since view-space's -Z is the
            // gaze direction. Called once at end of init(); paint
            // path only writes space + pose.position + size.
            void initQuadLayerConstants() {
                m_quadLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                m_quadLayer.next = nullptr;
                m_quadLayer.layerFlags =
                    XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                m_quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                m_quadLayer.subImage.swapchain = m_swapchain;
                m_quadLayer.subImage.imageRect.offset = {0, 0};
                m_quadLayer.subImage.imageRect.extent = {kTexWPhys, kTexHPhys};
                m_quadLayer.subImage.imageArrayIndex = 0;
                m_quadLayer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            }

            OpenXrApi*                  m_api = nullptr;
            XrSession                   m_session = XR_NULL_HANDLE;
            // App's D3D11 device + context. All GPU paint work goes
            // through these now — no second device, no cross-device
            // share, no keyed mutex.
            ComPtr<ID3D11Device>        m_device;
            ComPtr<ID3D11DeviceContext> m_context;
            xrprof::Probe*              m_probe = nullptr;  // not owned; inline GPU timer
            // D3D11.1 views of the same device/context, for state
            // isolation (SwapDeviceContextState). m_overlayState is
            // our private pipeline-state block; m_savedState holds the
            // app's state for the duration of one overlay paint.
            ComPtr<ID3D11Device1>        m_device1;
            ComPtr<ID3D11DeviceContext1> m_context1;
            ComPtr<ID3DDeviceContextState> m_overlayState;
            ComPtr<ID3DDeviceContextState> m_savedState;
            XrSwapchain                 m_swapchain = XR_NULL_HANDLE;
            // OpenXR swapchain images (typeless on the tested runtimes) +
            // one typed RTV each, in the UNORM format rtvFormatForSwapchain
            // picks (BGRA8 or RGBA8; never sRGB). We paint directly into the
            // acquired image's RTV every frame — no intermediate, no
            // CopyResource. State isolation (SwapDeviceContextState)
            // keeps our pipeline changes from leaking into the app.
            std::vector<ComPtr<ID3D11Texture2D>>          m_images;
            std::vector<ComPtr<ID3D11RenderTargetView>>   m_imageRtvs;

            // GPU pipelines, all on the APP device. init() fails closed
            // on any of them (overlay disabled, no D2D fallback), so by
            // the time we paint all three are live — no per-renderer
            // enable flag needed.
            HistogramBarRenderer        m_bars;
            glyph_atlas::Renderer       m_glyphRenderer;
            chrome_shapes::Renderer     m_chromeShapeRenderer;
            // Chrome cadence: drawChrome rebuild only when snap.version
            // ticks or the watchdog fires (kChromeWatchdogFrames at
            // namespace scope). The rebuilt scratches re-flush every
            // frame into the paint target.
            PaintCadence                m_barsCadence;

            CoreRenderer                m_core;
            HistogramRing<kRingSize>    m_cpuRing;
            HistogramRing<kRingSize>    m_gpuRing;
            XrCompositionLayerQuad      m_quadLayer{};
            // Geometry (centre offset + size) arrives per frame in
            // renderAndCompose — the caller owns it, computed once from the
            // immutable overlay settings — so the renderer caches nothing.
            bool                        m_ready = false;
        };

        // -------- D3D12 renderer (via D3D11On12 bridge) ---------------------
        //
        // App uses D3D12. Our GPU shader pipelines are D3D11, so we wrap
        // the app's D3D12 device into an ID3D11Device via
        // D3D11On12CreateDevice. Each swapchain image (ID3D12Resource*) is
        // bridged to an ID3D11Resource via CreateWrappedResource (with
        // BIND_RENDER_TARGET), and we create one D3D11 RTV per wrapped
        // image.
        //
        // We paint the chrome / text / bars shaders DIRECTLY into the
        // acquired wrapped image — no private shim, no per-frame
        // CopyResource (the D3D11 path's direct-to-swapchain win, mirrored
        // here). Per-frame dance:
        //   1. Acquire+wait the OpenXR swapchain image.
        //   2. D3D11On12::AcquireWrappedResources(image) — flips it into the
        //      RENDER_TARGET state so D3D11 can write it.
        //   3. paintOverlay → the image's D3D11 RTV (chrome shapes + glyph
        //      atlas + bars), repainted fully each frame.
        //   4. D3D11On12::ReleaseWrappedResources(image) — flips it back to
        //      PIXEL_SHADER_RESOURCE for the runtime to composite.
        //   5. ID3D11DeviceContext::Flush() — pumps the D3D11On12 command
        //      list into the underlying D3D12 queue.
        //   6. xrReleaseSwapchainImage.
        class D3D12OverlayRenderer final : public OverlayRenderer {
          public:
            D3D12OverlayRenderer(OpenXrApi* api, XrSession session,
                                  ID3D12Device* device,
                                  ID3D12CommandQueue* queue, xrprof::Probe* probe)
                : m_api(api), m_session(session),
                  m_d3d12Device(device), m_d3d12Queue(queue) {
                m_probe = probe;  // assigned (not init-list) to avoid -Wreorder
                m_ready = init();
                // Attach the inline GPU timer to the D3D11On12-bridged device we
                // paint on (built in init()). renderAndCompose brackets the paint
                // with a GpuScope on that bridge context, so D3D12 hosts get GPU
                // timing too (timestamps ride the same Flush to the D3D12 queue).
                if (m_ready && m_probe && m_d3d11Device) {
                    m_probe->attachD3D11(m_d3d11Device.Get());
                }
            }

            ~D3D12OverlayRenderer() override {
                // Release our D3D11 views + wrapped-resource handles (which
                // alias the swapchain's D3D12 textures) BEFORE destroying the
                // swapchain. Flush first so no in-flight bridge work still
                // references them; on runtimes that free image backing at
                // xrDestroySwapchain, releasing the wrappers afterward would
                // be a use-after-free.
                if (m_d3d11Context) m_d3d11Context->Flush();
                m_imageRtvs.clear();
                m_wrappedResources.clear();
                if (m_swapchain != XR_NULL_HANDLE && m_api) {
                    m_api->xrDestroySwapchain(m_swapchain);
                }
            }

            bool isReady() const noexcept override { return m_ready; }

            void pushFrameSample(int64_t cpu_per_cycle_ns,
                                  int64_t gpu_time_ns) override {
                m_cpuRing.push(cpu_per_cycle_ns);
                m_gpuRing.push(gpu_time_ns);
            }

            const XrCompositionLayerBaseHeader* renderAndCompose(
                XrSpace space,
                const XrPosef* anchorPose,
                const OverlaySnapshot& snap,
                const OverlayGeometry& geo) override {
                if (!m_ready || !snap.valid) return nullptr;

                uint32_t imageIdx = 0;
                XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if (XR_FAILED(m_api->xrAcquireSwapchainImage(
                        m_swapchain, &acquireInfo, &imageIdx))) {
                    return nullptr;
                }
                XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                waitInfo.timeout = XR_INFINITE_DURATION;
                if (XR_FAILED(m_api->xrWaitSwapchainImage(m_swapchain, &waitInfo))) {
                    m_api->xrReleaseSwapchainImage(m_swapchain, nullptr);
                    return nullptr;
                }

                // Paint the overlay DIRECTLY into the acquired wrapped
                // D3D12 swapchain image — no shim, no per-frame CopyResource
                // (the D3D11 path's direct-to-swapchain win, mirrored here).
                // The wrapped images rotate and OpenXR doesn't guarantee
                // content across acquire, so we repaint fully each frame;
                // the cached static chrome makes that cheap.
                //
                // AcquireWrappedResources flips the image into the D3D11-
                // writable RENDER_TARGET state before we draw; Release flips
                // it back to PIXEL_SHADER_RESOURCE for the runtime to
                // composite. They bracket the paint so the resource state is
                // ALWAYS restored — even if paintOverlay throws (it
                // allocates display-value strings / per-segment vectors, so
                // an OOM throw is possible; it must not cross the xrEndFrame
                // C-ABI boundary). No state isolation is needed: the
                // D3D11On12 bridge is a separate device from the app's, so
                // the shared-context state-leak that bit the D3D11 path
                // can't occur here.
                // m_imageRtvs and m_wrappedResources are resized to imgCount
                // together in init(), so one bounds check covers both (same
                // invariant the D3D11 path checks). A desync should be
                // impossible; log once if it ever happens so it surfaces as
                // a diagnostic rather than an intermittently-vanishing HUD.
                const bool idxOk = imageIdx < m_imageRtvs.size();
                if (!idxOk) {
                    static bool s_loggedOob = false;
                    if (!s_loggedOob) {
                        s_loggedOob = true;
                        // fmt::format + Log allocate and can throw; this runs
                        // on the xrEndFrame render thread, so an escaping
                        // exception would cross the extern-"C" boundary (the
                        // paint below is wrapped for exactly this reason).
                        try {
                            Log(fmt::format(
                                "xr_telemetry: overlay D3D12 image index {} out "
                                "of range ({} images) — overlay skipped\n",
                                imageIdx, m_imageRtvs.size()));
                        } catch (...) {}
                    }
                }
                ID3D11Resource* wrapped =
                    idxOk ? m_wrappedResources[imageIdx].Get() : nullptr;
                bool painted = false;
                if (wrapped && m_d3d11Context && m_d3d11On12) {
                    m_d3d11On12->AcquireWrappedResources(&wrapped, 1);
                    // RAII: ALWAYS unbind our RTV + release the wrapped
                    // resource on the way out — even if paintOverlay throws,
                    // or a future early-return is added between here and the
                    // scope end. Leaving the image stuck in RENDER_TARGET
                    // state would make the runtime composite garbage. Mirrors
                    // the D3D11 path's StateRestoreGuard discipline.
                    struct WrappedReleaseGuard {
                        ID3D11On12Device*    on12;
                        ID3D11DeviceContext* ctx;
                        ID3D11Resource*      res;
                        ~WrappedReleaseGuard() {
                            if (!on12 || !ctx || !res) return;
                            // Unbind first so the resource isn't OM-bound
                            // when it flips to PIXEL_SHADER_RESOURCE
                            // (silences the debug layer's RT hazard).
                            ID3D11RenderTargetView* nullRtv = nullptr;
                            ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
                            on12->ReleaseWrappedResources(&res, 1);
                        }
                    } releaseGuard{m_d3d11On12.Get(), m_d3d11Context.Get(),
                                    wrapped};
                    try {
                        // GPU self-profiling: bracket the D3D11On12 paint with an
                        // inline GPU span on the bridge context. The Flush below
                        // pushes our timestamps + the paint to the app's D3D12
                        // queue together, so the span measures the overlay's GPU
                        // work. No-op if profiling is off.
                        std::optional<xrprof::Probe::GpuScope> gpu;
                        if (m_probe) {
                            gpu.emplace(*m_probe, m_d3d11Context.Get());
                        }
                        painted =
                            paintOverlay(m_core, m_bars, m_barsCadence,
                                          m_d3d11Context.Get(),
                                          m_imageRtvs[imageIdx].Get(),
                                          m_glyphRenderer,
                                          m_chromeShapeRenderer,
                                          m_cpuRing, m_gpuRing, snap);
                        gpu.reset();  // close the GPU span (before the Flush)
                    } catch (...) {
                        painted = false;
                    }
                }
                // Flush the D3D11On12 command list into the D3D12 queue so
                // the runtime sees the painted image when it composites.
                if (m_d3d11Context) m_d3d11Context->Flush();

                XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                m_api->xrReleaseSwapchainImage(m_swapchain, &releaseInfo);

                if (!painted) return nullptr;

                // Immutable quad fields filled once in init(); per-frame
                // writes the space + pose + size from the caller-supplied
                // geometry. Head-locked uses geo's position offset,
                // world-locked takes the caller's frozen anchorPose (see the
                // D3D11 path's longer comment).
                applyQuadPose(m_quadLayer, space, anchorPose, geo);

                return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_quadLayer);
            }

          private:
            bool init() {
                if (!m_d3d12Device || !m_d3d12Queue || !m_api ||
                    m_session == XR_NULL_HANDLE) return false;

                // 1. Wrap the D3D12 device via D3D11On12CreateDevice.
                //    The D3D11 device this returns shares the same
                //    underlying D3D12 device, so our D3D11 shader draws
                //    end up in the D3D12 queue's command stream.
                //
                //    D3D11On12CreateDevice takes raw `IUnknown*` (not
                //    ComPtr<>) for both the D3D12 device and the queue
                //    array. ComPtr<>::Get() returns the underlying T*,
                //    which IUnknown is a base of — a static_cast makes
                //    the upcast explicit so MSVC can't get confused.
                //    The queue is passed as a length-1 array of
                //    IUnknown*.
                IUnknown* queueAsUnknown = static_cast<IUnknown*>(m_d3d12Queue.Get());
                if (FAILED(::D3D11On12CreateDevice(
                        static_cast<IUnknown*>(m_d3d12Device.Get()),
                        D3D11_CREATE_DEVICE_BGRA_SUPPORT,    // mandatory for D2D
                        nullptr, 0,
                        &queueAsUnknown, 1,
                        0,
                        m_d3d11Device.GetAddressOf(),
                        m_d3d11Context.GetAddressOf(),
                        nullptr))) {
                    Log("xr_telemetry: overlay disabled — D3D11On12CreateDevice "
                        "failed\n");
                    return false;
                }
                if (FAILED(m_d3d11Device.As(&m_d3d11On12))) {
                    Log("xr_telemetry: overlay disabled — QueryInterface "
                        "ID3D11On12Device failed\n");
                    return false;
                }

                if (!m_core.init(kEffectiveSupersample)) {
                    Log("xr_telemetry: overlay DirectWrite / glyph-atlas init failed; HUD disabled\n");
                    return false;
                }
                const int64_t format = pickSwapchainFormat(m_api, m_session);
                if (format == 0) {
                    Log("xr_telemetry: overlay disabled — runtime advertises no "
                        "accepted BGRA8/RGBA8 swapchain format for the D3D12 path\n");
                    return false;
                }

                XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                  XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                sci.format = format;
                sci.sampleCount = 1;
                sci.width = static_cast<uint32_t>(kTexWPhys);
                sci.height = static_cast<uint32_t>(kTexHPhys);
                sci.faceCount = 1;
                sci.arraySize = 1;
                sci.mipCount = 1;
                if (XR_FAILED(m_api->xrCreateSwapchain(m_session, &sci, &m_swapchain))) {
                    Log("xr_telemetry: overlay disabled — xrCreateSwapchain "
                        "(D3D12 path) failed\n");
                    return false;
                }

                uint32_t imgCount = 0;
                if (XR_FAILED(m_api->xrEnumerateSwapchainImages(
                        m_swapchain, 0, &imgCount, nullptr)) || imgCount == 0) {
                    Log("xr_telemetry: overlay disabled — D3D12 swapchain returned "
                        "0 images\n");
                    return false;
                }
                std::vector<XrSwapchainImageD3D12KHR> raw(
                    imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                if (XR_FAILED(m_api->xrEnumerateSwapchainImages(
                        m_swapchain, imgCount, &imgCount,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(raw.data())))) {
                    Log("xr_telemetry: overlay disabled — D3D12 swapchain enum "
                        "second pass failed\n");
                    return false;
                }

                m_wrappedResources.resize(imgCount);
                for (uint32_t i = 0; i < imgCount; ++i) {
                    D3D11_RESOURCE_FLAGS flags{};
                    flags.BindFlags = D3D11_BIND_RENDER_TARGET;
                    // Wrap the D3D12 texture so D3D11 (and D2D) can
                    // paint into it. The "in" state is RENDER_TARGET
                    // and the "out" state is PIXEL_SHADER_RESOURCE so
                    // the runtime can sample it when compositing.
                    if (FAILED(m_d3d11On12->CreateWrappedResource(
                            raw[i].texture,
                            &flags,
                            D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                            __uuidof(ID3D11Resource),
                            reinterpret_cast<void**>(
                                m_wrappedResources[i].GetAddressOf())))) {
                        Log("xr_telemetry: overlay disabled — CreateWrappedResource "
                            "failed for D3D12 image " + std::to_string(i) + "\n");
                        return false;
                    }
                    if (i == 0) {
                        // Diagnostic on image 0 like the D3D11 path —
                        // the wrapped resource's apparent format is
                        // useful when the D3D12 swapchain ends up
                        // typeless.
                        ComPtr<ID3D11Texture2D> tex2d;
                        if (SUCCEEDED(m_wrappedResources[i].As(&tex2d))) {
                            D3D11_TEXTURE2D_DESC d{};
                            tex2d->GetDesc(&d);
                            Log(fmt::format(
                                "xr_telemetry: overlay D3D12 wrapped image[0] "
                                "format={}, bindFlags={:#x}, usage={}, sampleCount={}\n",
                                static_cast<int>(d.Format),
                                static_cast<unsigned int>(d.BindFlags),
                                static_cast<int>(d.Usage),
                                static_cast<int>(d.SampleDesc.Count)));
                        }
                    }
                }

                // One RTV per wrapped swapchain image — we paint directly
                // into the acquired image each frame (no shim, no per-frame
                // CopyResource). The wrapped resources were created with
                // BIND_RENDER_TARGET above, so they're valid RTV targets.
                // Use an EXPLICIT typed view; rtvFormatForSwapchain maps an
                // sRGB swapchain to its UNORM sibling so a typeless wrapped
                // resource resolves to a non-re-encoding view — mirrors D3D11,
                // including the typed-sRGB fallback in createOverlayImageRtv.
                const DXGI_FORMAT rtvFormat = rtvFormatForSwapchain(format);
                const DXGI_FORMAT rtvFallback = static_cast<DXGI_FORMAT>(format);
                m_imageRtvs.resize(imgCount);
                for (uint32_t i = 0; i < imgCount; ++i) {
                    ComPtr<ID3D11Texture2D> tex2d;
                    if (FAILED(m_wrappedResources[i].As(&tex2d))) {
                        Log("xr_telemetry: overlay disabled — D3D12 path "
                            "wrapped-resource QI ID3D11Texture2D failed for "
                            "image " + std::to_string(i) + "\n");
                        return false;
                    }
                    if (!createOverlayImageRtv(m_d3d11Device.Get(), tex2d.Get(),
                                               rtvFormat, rtvFallback,
                                               m_imageRtvs[i].GetAddressOf())) {
                        Log("xr_telemetry: overlay disabled — D3D12 path "
                            "CreateRenderTargetView (wrapped image "
                            + std::to_string(i) + ") failed\n");
                        return false;
                    }
                }

                // GPU pipelines on the D3D11On12 bridge — same renderers as the
                // D3D11 path; shared init so the two bring-ups can't drift. RTV
                // is passed per draw call.
                if (!initOverlayRenderers(m_d3d11Device.Get(),
                                           m_d3d11Context.Get(),
                                           m_core, m_bars, m_glyphRenderer,
                                           m_chromeShapeRenderer))
                    return false;

                // One-time fill of the immutable quad-layer fields.
                // Byte-identical to the D3D11 path's helper — a shared base
                // could host this (m_swapchain + m_quadLayer are both
                // members); tracked as the D3D11/D3D12 dedup follow-up.
                initQuadLayerConstants();

                Log("xr_telemetry: overlay D3D12 renderer ready ("
                    + std::to_string(imgCount) +
                    " swapchain images, D3D11On12 bridge, swapchain format="
                    + std::to_string(format)
                    + (isSrgbSwapchainFormat(format) ? " (sRGB)" : "")
                    + ", direct-to-image RT format="
                    + std::to_string(static_cast<int>(rtvFormat)) + ")\n");
                return true;
            }

            // Shader-path paint is the free function paintOverlay()
            // declared above the class bodies — same body served both
            // D3D11OverlayRenderer and D3D12OverlayRenderer, so the
            // duplicate per-class definitions are gone.

            void initQuadLayerConstants() {
                m_quadLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
                m_quadLayer.next = nullptr;
                m_quadLayer.layerFlags =
                    XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                m_quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                m_quadLayer.subImage.swapchain = m_swapchain;
                m_quadLayer.subImage.imageRect.offset = {0, 0};
                m_quadLayer.subImage.imageRect.extent = {kTexWPhys, kTexHPhys};
                m_quadLayer.subImage.imageArrayIndex = 0;
                m_quadLayer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            }

            OpenXrApi*                            m_api = nullptr;
            XrSession                             m_session = XR_NULL_HANDLE;
            ComPtr<ID3D12Device>                  m_d3d12Device;
            ComPtr<ID3D12CommandQueue>            m_d3d12Queue;
            ComPtr<ID3D11Device>                  m_d3d11Device;
            ComPtr<ID3D11DeviceContext>           m_d3d11Context;
            xrprof::Probe*                        m_probe = nullptr;  // not owned
            ComPtr<ID3D11On12Device>              m_d3d11On12;
            XrSwapchain                           m_swapchain = XR_NULL_HANDLE;
            std::vector<ComPtr<ID3D11Resource>>   m_wrappedResources;
            // One D3D11 RTV per wrapped D3D12 swapchain image. The GPU
            // pipelines paint DIRECTLY into the acquired image's RTV each
            // frame — no private shim, no per-frame CopyResource (the same
            // direct-to-swapchain model as the D3D11 path), bracketed by
            // Acquire/ReleaseWrappedResources so D3D11 may write the D3D12
            // resource.
            std::vector<ComPtr<ID3D11RenderTargetView>> m_imageRtvs;
            // GPU pipelines on the D3D11On12 bridge — same classes as the
            // D3D11 path, targeting the per-image RTVs above. The D3D11On12
            // Flush() in renderAndCompose is incompressible (the D3D12
            // queue handoff), so the bridge overhead is the price of the
            // D3D12 path; the chrome/text/bars themselves are all GPU.
            // init() fails closed on any of them, so no enable flags.
            HistogramBarRenderer                  m_bars;
            glyph_atlas::Renderer                 m_glyphRenderer;
            chrome_shapes::Renderer               m_chromeShapeRenderer;
            // Chrome cadence — drawChrome rebuild only on snap.version
            // ticks or the watchdog. Same kChromeWatchdogFrames constant
            // as the D3D11 path, hoisted to namespace scope so a tweak
            // lands once.
            PaintCadence                          m_barsCadence;
            CoreRenderer                          m_core;
            HistogramRing<kRingSize>              m_cpuRing;
            HistogramRing<kRingSize>              m_gpuRing;
            XrCompositionLayerQuad                m_quadLayer{};
            // Geometry arrives per frame in renderAndCompose (caller-owned,
            // computed once from the immutable settings) — no cache here.
            bool                                  m_ready = false;
        };

    } // anonymous namespace

    // -------- Factory functions ----------------------------------------------

    std::unique_ptr<OverlayRenderer> makeD3D11OverlayRenderer(
        OpenXrApi* api, XrSession session, ID3D11Device* device,
        xrprof::Probe* probe) {
        auto r = std::make_unique<D3D11OverlayRenderer>(api, session, device, probe);
        return r->isReady() ? std::unique_ptr<OverlayRenderer>(std::move(r))
                            : nullptr;
    }

    std::unique_ptr<OverlayRenderer> makeD3D12OverlayRenderer(
        OpenXrApi* api, XrSession session,
        ID3D12Device* device, ID3D12CommandQueue* queue,
        xrprof::Probe* probe) {
        auto r = std::make_unique<D3D12OverlayRenderer>(api, session, device, queue, probe);
        return r->isReady() ? std::unique_ptr<OverlayRenderer>(std::move(r))
                            : nullptr;
    }

    // -------- GPU-path snapshot entry point -------------------------------
    //
    // Mirrors the in-headset D3D11 paint: build the atlas, init the three
    // GPU renderers against `target`, and run one paintOverlay pass.
    // Caller reads `target` back afterwards. See the header for the
    // contract (BGRA8_UNORM target, BGRA-capable device).
    bool renderOverlayToTextureD3D11(
        ID3D11Device* device,
        ID3D11DeviceContext* ctx,
        ID3D11Texture2D* target,
        const OverlaySnapshot& snap,
        const HistogramRing<kOverlayHistoRingSize>& cpuRing,
        const HistogramRing<kOverlayHistoRingSize>& gpuRing,
        std::string* errOut) {
        auto fail = [&](const char* msg) {
            if (errOut) *errOut = msg;
            return false;
        };
        if (!device || !ctx || !target) return fail("null device/ctx/target");

        // Render at the PRODUCTION supersample factor so the snapshot mirrors
        // what ships: the atlas bakes at the physical glyph sizes, the text PS
        // applies its gamma + edge-contrast, the bars use physical edge
        // coverage, and the grid/budget lines take the AA path — none of which
        // are exercised at 1x (they're all gated on supersample > 1). The
        // caller MUST size `target` via overlaySnapshotTargetSize() (= kTexWPhys
        // x kTexHPhys) to match the physical viewport these renderers use.
        CoreRenderer core;
        if (!core.init(kEffectiveSupersample)) return fail("core.init() failed (DWrite / atlas)");
        if (!core.atlasReady()) return fail("glyph atlas not built");

        HistogramBarRenderer    bars;
        glyph_atlas::Renderer   text;
        chrome_shapes::Renderer shapes;
        if (!bars.init(device, ctx, kEffectiveSupersample)) return fail("bars init failed");
        if (!text.init(device, ctx, static_cast<UINT>(kTexW),
                        static_cast<UINT>(kTexH), core.atlas(),
                        static_cast<UINT>(kTexWPhys),
                        static_cast<UINT>(kTexHPhys)))
            return fail("glyph renderer init failed");
        if (!shapes.init(device, ctx, static_cast<UINT>(kTexW),
                          static_cast<UINT>(kTexH),
                          static_cast<UINT>(kTexWPhys),
                          static_cast<UINT>(kTexHPhys)))
            return fail("chrome shapes init failed");

        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(device->CreateRenderTargetView(
                target, nullptr, rtv.GetAddressOf())))
            return fail("CreateRenderTargetView (target) failed");

        // Fresh cadence → first paintOverlay does a full static rebuild
        // (needStatic is true on the first frame, so chrome flushes).
        PaintCadence cadence;
        if (!paintOverlay(core, bars, cadence, ctx, rtv.Get(),
                           text, shapes, cpuRing, gpuRing, snap))
            return fail("paintOverlay failed");
        ctx->Flush();
        return true;
    }

    void overlaySnapshotTargetSize(UINT& width, UINT& height) {
        // Physical (supersampled) pixel size renderOverlayToTextureD3D11
        // renders at — the production dimensions. Exposed so the snapshot
        // test sizes its target from here and tracks kOverlaySupersample
        // automatically (no hardcoded dims to drift if the factor changes).
        width  = static_cast<UINT>(kTexWPhys);
        height = static_cast<UINT>(kTexHPhys);
    }

} // namespace openxr_api_layer::detail
