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

#include "histogram_ring.h"
#include "overlay_cadence.h"
#include "overlay_layout.h"

#include "framework/dispatch.gen.h"   // OpenXrApi (auto-generated at build)
#include "framework/log.h"            // Log / ErrorLog

#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_3.h>     // IDWriteFactory5 / IDWriteInMemoryFontFileLoader (Win10 1703+)
// Extra DXGI / D3D11 headers required by the cross-device shim
// path. pch.h's <d3d11.h> + <dxgi.h> only expose the base
// interfaces; we need:
//   - IDXGIResource1 (CreateSharedHandle)            from <dxgi1_2.h>
//   - ID3D11Device1::OpenSharedResource1             from <d3d11_1.h>
//   - IDXGIKeyedMutex                                from <dxgi.h>  ✓ already there
#include <dxgi1_2.h>
#include <d3d11_1.h>

#include <array>
#include <cmath>
#include <cstring>     // std::memcpy for constant-buffer uploads
#include <cwchar>      // std::wcslen for the wide-literal degree-symbol path
#include <iterator>    // std::size for the dash-style array
#include <string>
#include <vector>

// Offline-compiled shader bytecode (FxCompile → $(IntDir), on the C++
// include path in both the layer and test vcxproj). Each header declares a
// `const BYTE g_<filename>[]` (internal linkage, so including them in this
// one TU is ODR-safe). HistogramBarRenderer builds the instanced-bar and
// solid-quad pipelines from these to paint the histogram region on the GPU
// instead of via the per-bar D2D FillRectangle loop.
#include "overlay_bars_vs.h"
#include "overlay_bars_ps.h"
#include "overlay_quad_vs.h"
#include "overlay_quad_ps.h"

// Pragma-link D2D + DirectWrite so we don't have to add them to the
// vcxproj's Link section. d2d1.lib and dwrite.lib ship with the Windows
// SDK; no extra NuGet package needed.
#pragma comment(lib, "d2d1.lib")
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

    namespace {

        // -------- Layout constants (fpsVR redesign) -------------------------
        //
        // Texture stays at this fixed size regardless of `scale` — the
        // QUAD in 3D scales, not the resolution. kTexW × kTexH packs
        // the four sections of the redesigned HUD:
        //   - header bar (FPS / FPS AVG / P95 / P99 / P99.9, 5 cells)
        //   - GPU FRAMETIME MS panel with histogram + current value
        //   - CPU FRAMETIME MS panel with histogram + App / Render split
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
        //   kSectionGap          (8)
        //   kFrametimeHeight    (90) — GPU panel
        //   kSectionGap          (8)
        //   kFrametimeHeight    (90) — CPU panel
        //   kSectionGap          (8)
        //   kBottomHeight      (109)
        //   inner padding        (4)
        //   kFrameStroke         (2)
        //   kOuterPad           (10)
        //   Total = 435 = kTexH, leaving zero slack between the last
        //   panel and the inner bottom edge. Keeping the budget exact
        //   makes the top and bottom borders read at the same
        //   thickness in the HMD (4 px inner pad + 2 px stroke = 6 px
        //   on each side). `bottomY + kBottomHeight == kInnerB` by
        //   construction (asserted via the (void)kInnerB line in
        //   paint()). Bumping any of the heights past this budget
        //   will visually clip the bottom panel.
        constexpr int32_t kTexW = 720;
        constexpr int32_t kTexH = 435;

        constexpr float kOuterPad       = 10.0f;
        constexpr float kFrameStroke    = 2.0f;
        constexpr float kSectionGap     =  8.0f;
        constexpr float kSectionInnerPad = 12.0f;  // padding INSIDE each panel

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
        constexpr float kFrametimeHeight  = 90.0f;   // strip height ≈ 54 px after the
                                                      // title row. Sized so that a normal
                                                      // frame at ~50 % budget fills most
                                                      // of the strip — eliminates the
                                                      // empty top-half users observed
                                                      // in light-load scenarios.
        constexpr float kBottomHeight     = 109.0f;  // 4 + label(22) + ~7 gap +
                                                      // metric(43) cell-centred +
                                                      // ~33 bottom margin. Sized
                                                      // so the centred digit lands
                                                      // with equal top/bottom space
                                                      // (33 px each) and the label
                                                      // sits in the upper region.

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
        // 133 bars fill the GPU histogram strip exactly at the fixed
        // 4-px-bar / 1-px-gap layout (133×4 + 132×1 = 664 px = strip
        // inner width) — flush, zero margin, every bar pixel-aligned.
        // Window covered ≈ 1.5 s @ 90 Hz / 0.9 s @ 144 Hz, still short
        // enough that the histogram reacts within ~a second. The D2D
        // fallback path (D3D12 + snapshot) recomputes its bar width from
        // this count, so it stays consistent.
        constexpr std::size_t kRingSize  = openxr_api_layer::detail::kOverlayHistoRingSize;
        static_assert(kRingSize == 133,
                       "kOverlayHistoRingSize must match the in-engine ring size; "
                       "bump both in lockstep if tuning the histogram length");

        // Font sizes — calibrated against the design spec's
        // hierarchy. Sizes in pixels at the native texture resolution;
        // rendered to the head-locked quad they read at their natural
        // visual weight in the HMD.
        constexpr float kFontTinyLabel    = 17.0f;  // "FPS", "P95", "TEMP", "VRAM"
        constexpr float kFontSectionTitle = 18.0f;  // "GPU FRAMETIME MS"
        // Labels and section titles render in Rajdhani SemiBold.
        constexpr float kFontMs           = 18.0f;  // GPU panel "6.7 ms" current value
        constexpr float kFontMsCompound   = 18.0f;  // CPU panel compound string
                                                     // ("App X ms / Render Y ms") —
                                                     // matches kFontMs so the GPU
                                                     // and CPU read-outs share the
                                                     // same visual weight.
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

        // Vertical insets for the 1-px column separator lines.
        // Header bar uses a tighter inset (the cells are 90 px tall);
        // bottom panel inset is larger because the panel is 130 px
        // and the design's airier separator visual asks for more
        // breathing room above and below.
        constexpr float kHeaderSepInsetY      =  8.0f;
        constexpr float kBottomSepInsetY      = 14.0f;

        // Target DXGI format for the swapchain image — also the format
        // the D2D RenderTarget paints into.
        constexpr int64_t kFormatBGRA = static_cast<int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM);

        // Pick a swapchain format the runtime advertises that we can paint
        // into. Returns kFormatBGRA on success, 0 on failure (caller logs
        // and degrades). We don't try to fall back to RGBA8 — D2D's BGRA
        // pipeline is the simple path, and modern runtimes (Pimax, SteamVR,
        // WMR, Oculus, Varjo) all advertise BGRA8.
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
            for (const int64_t f : formats) {
                if (f == kFormatBGRA) return kFormatBGRA;
            }
            return 0;
        }

        // -------- CoreRenderer: D2D + DirectWrite painting ------------------
        //
        // Shared between the D3D11-native path and the D3D12-via-D3D11On12
        // bridge. Owns the D2D / DirectWrite factories + the cached
        // IDWriteTextFormat. The per-image render targets live on the
        // outer class because they bind to specific swapchain images.
        class CoreRenderer {
          public:
            // -------- Public lifecycle --------------------------------------
            //
            // Destructor unregisters the bundled-font loader before the
            // ComPtr drops the last reference, so the shared
            // IDWriteFactory's internal loader table never holds a
            // dangling pointer across sessions. Order of destruction:
            // m_customFontCollection → m_bundledFontFiles → loader
            // (here, explicit unregister) → m_dwriteFactory → m_d2dFactory.
            ~CoreRenderer() {
                if (m_inMemoryFontLoader && m_dwriteFactory) {
                    m_dwriteFactory->UnregisterFontFileLoader(
                        m_inMemoryFontLoader.Get());
                }
            }

            bool init() {
                if (FAILED(::D2D1CreateFactory(
                        D2D1_FACTORY_TYPE_SINGLE_THREADED,
                        m_d2dFactory.GetAddressOf()))) {
                    return false;
                }
                if (FAILED(::DWriteCreateFactory(
                        DWRITE_FACTORY_TYPE_SHARED,
                        __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
                    return false;
                }

                // Load the BUNDLED Barlow font (Medium + Medium Italic,
                // subset to digits / ASCII letters / °/µ/× / punctuation).
                // The TTF files live as RT_BUNDLED_FONT resources in the
                // DLL (see fonts/bundled_fonts.rc.inc); we feed their
                // bytes into an IDWriteInMemoryFontFileLoader and build
                // a custom IDWriteFontCollection around them. The
                // collection is then referenced by family name ("Barlow")
                // in every CreateTextFormat call below — DirectWrite
                // resolves through the custom collection, never falling
                // back to system fonts.
                //
                // Why bundle two families: design lands on a true italic
                // for the chiffres (Barlow Medium Italic) but keeps the
                // labels and section titles in the original technical-
                // grotesque feel (Rajdhani SemiBold upright). Synthesising
                // italic via DWRITE_FONT_STYLE_OBLIQUE on a non-italic
                // face produces a sheared geometric look that's
                // noticeably uglier than a real italic cut, so we ship
                // Barlow-MediumItalic.ttf for the italic chiffres.
                // Rajdhani is brought back specifically because its
                // narrower upright glyphs read crisper for short caps
                // labels ("FPS", "GPU TEMP", "VRAM") than Barlow's
                // wider Medium would.
                //
                // On failure (resource missing, factory QI fails,
                // collection creation fails), we proceed without the
                // custom collection and let CreateTextFormat fall
                // back to system default — Bahnschrift on Win10+. The
                // fallback face is upright-only, so chiffres formats
                // will render upright instead of italic. Graceful
                // degrade — never crash the host process for a
                // cosmetic font.
                const wchar_t* kFamilyChiffres = L"Barlow";   // italic chiffres
                const wchar_t* kFamilyLabels   = L"Rajdhani"; // upright labels + titles
                IDWriteFontCollection* customCollection = nullptr;
                if (!loadBundledFontCollection(m_dwriteFactory.Get(),
                                                 m_customFontCollection)) {
                    // Fallback: leave m_customFontCollection null and
                    // let DirectWrite use system fonts. Bahnschrift
                    // is the next-best match for both roles — italic
                    // chiffres will be synthesised oblique on the
                    // fallback face since system Bahnschrift has no
                    // true italic face.
                    kFamilyChiffres = L"Bahnschrift";
                    kFamilyLabels   = L"Bahnschrift";
                } else {
                    customCollection = m_customFontCollection.Get();
                }

                // Format split — Barlow Medium Italic is reserved for
                // digit characters only; everything else (labels,
                // titles, unit suffixes like " ms" / "°C" / "%") uses
                // Rajdhani SemiBold upright. The two pure-digit formats
                // (m_fmtBigNumber / m_fmtAccentNumber) stay anchored
                // directly on Barlow Italic because their strings are
                // always digit-only (FPS / AVG / P95 / …). The mixed
                // formats (m_fmtTemp, m_fmtMsValue, m_fmtMsCompound)
                // anchor on Rajdhani upright and get per-range Barlow
                // Italic overrides at draw time via drawValueAscii /
                // drawValueWide — those helpers walk the value runs
                // produced by findValueRuns and apply SetFontFamilyName
                // + SetFontWeight + SetFontStyle on the digit portion.
                //
                //   Pure-digit (Barlow Medium Italic):
                //     m_fmtBigNumber, m_fmtAccentNumber
                //   Mixed value (Rajdhani SemiBold upright BASE +
                //   per-digit Barlow Italic overrides):
                //     m_fmtTemp, m_fmtMsValue, m_fmtMsCompound
                //   Labels / titles (Rajdhani SemiBold upright):
                //     m_fmtTinyLabelCenter, m_fmtSectionTitle
                constexpr DWRITE_FONT_WEIGHT kChiffresWeight = DWRITE_FONT_WEIGHT_MEDIUM;
                constexpr DWRITE_FONT_WEIGHT kLabelWeight    = DWRITE_FONT_WEIGHT_SEMI_BOLD;
                if (!makeFormat(kFamilyChiffres, customCollection, kFontBigNumber,    kChiffresWeight, DWRITE_FONT_STYLE_ITALIC,
                                 DWRITE_TEXT_ALIGNMENT_CENTER, m_fmtBigNumber)) return false;
                if (!makeFormat(kFamilyChiffres, customCollection, kFontAccentNumber, kChiffresWeight, DWRITE_FONT_STYLE_ITALIC,
                                 DWRITE_TEXT_ALIGNMENT_CENTER, m_fmtAccentNumber)) return false;
                // m_fmtTemp is CENTER-aligned: the bottom panel
                // stacks each value (TEMP / LOAD / VRAM) centred
                // under its column label. Anchored on Rajdhani
                // upright so the " °C" / " %" / " GB" unit suffixes
                // render upright; digit prefixes get Barlow Italic
                // via drawValueWide / drawValueAscii.
                if (!makeFormat(kFamilyLabels,   customCollection, kFontTemp,         kLabelWeight,    DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_TEXT_ALIGNMENT_CENTER, m_fmtTemp)) return false;
                // m_fmtMsValue anchors on Rajdhani upright too:
                // the " ms" suffix renders upright and the digit
                // prefix flips to Barlow Italic via drawValueAscii.
                if (!makeFormat(kFamilyLabels,   customCollection, kFontMs,           kLabelWeight,    DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_TEXT_ALIGNMENT_TRAILING, m_fmtMsValue)) return false;
                // m_fmtMsCompound's BASE is Rajdhani SemiBold upright
                // (it's the "App" / " / Render " label portions plus
                // the " ms" unit suffixes). drawValueAscii applies
                // Barlow Medium Italic on the digit ranges per draw.
                if (!makeFormat(kFamilyLabels,   customCollection, kFontMsCompound,   kLabelWeight,    DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_TEXT_ALIGNMENT_TRAILING, m_fmtMsCompound)) return false;
                if (!makeFormat(kFamilyLabels,   customCollection, kFontTinyLabel,    kLabelWeight,    DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_TEXT_ALIGNMENT_CENTER, m_fmtTinyLabelCenter)) return false;
                if (!makeFormat(kFamilyLabels,   customCollection, kFontSectionTitle, kLabelWeight,    DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_TEXT_ALIGNMENT_LEADING, m_fmtSectionTitle)) return false;

                // TODO(barlow-followup): activate OpenType `tnum`
                // (tabular figures) on the chiffres formats so digit
                // widths stay uniform — without it, FPS bouncing
                // between "138" and "142" makes the rendered string
                // visibly shift horizontally at 10 Hz refresh. The
                // typography opt-in lives on IDWriteTextLayout, not
                // on IDWriteTextFormat, so it requires refactoring
                // drawAscii/drawWide from ID2D1RenderTarget::DrawTextW
                // to IDWriteTextLayout + DrawTextLayout. Not done in
                // this PR (font swap is already a chunk). Tracked as
                // a follow-up because both Rajdhani (previous) and
                // Barlow (this) have proportional figures by default
                // — switching fonts doesn't introduce regression on
                // this axis, both versions of the HUD jitter equally.
                return true;
            }

            ID2D1Factory* d2d() const noexcept { return m_d2dFactory.Get(); }

            // -------- Brushes (palette) -------------------------------------
            //
            // Brushes are bound to the render target they were created
            // against — call once after the owning renderer creates its
            // D2D ID2D1RenderTarget, not per frame. The redesign uses
            // a ~12-brush palette (background fills, panel fills,
            // text colours, gauge colours, bar colours, icon strokes);
            // creating them all once amortises ~5 µs of D2D allocation
            // away from the frame thread.
            bool initBrushes(ID2D1RenderTarget* rt) {
                if (!rt) return false;
                // Adopt the RT's owning factory unconditionally. D2D
                // resources (path geometries, stroke styles) are
                // factory-bound — using one created from factory A
                // against an RT from factory B causes a deferred
                // error that surfaces at EndDraw time as a silent
                // failure (no doctest assert hit, just `false`
                // returned from paint()).
                //
                // Production path (D3D11/D3D12 renderers): rt was
                // created via m_core.d2d()->CreateDxgiSurfaceRender
                // Target(...), so rt->GetFactory == m_d2dFactory
                // already; this is a refcount-only no-op.
                //
                // Snapshot path (renderOverlayToTarget): the test
                // owns its own ID2D1Factory and creates a WIC bitmap
                // RT from it. The factory init() created is now
                // dropped (no D2D resources reference it yet — text
                // formats only reference m_dwriteFactory), and the
                // geometries later created in paint() use rt's
                // factory. EndDraw then succeeds.
                rt->GetFactory(m_d2dFactory.ReleaseAndGetAddressOf());
                auto make = [&](D2D1::ColorF c, ComPtr<ID2D1SolidColorBrush>& out) {
                    return SUCCEEDED(rt->CreateSolidColorBrush(c, out.GetAddressOf()));
                };
                // Palette tuned to a "futuristic gaming HUD / racing
                // telemetry" aesthetic — deep carbon-black background,
                // graphite panels, soft off-white text, saturated
                // cyan and electric-blue accents. Hex values come
                // from the design spec; alpha 0.94 on the outer bg
                // composites the HUD over the game with a subtle
                // window into the world behind it.
                // Refined palette from the design spec — slightly
                // lighter background, deeper panel fill, more
                // muted cyan for text accents (vs. saturated
                // cyan-bright reserved for the bar gradient top).
                if (!make(D2D1::ColorF(0.020f, 0.024f, 0.024f, 0.94f), m_brushBg)) return false;          // #050606
                if (!make(D2D1::ColorF(0.035f, 0.039f, 0.039f, 1.00f), m_brushPanelBg)) return false;    // #090A0A
                if (!make(D2D1::ColorF(0.122f, 0.133f, 0.133f, 1.00f), m_brushFrameLine)) return false;  // #1F2222 darker frame
                if (!make(D2D1::ColorF(0.184f, 0.200f, 0.204f, 1.00f), m_brushSeparator)) return false;  // #2F3334
                // Text — neutral off-white, #F7F7F7. The HUD's single
                // text brush: covers header micro-labels (FPS / P95 /
                // P99 / …), footer captions (GPU TEMP / LOAD / VRAM),
                // histogram titles (GPU FRAMETIME MS / CPU FRAMETIME
                // MS), the FPS big number, and the upright "App" /
                // " / Render " labels in the CPU compound. Earlier
                // iterations split into a brighter "text" brush and a
                // dimmer "label" brush, but the design lands on the
                // same off-white for every text role.
                if (!make(D2D1::ColorF(0.969f, 0.969f, 0.969f, 1.00f), m_brushTextWhite)) return false;  // #F7F7F7
                // Cyan accent — the "duller" cyan from the spec
                // (#19D1D9), reserved for TEXT accents (FPS AVG /
                // P95 / P99 / P99.9 + healthy LOAD / VRAM). The
                // brighter #28E0E5 lives in the bar gradient TOP
                // (see m_gpuTealStops below) — different role,
                // different colour.
                if (!make(D2D1::ColorF(0.098f, 0.820f, 0.851f, 1.00f), m_brushAccentCyan)) return false; // #19D1D9
                // Histogram bar GRADIENT brushes — top→bottom
                // linear gradient inside each bar gives the bars
                // the "lit from above" look from the design spec.
                // The brushes are created here with placeholder
                // endpoints (0,0)→(0,1); drawHistogramBars sets the
                // actual strip-top / strip-bottom per frame via
                // SetStartPoint / SetEndPoint. That avoids
                // re-creating the brush every frame while still
                // letting it sample the gradient across whichever
                // strip is being painted.
                D2D1_GRADIENT_STOP gpuStops[2] = {
                    {0.0f, D2D1::ColorF(0.157f, 0.878f, 0.898f, 1.00f)},  // #28E0E5 (top, lighter)
                    {1.0f, D2D1::ColorF(0.075f, 0.682f, 0.710f, 1.00f)},  // #13AEB5 (bottom, darker)
                };
                D2D1_GRADIENT_STOP cpuStops[2] = {
                    {0.0f, D2D1::ColorF(0.157f, 0.686f, 1.000f, 1.00f)},  // #28AFFF (top, lighter)
                    {1.0f, D2D1::ColorF(0.047f, 0.561f, 0.847f, 1.00f)},  // #0C8FD8 (bottom, darker)
                };
                if (FAILED(rt->CreateGradientStopCollection(
                        gpuStops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                        m_gpuTealStops.GetAddressOf()))) return false;
                if (FAILED(rt->CreateGradientStopCollection(
                        cpuStops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                        m_cpuBlueStops.GetAddressOf()))) return false;
                const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES gradProps = {
                    /* startPoint */ {0.0f, 0.0f},
                    /* endPoint   */ {0.0f, 1.0f},
                };
                if (FAILED(rt->CreateLinearGradientBrush(
                        gradProps, m_gpuTealStops.Get(),
                        m_brushGpuTealGrad.GetAddressOf()))) return false;
                if (FAILED(rt->CreateLinearGradientBrush(
                        gradProps, m_cpuBlueStops.Get(),
                        m_brushCpuBlueGrad.GetAddressOf()))) return false;
                // Warning / critical tiers — kept from the previous
                // palette. Apply to both histogram bars (per-sample)
                // and LOAD / VRAM text (overall).
                if (!make(D2D1::ColorF(1.000f, 0.553f, 0.000f, 1.00f), m_brushOrange)) return false;     // #FF8D00
                if (!make(D2D1::ColorF(1.000f, 0.196f, 0.235f, 1.00f), m_brushGaugeRed)) return false;   // #FF323C
                // Dashed grid + budget line — desaturated and low-
                // alpha so they sit BEHIND the bars without competing
                // for attention.
                if (!make(D2D1::ColorF(0.353f, 0.431f, 0.451f, 0.30f), m_brushGridDash)) return false;   // #5A6E73 @ 30%
                if (!make(D2D1::ColorF(0.949f, 0.957f, 0.961f, 0.45f), m_brushBudgetLine)) return false; // off-white @ 45%

                // Stroke style for the dashed grid lines. ID2D1Strok
                // Style is shareable; we cache the one we need.
                D2D1_STROKE_STYLE_PROPERTIES dashProps =
                    D2D1::StrokeStyleProperties(
                        D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                        D2D1_LINE_JOIN_MITER, 10.0f,
                        D2D1_DASH_STYLE_CUSTOM, 0.0f);
                const FLOAT dashes[] = {2.0f, 4.0f};
                if (FAILED(m_d2dFactory->CreateStrokeStyle(
                        dashProps, dashes,
                        static_cast<UINT32>(std::size(dashes)),
                        m_strokeDashed.GetAddressOf()))) return false;
                return true;
            }

            // -------- paint() -----------------------------------------------
            //
            // Layout (top → bottom):
            //   - Outer frame: dark grey 2-px stroke around a deep-
            //     carbon background, chamfered corners (12-px diagonal
            //     cuts), 10-px padding to the texture edge so the
            //     OpenXR runtime's bilinear filter doesn't soften the
            //     corner anti-alias.
            //   - Header bar: 5 cells (FPS / FPS AVG / P95 / P99 /
            //     P99.9) with thin vertical separators. Big white FPS
            //     number, cyan accent numbers on the right four cells.
            //   - GPU FRAMETIME MS panel: title + "X.X ms" value
            //     (top-right) + dashed grid + teal-gradient histogram.
            //   - CPU FRAMETIME MS panel: title + compound
            //     "App X ms / Render Y ms" (top-right) + blue-gradient
            //     histogram.
            //   - Bottom row (60 / 40 split between two panels):
            //       GPU panel = TEMP / LOAD / VRAM (3 cells). LOAD &
            //         VRAM are tier-coloured (cyan / orange / red
            //         per gaugeTierForUtilisation).
            //       CPU panel = TEMP / LOAD (2 cells). TEMP shows
            //         "-- °C" until PawnIO support lands.
            //
            // Two-tier paint:
            //   - Static tier (~10 Hz): outer frame, header, panel
            //     chrome, bottom row. Triggered by `snap.version`
            //     ticking, with kMaxFramesBetweenStaticPaints as a
            //     defensive timeout in case the aggregator stalls.
            //   - Dynamic tier (every frame): the two histogram
            //     regions. The ring buffer is fed by pushFrameSample()
            //     at the host frame rate, so refreshing the bars at
            //     90-144 Hz is what makes the scroll look smooth.
            //
            // The shim texture is persistent between paint() calls.
            // On dynamic-only frames we skip rt->Clear() so the
            // static chrome from the last static paint stays in
            // place — drawHistoRegion() opaquely overwrites the
            // histogram rectangles, leaving the surrounding chrome
            // untouched.
            bool paint(ID2D1RenderTarget* rt,
                       const OverlaySnapshot& snap,
                       const HistogramRing<kRingSize>& cpuRing,
                       const HistogramRing<kRingSize>& gpuRing) {
                if (!rt) return false;
                if (!m_brushBg || !m_strokeDashed) return false;  // brushes not init'd

                const bool needStatic = needStaticPaint(
                    m_cadence, snap.version, kMaxFramesBetweenPaints);

                rt->BeginDraw();

                // Panel Ys for the dynamic histo tier below. The chrome
                // (incl. the bottom row) is drawn by drawChrome(), which
                // recomputes its own Ys from the same constants — they
                // can't drift since both derive from kInner{T} + the
                // section heights.
                const float headerY    = kInnerT;
                const float gpuPanelY  = headerY + kHeaderHeight + kSectionGap;
                const float cpuPanelY  = gpuPanelY + kFrametimeHeight + kSectionGap;

                if (needStatic) {
                    // Fully transparent clear — we paint our own
                    // opaque panel backgrounds, so the corners
                    // outside the frame composite through to the
                    // game underneath as transparent pixels.
                    rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

                    m_cachedValues = formatOverlayDisplayValues(snap);
                    drawChrome(rt, m_cachedValues);
                }

                // Dynamic tier — runs every frame so the histogram
                // scrolls smoothly. Cheap relative to the static
                // tier: ~2 FillRect + ~8 DrawLine + ~240 bar
                // FillRect, no DirectWrite, no path geometry.
                drawHistoRegion(rt, gpuPanelY, gpuRing, snap.target_fps,
                                 m_brushGpuTealGrad.Get());
                drawHistoRegion(rt, cpuPanelY, cpuRing, snap.target_fps,
                                 m_brushCpuBlueGrad.Get());

                const bool ok = SUCCEEDED(rt->EndDraw());
                // Fold the result back into the cadence — see
                // commitPaint() in overlay_cadence.h for the
                // static-success / static-failure / dynamic rules
                // (the dynamic branch advances the watchdog even on
                // failure so a stuck dynamic tier can still recover).
                commitPaint(m_cadence, needStatic, ok, snap.version);
                return ok;
            }

            // Paint ONLY the static chrome (outer frame, header, panel
            // titles + current values, bottom row) into `rt` — no
            // histogram region, no cadence bookkeeping. Used by the
            // D3D11 path, which draws the histogram on the GPU
            // (HistogramBarRenderer) and drives its own cadence; the
            // caller decides when chrome is stale and calls this only
            // then. Wraps its own BeginDraw/EndDraw and clears first
            // (the shim is fully repainted on a chrome refresh).
            // Returns EndDraw success.
            bool paintChromeOnly(ID2D1RenderTarget* rt,
                                  const OverlaySnapshot& snap) {
                if (!rt) return false;
                if (!m_brushBg || !m_strokeDashed) return false;
                rt->BeginDraw();
                m_cachedValues = formatOverlayDisplayValues(snap);
                if (!m_chromeStaticPainted) {
                    // First chrome paint (or post-reset): full
                    // chrome — chamfered frame + panel BGs + labels +
                    // titles + separators + values. Latches the
                    // "static is in the shim" invariant for every
                    // subsequent call.
                    rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
                    drawChrome(rt, m_cachedValues);
                    m_chromeStaticPainted = true;
                } else {
                    // Subsequent chrome paints (every ~10 Hz snap.
                    // version tick): the static chrome is already in
                    // the shim — only the numeric values change. Skip
                    // the Clear and skip the full drawChrome; just
                    // erase each value rect to the panel BG and
                    // redraw the new value. Cuts ~half the chrome
                    // spike cost (the bulk was the DirectWrite labels
                    // + frame geometry, both unchanged frame-to-
                    // frame). See drawValuesIntoChrome().
                    drawValuesIntoChrome(rt, m_cachedValues);
                }
                return SUCCEEDED(rt->EndDraw());
            }

          private:
            // The static-tier drawing shared by paint() and
            // paintChromeOnly(): outer chamfered frame, header bar,
            // both frametime-panel chromes (background + title +
            // current value, NO histogram), and the bottom TEMP/LOAD/
            // VRAM row. Caller owns BeginDraw/Clear/EndDraw. Panel Ys
            // derive from the namespace-scope layout constants so they
            // can't drift from drawHistoRegion / the bar renderer.
            void drawChrome(ID2D1RenderTarget* rt,
                             const OverlayDisplayValues& v) {
                const float headerY   = kInnerT;
                const float gpuPanelY = headerY + kHeaderHeight + kSectionGap;
                const float cpuPanelY = gpuPanelY + kFrametimeHeight + kSectionGap;
                const float bottomY   = cpuPanelY + kFrametimeHeight + kSectionGap;

                // Outer frame: octagonal-ish chamfered shape (12-px
                // diagonal corner cuts), one FillGeometry/DrawGeometry
                // pair for fill + metal-line stroke.
                const D2D1_RECT_F frameOuter = D2D1::RectF(
                    kOuterPad, kOuterPad,
                    static_cast<float>(kTexW) - kOuterPad,
                    static_cast<float>(kTexH) - kOuterPad);
                drawChamferedRect(rt, frameOuter, 12.0f,
                                   m_brushBg.Get(),
                                   m_brushFrameLine.Get(), kFrameStroke);

                drawHeaderBar(rt, kInnerL, headerY, kInnerR,
                               headerY + kHeaderHeight, v);
                // Histogram content is owned by drawHistoRegion() (D2D
                // path) or HistogramBarRenderer (D3D11 path); these
                // panel calls only paint the chrome (bg + title +
                // current value).
                drawFrametimePanel(rt, kInnerL, gpuPanelY, kInnerR,
                                    gpuPanelY + kFrametimeHeight,
                                    L"GPU FRAMETIME MS",
                                    v.gpu_frametime_ms,
                                    /*secondaryValue=*/std::string{});
                drawFrametimePanel(rt, kInnerL, cpuPanelY, kInnerR,
                                    cpuPanelY + kFrametimeHeight,
                                    L"CPU FRAMETIME MS",
                                    v.cpu_frametime_ms,
                                    v.cpu_app_ms);

                // Bottom row: 5 equal cells (GPU TEMP/LOAD/VRAM +
                // CPU TEMP/LOAD), 60/40 split. Tier colours follow
                // gaugeTierForUtilisation.
                const float bottomCellW =
                    (kInnerR - kInnerL - kSectionGap) / 5.0f;
                const float gpuPanelL = kInnerL;
                const float gpuPanelR = gpuPanelL + 3.0f * bottomCellW;
                const float cpuPanelL = gpuPanelR + kSectionGap;
                const float cpuPanelR = kInnerR;
                drawBottomPanel(rt, gpuPanelL, bottomY,
                                 gpuPanelR, bottomY + kBottomHeight,
                                 L"GPU TEMP", L"GPU LOAD",
                                 v.gpu_temp_c, v.gpu_util_pct,
                                 v.gpu_util_fraction,
                                 /*vramValue=*/v.vram_pct,
                                 /*vramFraction=*/v.vram_fraction);
                drawBottomPanel(rt, cpuPanelL, bottomY,
                                 cpuPanelR, bottomY + kBottomHeight,
                                 L"CPU TEMP", L"CPU LOAD",
                                 v.cpu_temp_c, v.cpu_util_pct,
                                 v.cpu_util_fraction,
                                 /*vramValue=*/std::string{},
                                 /*vramFraction=*/0.0f);
            }

            // Invalidate-and-redraw the variable values only. Assumes
            // the surrounding chrome (frame, panel BGs, labels, titles,
            // separators) is already in the shim from a previous
            // drawChrome() — that's the contract paintChromeOnly()
            // enforces via m_chromeStaticPainted.
            //
            // For each value: FillRectangle(rect, panelBg) to opaquely
            // erase the previous value's pixels, then drawValue* to
            // render the new value. Erase rects are scoped to the
            // value region only — the label above the value, the title
            // on the panel's left, the separators and panel borders
            // stay untouched.
            //
            // Cell math mirrors drawHeaderBar / drawHeaderCell,
            // drawFrametimePanel, and drawBottomPanel exactly so the
            // values land in the same pixel positions they would after
            // a full drawChrome().
            void drawValuesIntoChrome(ID2D1RenderTarget* rt,
                                       const OverlayDisplayValues& v) {
                // Panel Ys mirror drawChrome / paint() exactly.
                const float headerY    = kInnerT;
                const float gpuPanelY  = headerY + kHeaderHeight + kSectionGap;
                const float cpuPanelY  = gpuPanelY + kFrametimeHeight + kSectionGap;
                const float bottomY    = cpuPanelY + kFrametimeHeight + kSectionGap;
                const float bottomB    = bottomY + kBottomHeight;

                // ----- Header bar: 5 cells, value rect under the label.
                {
                    const float cellW        = (kInnerR - kInnerL) / 5.0f;
                    const float labelY       = headerY + 4.0f;
                    const float labelH       = 22.0f;
                    const float valueY       = labelY + labelH;
                    const float valueBottom  = valueY + kFontBigNumber + 6.0f;
                    auto cell = [&](int col, const std::string& val,
                                      IDWriteTextFormat* fmt,
                                      ID2D1Brush* brush) {
                        const float cl = kInnerL + cellW * static_cast<float>(col);
                        const float cr = kInnerL + cellW * static_cast<float>(col + 1);
                        // Erase only the value region (label above
                        // valueY stays intact).
                        rt->FillRectangle(
                            D2D1::RectF(cl, valueY, cr, valueBottom),
                            m_brushPanelBg.Get());
                        // Re-draw the value — same rect math as
                        // drawHeaderCell.
                        const D2D1_RECT_F valueRect = D2D1::RectF(
                            cl, valueY - 2.0f, cr,
                            valueY + kFontBigNumber + 6.0f);
                        drawAscii(rt, val, fmt, valueRect, brush);
                    };
                    cell(0, v.fps_instant, m_fmtBigNumber.Get(),
                         m_brushTextWhite.Get());
                    cell(1, v.fps_avg,     m_fmtAccentNumber.Get(),
                         m_brushAccentCyan.Get());
                    cell(2, v.fps_p95,     m_fmtAccentNumber.Get(),
                         m_brushAccentCyan.Get());
                    cell(3, v.fps_p99,     m_fmtAccentNumber.Get(),
                         m_brushAccentCyan.Get());
                    cell(4, v.fps_p99_9,   m_fmtAccentNumber.Get(),
                         m_brushAccentCyan.Get());
                }

                // ----- Frametime panels: top-right "X.X ms" /
                // "App X / Render Y" value runs. Title on the left
                // stays untouched (different rect).
                auto frametimeValue = [&](float panelTop,
                                            const std::string& currentValue,
                                            const std::string& secondaryValue) {
                    const float titleT = panelTop + kPanelTitleTopPad;
                    const float titleB = titleT + kHistoTitleH;
                    if (secondaryValue.empty()) {
                        const D2D1_RECT_F rect = D2D1::RectF(
                            kInnerR - kSectionInnerPad - 160.0f, titleT,
                            kInnerR - kSectionInnerPad,          titleB);
                        rt->FillRectangle(rect, m_brushPanelBg.Get());
                        const std::string s = currentValue + " ms";
                        drawValueAscii(rt, s, m_fmtMsValue.Get(), rect,
                                        m_brushAccentCyan.Get());
                    } else {
                        const D2D1_RECT_F rect = D2D1::RectF(
                            kInnerR - kSectionInnerPad - 320.0f, titleT,
                            kInnerR - kSectionInnerPad,          titleB);
                        rt->FillRectangle(rect, m_brushPanelBg.Get());
                        const std::string compound =
                            "App " + secondaryValue + " ms / Render "
                            + currentValue + " ms";
                        drawValueAscii(rt, compound, m_fmtMsCompound.Get(),
                                        rect,
                                        m_brushTextWhite.Get(),
                                        m_brushAccentCyan.Get());
                    }
                };
                frametimeValue(gpuPanelY, v.gpu_frametime_ms, std::string{});
                frametimeValue(cpuPanelY, v.cpu_frametime_ms, v.cpu_app_ms);

                // ----- Bottom row: GPU (3 cells) + CPU (2 cells).
                // Value occupies the lower portion of each cell below
                // the 22-px label. Erase from labelBottom to cellB —
                // the label (and the panel's separator strokes drawn
                // earlier) stay intact.
                //
                // Refresh cadence: 2 Hz (every 5th values-only call)
                // instead of 10 Hz. Temperatures and util-% don't
                // change fast enough to merit a redraw on every
                // aggregator tick, and skipping their ~5 DirectWrite
                // calls on 4 of every 5 ticks knocks ~25-30% off the
                // typical chrome-paint spike. The values stay correct
                // because the aggregator's snapshot still updates
                // m_cachedValues at the usual 10 Hz — we just don't
                // repaint them every time.
                if (m_valueOnlyTick++ % 5 == 0)
                {
                    const float bottomCellW =
                        (kInnerR - kInnerL - kSectionGap) / 5.0f;
                    const float gpuPanelL = kInnerL;
                    const float gpuPanelR = gpuPanelL + 3.0f * bottomCellW;
                    const float cpuPanelL = gpuPanelR + kSectionGap;
                    const float cpuPanelR = kInnerR;
                    const float labelBottom = bottomY + 4.0f + 22.0f;

                    auto tierBrush = [&](float fraction) -> ID2D1Brush* {
                        const BarTier tier = gaugeTierForUtilisation(fraction);
                        if (tier == BarTier::Red)    return m_brushGaugeRed.Get();
                        if (tier == BarTier::Orange) return m_brushOrange.Get();
                        return m_brushAccentCyan.Get();
                    };

                    auto bottomCell = [&](float cellL, float cellR,
                                            const std::string& asciiValue,
                                            const wchar_t* unitSuffix,
                                            ID2D1Brush* valueBrush,
                                            bool useWideValue) {
                        // Erase below the label only.
                        rt->FillRectangle(
                            D2D1::RectF(cellL, labelBottom, cellR, bottomB),
                            m_brushPanelBg.Get());
                        // Re-draw paragraph-centred in the full cell
                        // rect — matches drawBottomPanel's drawCell.
                        const D2D1_RECT_F valueRect = D2D1::RectF(
                            cellL, bottomY, cellR, bottomB);
                        if (useWideValue) {
                            std::wstring wide(asciiValue.begin(),
                                               asciiValue.end());
                            wide += unitSuffix;
                            drawValueWide(rt, wide, m_fmtTemp.Get(),
                                           valueRect, valueBrush,
                                           /*chiffresBrush=*/nullptr,
                                           /*unitFontSize=*/kFontTempUnit);
                        } else {
                            // ASCII-safe suffix concat — same contract
                            // as drawBottomPanel's drawCell lambda.
                            std::string full = asciiValue;
                            for (const wchar_t* p = unitSuffix; *p; ++p) {
                                full.push_back(static_cast<char>(*p));
                            }
                            drawValueAscii(rt, full, m_fmtTemp.Get(),
                                            valueRect,
                                            valueBrush,
                                            /*chiffresBrush=*/nullptr,
                                            /*unitFontSize=*/kFontTempUnit);
                        }
                    };

                    // GPU panel (3 cells: TEMP / LOAD / VRAM).
                    const float gpuColW =
                        (gpuPanelR - gpuPanelL) / 3.0f;
                    // L" \u00B0C" — see drawBottomPanel's identical
                    // comment block on why we escape (MSVC without
                    // /utf-8 mishandles literal ° as CP1252).
                    bottomCell(gpuPanelL + gpuColW * 0.0f,
                                gpuPanelL + gpuColW * 1.0f,
                                v.gpu_temp_c, L" \u00B0C",
                                m_brushTextWhite.Get(),
                                /*useWideValue=*/true);
                    bottomCell(gpuPanelL + gpuColW * 1.0f,
                                gpuPanelL + gpuColW * 2.0f,
                                v.gpu_util_pct, L" %",
                                tierBrush(v.gpu_util_fraction),
                                /*useWideValue=*/false);
                    bottomCell(gpuPanelL + gpuColW * 2.0f,
                                gpuPanelR,
                                v.vram_pct, L" %",
                                tierBrush(v.vram_fraction),
                                /*useWideValue=*/false);

                    // CPU panel (2 cells: TEMP / LOAD).
                    const float cpuColW =
                        (cpuPanelR - cpuPanelL) / 2.0f;
                    bottomCell(cpuPanelL + cpuColW * 0.0f,
                                cpuPanelL + cpuColW * 1.0f,
                                v.cpu_temp_c, L" \u00B0C",
                                m_brushTextWhite.Get(),
                                /*useWideValue=*/true);
                    bottomCell(cpuPanelL + cpuColW * 1.0f,
                                cpuPanelR,
                                v.cpu_util_pct, L" %",
                                tierBrush(v.cpu_util_fraction),
                                /*useWideValue=*/false);
                }
            }

            // -------- end static-tier helpers --------

          private:
            // -------- Format / brush helpers --------------------------------
            //
            // makeFormat takes an optional IDWriteFontCollection — non-null
            // when the renderer loaded the bundled Barlow fonts, null when
            // we fell back to system fonts. CreateTextFormat resolves the
            // family name through the supplied collection (or the system
            // collection on null).
            //
            // `style` selects upright (`DWRITE_FONT_STYLE_NORMAL`) vs
            // true italic (`_ITALIC`). DirectWrite resolves the right
            // font FACE inside the collection — for Barlow that means
            // picking Barlow-Medium.ttf vs Barlow-MediumItalic.ttf
            // (both bundled, see fonts/bundled_fonts.rc.inc). If the
            // collection only has the upright face and ITALIC is
            // requested, DirectWrite falls back to synthetic-oblique
            // (sheared upright glyphs) — uglier but functional, and
            // happens only on the system-fallback path where we
            // couldn't load the bundle.
            bool makeFormat(const wchar_t* family,
                            IDWriteFontCollection* collection,
                            float size,
                            DWRITE_FONT_WEIGHT weight,
                            DWRITE_FONT_STYLE style,
                            DWRITE_TEXT_ALIGNMENT alignment,
                            ComPtr<IDWriteTextFormat>& out) {
                if (FAILED(m_dwriteFactory->CreateTextFormat(
                        family, collection, weight,
                        style,
                        DWRITE_FONT_STRETCH_NORMAL,
                        size, L"en-US", out.GetAddressOf()))) {
                    return false;
                }
                out->SetTextAlignment(alignment);
                out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                return true;
            }

            // Loads the bundled font resources (RT_BUNDLED_FONT, IDs
            // 200 + 201 in fonts/bundled_fonts.rc.inc — the shared
            // include compiled into both the layer DLL and the test
            // EXE so loadBundledFontCollection finds the same bytes
            // in either binary) into a custom IDWriteFontCollection.
            // The collection holds two families:
            //   ID 200 → Rajdhani SemiBold (labels + section titles)
            //   ID 201 → Barlow Medium Italic (chiffres)
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

                // Find both font resources in the DLL. The trick
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

                // 200 = SemiBold, 201 = Bold (matches the IDs in the
                // .rc.in template).
                if (!addResourceFont(200)) return false;
                if (!addResourceFont(201)) return false;

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
                    // the file (single face per file for our subsets
                    // — Rajdhani-SemiBold.ttf is one upright face in
                    // the Rajdhani family, Barlow-MediumItalic.ttf is
                    // one italic face in the Barlow family — but the
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

            // ASCII-only DrawTextW shortcut. The redesign emits only
            // ASCII digits + uppercase labels — except for the °C
            // suffix, which we draw via the dedicated wide-literal
            // path drawWide().
            void drawAscii(ID2D1RenderTarget* rt, const std::string& s,
                            IDWriteTextFormat* fmt,
                            const D2D1_RECT_F& rect,
                            ID2D1Brush* brush) const {
                if (s.empty()) return;
                std::wstring wide(s.begin(), s.end());
                rt->DrawTextW(wide.c_str(),
                              static_cast<UINT32>(wide.length()),
                              fmt, rect, brush);
            }

            void drawWide(ID2D1RenderTarget* rt, const wchar_t* s,
                           IDWriteTextFormat* fmt,
                           const D2D1_RECT_F& rect,
                           ID2D1Brush* brush) const {
                if (!s) return;
                const std::size_t len = std::wcslen(s);
                rt->DrawTextW(s, static_cast<UINT32>(len), fmt, rect, brush);
            }

            // A "value run" inside a rendered string: a value-shaped
            // prefix (e.g. "67", "4.3", "--", "--.-") followed by an
            // optional unit suffix (e.g. " ms", " °C", " %", " GB").
            // Detected by findValueRuns and consumed by drawValueWide /
            // drawValueAscii to apply per-range font / brush / size
            // overrides.
            //
            // Three independently-applied sub-ranges:
            //   italic[Start,Len]  digits-only sub-range; flips to Barlow
            //                      Medium Italic. Empty (italicLen == 0)
            //                      when the prefix is dash/dot-only —
            //                      e.g. the "--" / "--.-" placeholders.
            //   unit[Start,Len]    leading-space + unit chars; receives
            //                      the optional `unitFontSize` override.
            //                      Empty when the prefix has no unit
            //                      suffix (pure-digit "142" FPS string).
            //   brush[Start,Len]   whole value (prefix + unit); receives
            //                      the optional chiffres brush. This is
            //                      what lets the CPU compound paint "X.X
            //                      ms" entirely in cyan including the
            //                      upright " ms", while "App" / " /
            //                      Render " labels stay in the base
            //                      brush (white).
            struct ValueRun {
                UINT32 brushStart;
                UINT32 brushLen;
                UINT32 italicStart;
                UINT32 italicLen;
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
            // dots in label copy ("App", " / Render ") would falsely
            // pull into a value run. A unit-only span never happens
            // because we walk the prefix first.
            //
            // The italic sub-range is the smallest range covering all
            // digits inside the prefix (digits + interior dots, but
            // not surrounding dashes). For "-5.2" the italic range is
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
                    UINT32 firstDigit = 0;
                    UINT32 lastDigit  = 0;
                    bool hasDigit = false;
                    while (i < n && isValuePrefix(s[i])) {
                        if (isDigit(s[i])) {
                            if (!hasDigit) firstDigit = i;
                            lastDigit = i;
                            hasDigit = true;
                        }
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
                        run.italicStart = hasDigit ? firstDigit : prefixStart;
                        run.italicLen   = hasDigit ? (lastDigit - firstDigit + 1) : 0;
                        run.unitStart   = unitStart;
                        run.unitLen     = unitLen;
                        runs.push_back(run);
                    }
                }
                return runs;
            }

            // Render a value string with mixed per-range styling.
            //
            // Digit characters flip to Barlow Medium Italic; the rest
            // (including unit suffixes like " ms" / " °C" / " %") stays
            // in `baseFmt`'s family + style — typically Rajdhani
            // SemiBold upright. Auto-detects digit and unit ranges from
            // the string content via findValueRuns.
            //
            // `brush` paints non-value characters. When `chiffresBrush`
            // is non-null, BOTH the digit and unit portions of every
            // value run flip to that brush — used by the CPU compound
            // so "X.X ms" reads in cyan while "App" / " / Render "
            // labels stay in white. When `unitFontSize > 0`, the unit
            // portion of every value run shrinks to that size while
            // the digit portion stays at the base size — used by the
            // bottom panel to render " °C" / " %" at the small label
            // size next to the big-number digit prefix.
            //
            // NOTE on tabular figures (`tnum`): an earlier iteration
            // attached an IDWriteTypography with DWRITE_FONT_FEATURE_TAG
            // _TABULAR_FIGURES via layout->SetTypography in this code
            // path AND on every drawAscii / drawWide path. A diagnostic
            // CI run (see PR #21 commits e453b52 / b067bdc) confirmed
            // the typography object was created successfully and
            // SetTypography returned S_OK, yet the rendered output was
            // bit-identical to the no-tnum baseline — chiffres "1"
            // stayed at 318 width units instead of widening to 494
            // like the font's tnum lookup specifies.
            //
            // Root cause hypothesis: DirectWrite's shape engine ignores
            // IDWriteTypography feature overrides when the resolved font
            // face comes from a custom IDWriteFontCollection populated
            // via IDWriteInMemoryFontFileLoader (the path used by
            // loadBundledFontCollection above). The chiffres-jitter
            // cosmetic issue stays unaddressed until we either find a
            // working DirectWrite path or accept the jitter as a known
            // footnote. The hook point if we ever revisit is exactly
            // here — applied per-layout, not per-format, so this
            // helper would gain a SetTypography call without further
            // refactor.
            void drawValueWide(
                ID2D1RenderTarget* rt,
                const std::wstring& wide,
                IDWriteTextFormat* baseFmt,
                const D2D1_RECT_F& rect,
                ID2D1Brush* brush,
                ID2D1Brush* chiffresBrush = nullptr,
                float unitFontSize = 0.0f) const {
                if (wide.empty() || !baseFmt) return;

                ComPtr<IDWriteTextLayout> layout;
                if (FAILED(m_dwriteFactory->CreateTextLayout(
                        wide.c_str(),
                        static_cast<UINT32>(wide.length()),
                        baseFmt,
                        rect.right - rect.left,
                        rect.bottom - rect.top,
                        layout.GetAddressOf()))) {
                    // Fall back to the simple single-style path so the
                    // panel still shows something readable. The whole
                    // string renders in the BASE format (upright
                    // Rajdhani for the value cases) — digit runs lose
                    // their italic Barlow flavour for this frame.
                    drawWide(rt, wide.c_str(), baseFmt, rect, brush);
                    return;
                }

                // Lock the layout's line spacing to the base format's
                // natural metrics BEFORE applying any per-range font
                // overrides. Without this, swapping the digit ranges to
                // Barlow Medium Italic widens the line (DirectWrite
                // takes max ascent/descent across the line's fonts),
                // which shifts the paragraph-centred baseline a couple
                // of pixels relative to the histogram title rendered
                // via DrawTextW (which stays on pure-base-format line
                // metrics). GetLineMetrics here reads the natural
                // Rajdhani-only line because no overrides have been
                // applied yet; piping those values through
                // SetLineSpacing(UNIFORM, …) freezes them so the later
                // Barlow per-range swap can't shift them.
                {
                    DWRITE_LINE_METRICS lm{};
                    UINT32 lineCount = 0;
                    if (SUCCEEDED(layout->GetLineMetrics(&lm, 1, &lineCount))
                        && lineCount > 0) {
                        layout->SetLineSpacing(
                            DWRITE_LINE_SPACING_METHOD_UNIFORM,
                            lm.height, lm.baseline);
                    }
                }

                for (const ValueRun& run : findValueRuns(wide)) {
                    // Italic sub-range → Barlow Medium Italic. Skipped
                    // when the prefix is dash/dot-only (italicLen == 0
                    // for "-- °C" / "--.- ms" placeholders). Font
                    // resolution happens against m_customFontCollection
                    // attached to baseFmt; that collection holds both
                    // "Rajdhani" and "Barlow", so SetFontFamilyName
                    // resolves without falling back to system fonts.
                    if (run.italicLen > 0) {
                        const DWRITE_TEXT_RANGE italicRange{
                            run.italicStart, run.italicLen};
                        layout->SetFontFamilyName(L"Barlow", italicRange);
                        layout->SetFontWeight(DWRITE_FONT_WEIGHT_MEDIUM, italicRange);
                        layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, italicRange);
                    }

                    // Unit sub-range → smaller size if requested
                    // (bottom-panel " °C" / " %" at the label size).
                    if (run.unitLen > 0 && unitFontSize > 0.0f) {
                        layout->SetFontSize(unitFontSize,
                            DWRITE_TEXT_RANGE{run.unitStart, run.unitLen});
                    }

                    // Whole value range (prefix + unit) → chiffres
                    // brush via SetDrawingEffect, so D2D's default
                    // text renderer paints both portions with that
                    // brush instead of the base `brush`. Placeholder
                    // dashes ride along with the digit + unit even
                    // when italicLen == 0.
                    if (chiffresBrush) {
                        layout->SetDrawingEffect(chiffresBrush,
                            DWRITE_TEXT_RANGE{
                                run.brushStart, run.brushLen});
                    }
                }
                rt->DrawTextLayout(
                    D2D1::Point2F(rect.left, rect.top),
                    layout.Get(), brush);
            }

            // ASCII overload — byte-widens to wstring and forwards.
            // Caller contract: `s` is ASCII-only (same as drawAscii).
            void drawValueAscii(
                ID2D1RenderTarget* rt,
                const std::string& s,
                IDWriteTextFormat* baseFmt,
                const D2D1_RECT_F& rect,
                ID2D1Brush* brush,
                ID2D1Brush* chiffresBrush = nullptr,
                float unitFontSize = 0.0f) const {
                if (s.empty()) return;
                std::wstring wide(s.begin(), s.end());
                drawValueWide(rt, wide, baseFmt, rect, brush,
                               chiffresBrush, unitFontSize);
            }

            // -------- Header bar --------------------------------------------
            //
            // Layout: 5 cells of equal width separated by 1-px vertical
            // bars. Each cell has a small uppercase label at the top
            // and a big number below. The FPS cell uses the white
            // brush; the four accent cells (AVG / P95 / P99 / P99.9)
            // use cyan.
            //
            // Cell width = ~688 / 5 ≈ 137 px on the 720-wide texture
            // (kInnerR - kInnerL). Barlow Medium Italic kFontBigNumber=52 px
            // on "142" measures ~95-100 px (Barlow is wider than the
            // previous Rajdhani Bold ~78 px, italic slant adds a few
            // px more). kFontTinyLabel=17 px upright Medium labels
            // stay well under 50 px even for "P99.9". Comfortable
            // margins. On systems without the bundled font we fall
            // back to Bahnschrift — narrower but no true italic, so
            // chiffres come out as synthetic-oblique upright Bahnschrift.
            void drawHeaderBar(ID2D1RenderTarget* rt, float l, float t,
                                float r, float b,
                                const OverlayDisplayValues& v) const {
                drawPanelBg(rt, l, t, r, b);

                const float w = r - l;
                const float cellW = w / 5.0f;

                // Vertical separators between cells (4 of them now).
                for (int i = 1; i <= 4; ++i) {
                    const float x = l + cellW * static_cast<float>(i);
                    rt->DrawLine(
                        D2D1::Point2F(x, t + kHeaderSepInsetY),
                        D2D1::Point2F(x, b - kHeaderSepInsetY),
                        m_brushSeparator.Get(), 1.0f);
                }

                const float labelH = 22.0f;
                const float labelY = t + 4.0f;
                const float valueY = labelY + labelH;

                drawHeaderCell(rt,
                                l + cellW * 0.0f, labelY, l + cellW * 1.0f, valueY,
                                L"FPS", v.fps_instant,
                                m_fmtBigNumber.Get(),
                                m_brushTextWhite.Get());
                drawHeaderCell(rt,
                                l + cellW * 1.0f, labelY, l + cellW * 2.0f, valueY,
                                L"FPS AVG", v.fps_avg,
                                m_fmtAccentNumber.Get(),
                                m_brushAccentCyan.Get());
                drawHeaderCell(rt,
                                l + cellW * 2.0f, labelY, l + cellW * 3.0f, valueY,
                                L"P95", v.fps_p95,
                                m_fmtAccentNumber.Get(),
                                m_brushAccentCyan.Get());
                drawHeaderCell(rt,
                                l + cellW * 3.0f, labelY, l + cellW * 4.0f, valueY,
                                L"P99", v.fps_p99,
                                m_fmtAccentNumber.Get(),
                                m_brushAccentCyan.Get());
                drawHeaderCell(rt,
                                l + cellW * 4.0f, labelY, l + cellW * 5.0f, valueY,
                                L"P99.9", v.fps_p99_9,
                                m_fmtAccentNumber.Get(),
                                m_brushAccentCyan.Get());
            }

            void drawHeaderCell(ID2D1RenderTarget* rt, float l, float t,
                                 float r, float valueY,
                                 const wchar_t* label,
                                 const std::string& value,
                                 IDWriteTextFormat* valueFormat,
                                 ID2D1Brush* valueBrush) const {
                const D2D1_RECT_F labelRect = D2D1::RectF(l, t, r, valueY);
                drawWide(rt, label, m_fmtTinyLabelCenter.Get(),
                          labelRect, m_brushTextWhite.Get());
                const D2D1_RECT_F valueRect = D2D1::RectF(
                    l, valueY - 2.0f, r,
                    valueY + kFontBigNumber + 6.0f);
                drawAscii(rt, value, valueFormat, valueRect, valueBrush);
            }

            // -------- Frametime panel ---------------------------------------
            //
            // Layout:
            //   - title "GPU FRAMETIME MS" (top-left, ~14 px line)
            //   - current value "6.7" + cyan "ms" suffix (top-right)
            //   - 4 horizontal dashed grid lines
            //   - histogram bars filling the remaining vertical space
            // Draws only the "chrome" of a frametime panel: panel
            // background, title, and the top-right current value. The
            // histogram region itself (background fill, dashed grid,
            // bars, budget line) is owned by drawHistoRegion() and is
            // refreshed every frame so the ring buffer's scroll stays
            // smooth — see paint()'s static/dynamic dispatch.
            void drawFrametimePanel(ID2D1RenderTarget* rt, float l, float t,
                                     float r, float b,
                                     const wchar_t* title,
                                     const std::string& currentValue,
                                     const std::string& secondaryValue) const {
                drawPanelBg(rt, l, t, r, b);

                // Title bar — top inner padding.
                // Title row paddings shared with drawHistoRegion()
                // — see kPanelTitleTopPad / kHistoTitleGap at the
                // layout-constants block.
                const float titleT = t + kPanelTitleTopPad;
                const float titleB = titleT + kHistoTitleH;
                const D2D1_RECT_F titleRect = D2D1::RectF(
                    l + kSectionInnerPad, titleT,
                    r - kSectionInnerPad, titleB);
                drawWide(rt, title, m_fmtSectionTitle.Get(), titleRect,
                          m_brushTextWhite.Get());

                // Current value (top-right). Two shapes depending on
                // whether `secondaryValue` is empty:
                //
                //   GPU panel (secondaryValue == "")   : "6.7 ms"
                //   CPU panel (secondaryValue == "4.3"): "App 4.3 ms / Render 7.4 ms"
                //
                // Both panels go through drawValueAscii: digit runs
                // flip to Barlow Medium Italic and the rest stays in
                // the base format (Rajdhani SemiBold upright). The GPU
                // panel uses a single cyan brush — "6.7" italic Barlow
                // cyan + " ms" upright Rajdhani cyan. The CPU compound
                // passes two brushes so only the "App" / " / Render "
                // label copy renders in white; each "X.X ms" value run
                // (digit + unit) picks up the chiffres brush in cyan.
                // Value rect shares the title rect's vertical bounds
                // (titleT → titleB) so paragraph CENTER places the
                // value baseline at the same line as the title — both
                // fonts are now at kFontMs / kFontMsCompound /
                // kFontSectionTitle = 18 px so the line metrics match.
                if (secondaryValue.empty()) {
                    // GPU panel: short "6.7 ms" primary frametime
                    // read-out. drawValueAscii auto-detects the "6.7
                    // ms" value run; digit prefix italic Barlow, " ms"
                    // upright Rajdhani, both cyan.
                    const std::string s = currentValue + " ms";
                    const D2D1_RECT_F valueRect = D2D1::RectF(
                        r - kSectionInnerPad - 160.0f, titleT,
                        r - kSectionInnerPad,          titleB);
                    drawValueAscii(rt, s, m_fmtMsValue.Get(), valueRect,
                                    m_brushAccentCyan.Get());
                } else {
                    // CPU panel: compound "App {x} ms / Render {y} ms".
                    // drawValueAscii finds the two "X.X ms" value runs
                    // and paints them in cyan (digits italic Barlow,
                    // " ms" upright Rajdhani — same colour, different
                    // font). "App" and " / Render " stay upright
                    // Rajdhani in white via the base brush.
                    const std::string compound =
                        "App " + secondaryValue + " ms / Render "
                        + currentValue + " ms";
                    const D2D1_RECT_F valueRect = D2D1::RectF(
                        r - kSectionInnerPad - 320.0f, titleT,
                        r - kSectionInnerPad,          titleB);
                    drawValueAscii(rt, compound, m_fmtMsCompound.Get(),
                                    valueRect,
                                    m_brushTextWhite.Get(),    // "App" / "/ Render"
                                    m_brushAccentCyan.Get());  // value runs
                }

                // Histogram region is intentionally left untouched
                // here — drawHistoRegion() fills it every frame so the
                // bars scroll at the host rate while the chrome above
                // only repaints on snap.version ticks.
            }

            // Histogram region: filled every frame with the panel's
            // background, dashed grid, current ring contents, and the
            // budget reference line. Repainting just this rectangle
            // each frame keeps the histogram scrolling smoothly at
            // 90-144 Hz while the surrounding chrome (panel title,
            // current numeric value, outer frame, header, bottom row)
            // is only redrawn when the aggregator ticks (~10 Hz).
            //
            // `panelY` is the panel's outer top in shim-pixel space;
            // the region's geometry is derived from layout constants
            // so the rect matches drawFrametimePanel's title row by
            // construction (any drift here would silently misalign
            // the grid under the title).
            void drawHistoRegion(ID2D1RenderTarget* rt, float panelY,
                                  const HistogramRing<kRingSize>& ring,
                                  float targetFps,
                                  ID2D1LinearGradientBrush* barBrush) const {
                // Inner bounds + title paddings come from namespace-
                // scope constexpr so this rect matches drawFrametime
                // Panel()'s title row by construction. The previous
                // 6.0f / kOuterPad-based duplication was a drift risk.
                const float titleB = panelY + kPanelTitleTopPad + kHistoTitleH;
                const float histoL = kInnerL + kSectionInnerPad;
                const float histoR = kInnerR - kSectionInnerPad;
                const float histoT = titleB + kHistoTitleGap;
                const float histoB = panelY + kFrametimeHeight -
                                      kSectionInnerPad;

                // Opaque erase of the previous frame's bars+grid.
                // Panel-bg brush is the same dark carbon used by
                // drawPanelBg, so the seam under the title is
                // invisible.
                rt->FillRectangle(
                    D2D1::RectF(histoL, histoT, histoR, histoB),
                    m_brushPanelBg.Get());

                drawDashedGrid(rt, histoL, histoT, histoR, histoB,
                                /*lineCount=*/4);

                const int64_t budgetNs = targetFps > 0.0f
                    ? static_cast<int64_t>(1.0e9f / targetFps)
                    : 0;
                drawHistogramBars(rt, ring, budgetNs,
                                   histoL, histoT, histoR, histoB,
                                   barBrush);

                // Budget line drawn AFTER bars so overruns visually
                // pierce it (the user reads "bar crosses line ⇒
                // budget breach"). Same brush/stroke as the static
                // version used to do.
                drawBudgetLine(rt, histoL, histoT, histoR, histoB);
            }

            // Budget reference line — fine solid line at the Y
            // position where a bar at exactly 100 % budget tops out
            // (= budgetLineFraction from the strip top). Drawn with
            // the dedicated m_brushBudgetLine (brighter than the
            // dashed grid). 1 px stroke, slight horizontal inset so
            // it doesn't touch the panel's inner border.
            void drawBudgetLine(ID2D1RenderTarget* rt, float l, float t,
                                  float r, float b) const {
                const float y = t + (b - t) * budgetLineFraction();
                rt->DrawLine(
                    D2D1::Point2F(l, y),
                    D2D1::Point2F(r, y),
                    m_brushBudgetLine.Get(), 1.0f);
            }

            // 4 evenly-spaced horizontal dashed lines across the
            // histogram region. Visual purpose: give the user a sense
            // of "vertical scale" for the bar heights, fpsVR-style.
            void drawDashedGrid(ID2D1RenderTarget* rt, float l, float t,
                                  float r, float b, int lineCount) const {
                const float h = b - t;
                for (int i = 1; i <= lineCount; ++i) {
                    const float y = t + h * static_cast<float>(i) /
                                          static_cast<float>(lineCount + 1);
                    rt->DrawLine(
                        D2D1::Point2F(l, y),
                        D2D1::Point2F(r, y),
                        m_brushGridDash.Get(), 1.0f,
                        m_strokeDashed.Get());
                }
            }

            void drawHistogramBars(ID2D1RenderTarget* rt,
                                    const HistogramRing<kRingSize>& ring,
                                    int64_t budgetNs,
                                    float l, float t, float r, float b,
                                    ID2D1LinearGradientBrush* accentBrush) const {
                if (r <= l || b <= t || budgetNs <= 0) return;
                const std::size_t n = ring.size();
                if (n == 0) return;
                const float fullW = r - l;
                const float stripH = b - t;
                const float barW =
                    (fullW - kHistoBarGap * static_cast<float>(n - 1)) /
                    static_cast<float>(n);
                if (barW <= 0.0f) return;

                // Update the gradient endpoints to span the strip's
                // vertical range. Each bar then samples the gradient
                // from its own position within the strip — a bar
                // that only reaches halfway up gets only the bottom
                // half of the gradient, matching how the spec's
                // mock-up renders. accentBrush is typed as
                // ID2D1LinearGradientBrush* so the previous per-
                // frame QueryInterface (288 QIs/s at 144 Hz × 2
                // strips) is gone — we already know the type at
                // brush-creation time in initBrushes().
                accentBrush->SetStartPoint(D2D1::Point2F((l + r) * 0.5f, t));
                accentBrush->SetEndPoint  (D2D1::Point2F((l + r) * 0.5f, b));

                // kDashPlaceholderH from overlay_layout.h — shared with the
                // GPU path so the empty-slot dash height can't drift.
                for (std::size_t i = 0; i < n; ++i) {
                    const float x = l + static_cast<float>(i) * (barW + kHistoBarGap);
                    const int64_t sample = ring.at(i);
                    if (sample <= 0) {
                        // Empty slot — render a 2-px dash at the
                        // strip bottom as a placeholder. Tells the
                        // user "this position exists, just no
                        // signal yet" (typical during the first
                        // second of a session while the ring fills,
                        // or during a transient where a GPU query
                        // result wasn't ready). Same brush as the
                        // dashed grid lines so the placeholders
                        // read as chart chrome rather than as data.
                        const D2D1_RECT_F dash = D2D1::RectF(
                            x, b - kDashPlaceholderH,
                            x + barW, b);
                        rt->FillRectangle(dash, m_brushGridDash.Get());
                        continue;
                    }
                    const auto vis = barVisualForSample(sample, budgetNs);
                    const float h = vis.heightFraction * stripH;
                    if (h <= 0.0f) continue;
                    // Per-bar tier colour: healthy bars use the
                    // panel's accent (teal for GPU, blue for CPU),
                    // warning tier goes ORANGE, critical goes RED.
                    // Same palette as the gauge — keeps the visual
                    // language consistent ("orange/red anywhere ⇒
                    // headroom problem").
                    ID2D1Brush* brush = accentBrush;
                    if (vis.tier == BarTier::Red)         brush = m_brushGaugeRed.Get();
                    else if (vis.tier == BarTier::Orange) brush = m_brushOrange.Get();
                    const D2D1_RECT_F bar = D2D1::RectF(
                        x, b - h,
                        x + barW, b);
                    rt->FillRectangle(bar, brush);
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
            // gaugeTierForUtilisation thresholds as the histogram
            // bars.
            //
            //   GPU panel (3 cells):                CPU panel (2 cells):
            //   ┌─────────┬──────────┬──────┐      ┌──────────┬──────────┐
            //   │GPU TEMP │GPU LOAD  │VRAM  │      │CPU TEMP  │CPU LOAD  │
            //   │ 67 °C   │ 92 %     │ 76 % │      │ -- °C    │ 78 %     │
            //   └─────────┴──────────┴──────┘      └──────────┴──────────┘
            //
            // `prefix` is L"GPU" or L"CPU"; TEMP / LOAD labels are
            // built by appending L" TEMP" / L" LOAD". The VRAM cell
            // is only rendered when `vramValue` is non-empty (the
            // CPU panel passes "" and skips the third column
            // entirely — same code path, fewer columns).
            void drawBottomPanel(ID2D1RenderTarget* rt, float l, float t,
                                  float r, float b,
                                  const wchar_t* tempLabel,
                                  const wchar_t* loadLabel,
                                  const std::string& tempValue,
                                  const std::string& utilValue,
                                  float utilFraction,
                                  const std::string& vramValue,
                                  float vramFraction) const {
                drawPanelBg(rt, l, t, r, b);

                // Tier-brush helper: same logic for LOAD and VRAM.
                // < 80 % = cyan default, 80–89 % = orange warning,
                // ≥ 90 % = red critical. The TEMP value stays white
                // (no thermal-tier contract yet — would require
                // knowing TjMax per SKU).
                auto tierBrush = [&](float fraction) -> ID2D1Brush* {
                    const BarTier tier = gaugeTierForUtilisation(fraction);
                    if (tier == BarTier::Red)    return m_brushGaugeRed.Get();
                    if (tier == BarTier::Orange) return m_brushOrange.Get();
                    return m_brushAccentCyan.Get();
                };

                const bool hasVram = !vramValue.empty();
                const int  numCols = hasVram ? 3 : 2;
                const float colW   = (r - l) / static_cast<float>(numCols);

                // Vertical separators between cells. Same brush as
                // the panel inner stroke so they read as subtle
                // column dividers, not hard borders.
                for (int i = 1; i < numCols; ++i) {
                    const float x = l + colW * static_cast<float>(i);
                    rt->DrawLine(
                        D2D1::Point2F(x, t + kBottomSepInsetY),
                        D2D1::Point2F(x, b - kBottomSepInsetY),
                        m_brushSeparator.Get(), 1.0f);
                }

                // tempLabel / loadLabel arrive pre-built as wide
                // string literals (L"GPU TEMP", L"CPU LOAD", …) — see
                // the comment at the drawBottomPanel call site in
                // paint(). VRAM stays a singleton label ("VRAM")
                // because it's implicitly GPU-side — no "GPU VRAM"
                // because that would be redundant and use a full
                // extra word of horizontal space.

                // Cell layout — label anchored near the top, metric
                // paragraph-centred in the whole cell. The value rect
                // spans the full cell height so paragraph CENTER
                // (inherited from m_fmtTemp) puts the 43-px digit on
                // the cell's vertical centre — equal blank space
                // above (cell top → digit top) and below (digit
                // bottom → cell bottom). The label sits in the upper
                // region without overlapping (digit centre ≈ cellH/2,
                // label rect = 22 px from t+4 → t+26 stays well above
                // the digit at t+33 → t+76 for the current
                // kBottomHeight = 109).
                const float labelY = t + 4.0f;

                auto drawCell = [&](float cellL, float cellR,
                                      const wchar_t* label,
                                      const std::string& asciiValue,
                                      const wchar_t* unitSuffix,
                                      ID2D1Brush* valueBrush,
                                      bool useWideValue) {
                    drawWide(rt, label, m_fmtTinyLabelCenter.Get(),
                              D2D1::RectF(cellL, labelY, cellR,
                                           labelY + 22.0f),
                              m_brushTextWhite.Get());
                    // m_fmtTemp's BASE is Rajdhani upright; the digit
                    // prefix flips to Barlow Italic via drawValueWide /
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
                    const D2D1_RECT_F valueRect = D2D1::RectF(cellL, t, cellR, b);
                    if (useWideValue) {
                        // For the °C suffix the whole string must be
                        // wide. tempValue is ASCII, so byte-widening
                        // it and concatenating L" \u00B0C" works.
                        std::wstring wide(asciiValue.begin(),
                                           asciiValue.end());
                        wide += unitSuffix;
                        drawValueWide(rt, wide, m_fmtTemp.Get(),
                                       valueRect,
                                       valueBrush,
                                       /*chiffresBrush=*/nullptr,
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
                        drawValueAscii(rt, full, m_fmtTemp.Get(),
                                        valueRect,
                                        valueBrush,
                                        /*chiffresBrush=*/nullptr,
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
                drawCell(l, l + colW,
                          tempLabel, tempValue, L" \u00B0C",
                          m_brushTextWhite.Get(), /*useWideValue=*/true);
                // Cell 1 — <prefix> LOAD / "92 %"
                drawCell(l + colW, l + colW * 2.0f,
                          loadLabel, utilValue, L" %",
                          tierBrush(utilFraction), /*useWideValue=*/false);
                // Cell 2 — VRAM / "76 %" (GPU panel only)
                if (hasVram) {
                    drawCell(l + colW * 2.0f, r,
                              L"VRAM", vramValue, L" %",
                              tierBrush(vramFraction), /*useWideValue=*/false);
                }
            }

            // Panel background — flat dark-blue panel with a 1-px
            // separator stroke on the inside. Used for the header,
            // both frametime panels, and the bottom row pair.
            // Earlier iterations added top-edge bevel highlight,
            // bottom-edge bevel shadow, and a low-alpha diagonal
            // carbon-fibre hatch for a "raised metal" industrial
            // look; all three were dropped in favour of a flat panel.
            void drawPanelBg(ID2D1RenderTarget* rt, float l, float t,
                              float r, float b) const {
                const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
                    D2D1::RectF(l, t, r, b), 4.0f, 4.0f);
                rt->FillRoundedRectangle(panel, m_brushPanelBg.Get());
                rt->DrawRoundedRectangle(panel, m_brushSeparator.Get(), 1.0f);
            }

            // Outer frame with chamfered (cut) corners. Builds an
            // octagonal-ish closed path: 4 straight edges joined by
            // 4 diagonal cuts at the corners. `chamfer` is the
            // diagonal cut length in pixels (12 px matches the
            // design's industrial-HUD look). Single fill + stroke
            // pair, no per-corner arc geometry needed.
            void drawChamferedRect(ID2D1RenderTarget* rt,
                                     const D2D1_RECT_F& rect,
                                     float chamfer,
                                     ID2D1Brush* fillBrush,
                                     ID2D1Brush* strokeBrush,
                                     float strokeWidth) const {
                ComPtr<ID2D1PathGeometry> path;
                if (FAILED(m_d2dFactory->CreatePathGeometry(path.GetAddressOf()))) {
                    // Fallback to a plain rounded-rect if path
                    // creation fails for any reason (shouldn't
                    // happen in practice — D2D factory is always
                    // alive at paint time).
                    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                        rect, 8.0f, 8.0f);
                    rt->FillRoundedRectangle(rr, fillBrush);
                    rt->DrawRoundedRectangle(rr, strokeBrush, strokeWidth);
                    return;
                }
                ComPtr<ID2D1GeometrySink> sink;
                if (FAILED(path->Open(sink.GetAddressOf()))) return;
                const float L = rect.left;
                const float Rg = rect.right;
                const float T = rect.top;
                const float B = rect.bottom;
                const float c = chamfer;
                sink->BeginFigure(D2D1::Point2F(L + c, T),
                                   D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(Rg - c, T));        // top edge
                sink->AddLine(D2D1::Point2F(Rg, T + c));        // top-right cut
                sink->AddLine(D2D1::Point2F(Rg, B - c));        // right edge
                sink->AddLine(D2D1::Point2F(Rg - c, B));        // bottom-right cut
                sink->AddLine(D2D1::Point2F(L + c, B));         // bottom edge
                sink->AddLine(D2D1::Point2F(L, B - c));         // bottom-left cut
                sink->AddLine(D2D1::Point2F(L, T + c));         // left edge
                sink->AddLine(D2D1::Point2F(L + c, T));         // top-left cut (close)
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                rt->FillGeometry(path.Get(), fillBrush);
                rt->DrawGeometry(path.Get(), strokeBrush, strokeWidth);
            }

            // -------- Members -----------------------------------------------
            ComPtr<ID2D1Factory>      m_d2dFactory;
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
            // Text formats — one per (font, size, alignment) combo
            // the layout uses. All immutable post-init, so paint()
            // never allocates a format.
            ComPtr<IDWriteTextFormat> m_fmtBigNumber;        // "142" (FPS)
            ComPtr<IDWriteTextFormat> m_fmtAccentNumber;     // "138", "124", "108"
            ComPtr<IDWriteTextFormat> m_fmtTemp;             // "67 °C", "92 %"
            ComPtr<IDWriteTextFormat> m_fmtMsValue;          // "6.7 ms" (GPU)
            ComPtr<IDWriteTextFormat> m_fmtMsCompound;       // "App ... ms / Render ... ms" (CPU, mixed style)
            ComPtr<IDWriteTextFormat> m_fmtTinyLabelCenter;  // "FPS", "GPU TEMP", "GPU LOAD"
            ComPtr<IDWriteTextFormat> m_fmtSectionTitle;     // "GPU FRAMETIME MS"

            // Brushes — the 12-colour palette.
            ComPtr<ID2D1SolidColorBrush> m_brushBg;
            ComPtr<ID2D1SolidColorBrush> m_brushPanelBg;
            ComPtr<ID2D1SolidColorBrush> m_brushFrameLine;
            ComPtr<ID2D1SolidColorBrush> m_brushSeparator;
            ComPtr<ID2D1SolidColorBrush> m_brushTextWhite;
            ComPtr<ID2D1SolidColorBrush> m_brushAccentCyan;
            // Histogram bar brushes use linear gradients (top→
            // bottom) rather than solid colours, matching the design
            // spec's "lit from above" look. Stop collections cached
            // once at init, brushes are reused with per-strip
            // endpoint updates in drawHistogramBars.
            ComPtr<ID2D1GradientStopCollection> m_gpuTealStops;
            ComPtr<ID2D1GradientStopCollection> m_cpuBlueStops;
            ComPtr<ID2D1LinearGradientBrush>    m_brushGpuTealGrad;
            ComPtr<ID2D1LinearGradientBrush>    m_brushCpuBlueGrad;
            ComPtr<ID2D1SolidColorBrush> m_brushOrange;      // warning tier (≥ 80 % load)
            ComPtr<ID2D1SolidColorBrush> m_brushGaugeRed;    // critical tier (≥ 90 % load)
            ComPtr<ID2D1SolidColorBrush> m_brushGridDash;
            ComPtr<ID2D1SolidColorBrush> m_brushBudgetLine;
            // Dashed stroke style for the grid lines.
            ComPtr<ID2D1StrokeStyle>     m_strokeDashed;

            // Static-tier cadence. The static tier (frame chrome,
            // header text, panel titles+values, bottom row) repaints
            // on either:
            //   - `snap.version` changing, or
            //   - kMaxFramesBetweenPaints frames having elapsed since
            //     the last static paint (defensive timeout against
            //     a stalled aggregator).
            //
            // The aggregator publishes at ~10 Hz so the version
            // trigger fires every ~9 frames at 90 Hz host rate;
            // K=30 (~333 ms at 90 Hz, ~208 ms at 144 Hz) only kicks
            // in if the aggregator stops ticking. The dynamic tier
            // (histogram region) runs every frame regardless and is
            // independent of the counter.
            //
            // The decision/update logic lives in overlay_cadence.h
            // (needStaticPaint / commitPaint) so it's unit-testable
            // without a render target — see test_overlay_cadence.cpp.
            static constexpr int kMaxFramesBetweenPaints = 30;
            OverlayDisplayValues m_cachedValues;
            PaintCadence         m_cadence;
            // Chrome-cache flag used by paintChromeOnly() (the shader
            // path's static-tier entry). First call does a full
            // drawChrome() and latches this to true; subsequent calls
            // assume the labels/frame/panel-BGs are still in the shim
            // and re-render only the variable values — cuts the bulk
            // of the chrome cost on every snap.version tick. The flag
            // starts false on construction so a fresh shim gets the
            // full chrome paint; paint() (the D2D fallback path) does
            // its own Clear+full-redraw so it never observes this
            // flag and the two paths can't interfere.
            bool                 m_chromeStaticPainted = false;
            // Values-only paint counter. drawValuesIntoChrome()
            // redraws the bottom row (TEMP / LOAD / VRAM) only every
            // 5th call — temps and util-% don't move fast enough to
            // need the aggregator's full 10 Hz cadence, so we drop
            // them to ~2 Hz and skip ~5 DirectWrite calls on 4 of
            // every 5 ticks. The header (FPS / P95 / P99…) and
            // frametime values stay at 10 Hz.
            int                  m_valueOnlyTick = 0;
        };

        // -------- HistogramBarRenderer: D3D11 instanced histogram region -----
        //
        // Replaces drawHistoRegion()'s D2D work (one FillRectangle per bar,
        // ×kRingSize ×2 panels every frame) with a handful of instanced GPU
        // draws into the shim. Owns two pipelines — solid-colour quads for the
        // background / grid / budget line, and gradient bars for the samples —
        // plus an RTV on the shim and the dynamic instance + constant buffers
        // refilled per panel per frame.
        //
        // Lives on the SAME D3D11 device as the shim's D2D render target, so
        // both share one immediate context. Contract for the caller:
        //   1. Flush() the D2D RT before drawPanel() — D2D batches commands;
        //      flushing commits the chrome so our draws land on top of it.
        //   2. Treat the D3D11 pipeline state as clobbered after drawPanel()
        //      (and D2D clobbers ours), so each drawPanel() sets ALL state.
        //
        // Colours mirror initBrushes() exactly so the GPU output matches the
        // old D2D look (modulo edge anti-aliasing). The histogram rect is
        // fully opaque after the background fill, so premultiplied == straight
        // there and the runtime composites the quad layer correctly.
        class HistogramBarRenderer {
          public:
            bool init(ID3D11Device* device, ID3D11DeviceContext* ctx,
                       ID3D11Texture2D* shim) {
                if (!device || !ctx || !shim) return false;
                m_device = device;
                m_ctx = ctx;

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

                if (FAILED(m_device->CreateRenderTargetView(
                        shim, nullptr, m_rtv.GetAddressOf())))
                    return fail("CreateRenderTargetView (shim)");

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

                const D3D11_INPUT_ELEMENT_DESC quadIL[] = {
                    {"POSITION",   0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,
                     D3D11_INPUT_PER_VERTEX_DATA,   0},
                    {"QUAD_RECT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,
                     D3D11_INPUT_PER_INSTANCE_DATA, 1},
                    {"QUAD_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16,
                     D3D11_INPUT_PER_INSTANCE_DATA, 1},
                };
                if (FAILED(m_device->CreateInputLayout(
                        quadIL, _countof(quadIL),
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

                // Dynamic instance buffers + the bars constant buffer
                // (refilled per panel per frame).
                if (!createDynamic(kRingSize * sizeof(BarInstance),
                                    D3D11_BIND_VERTEX_BUFFER, m_barInstances))
                    return fail("CreateBuffer (bar instances)");
                if (!createDynamic(kQuadSlots * sizeof(QuadInstance),
                                    D3D11_BIND_VERTEX_BUFFER, m_quadInstances))
                    return fail("CreateBuffer (quad instances)");
                if (!createDynamic(sizeof(BarConstants),
                                    D3D11_BIND_CONSTANT_BUFFER, m_barCB))
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

            bool isReady() const noexcept { return m_ready; }

            // Paint one panel's histogram region (bg + grid + bars + budget)
            // into the shim. Rect is in shim pixels; isGpu picks the teal vs
            // blue gradient. budgetNs <= 0 (no display-period info yet
            // from the runtime — the first few frames before
            // xrWaitFrame's predicted period lands) still paints the
            // bg/grid/budget chrome; only the bars themselves are
            // skipped (each one has no reference height, so
            // barVisualForSample returns heightFraction 0 and the loop
            // emits no instances). That keeps the histo region from
            // freezing on stale shim content during the warm-up.
            void drawPanel(const HistogramRing<kRingSize>& ring,
                            int64_t budgetNs,
                            float histoL, float histoT,
                            float histoR, float histoB,
                            bool isGpu) {
                if (!m_ready) return;
                const float fullW = histoR - histoL;
                const float stripH = histoB - histoT;
                if (fullW <= 0.0f || stripH <= 0.0f) return;

                const std::size_t n = ring.size();
                // Fixed integer bar geometry: 4-px bars + 1-px gaps
                // (step 5). Integer width AND integer positions make every
                // bar pixel-identical — no sub-pixel width/gap variation,
                // no AA needed. kRingSize bars span n*5 - 1 px; the histo
                // region is wider, so the run is centred and the slack
                // splits into equal left/right margins. (The strip width
                // can be adapted later if the bars should sit flush to the
                // panel edges.)
                constexpr float kBarPx  = 4.0f;
                constexpr float kStepPx = 5.0f;   // 4-px bar + 1-px gap
                const float usedW = static_cast<float>(n) * kStepPx - 1.0f;
                const float slack = fullW - usedW;
                // Positive slack → centre the run (equal L/R margins).
                // Negative slack (strip narrower than the run, e.g. if a
                // future layout tweak shrinks fullW): shift the start
                // LEFT by the deficit so the rightmost bars stay flush
                // with histoR. The scissor then clips the leftmost
                // (oldest) bars rather than the rightmost (newest) —
                // the newest sample carries the most signal.
                const float startX = histoL + (slack >= 0.0f
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

                // --- refill quad instances: [0]=bg, [1..4]=grid, [5]=budget ---
                {
                    D3D11_MAPPED_SUBRESOURCE map{};
                    if (SUCCEEDED(m_ctx->Map(m_quadInstances.Get(), 0,
                            D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                        auto* q = static_cast<QuadInstance*>(map.pData);
                        q[0] = makeQuad(histoL, histoT, fullW, stripH, kPanelBg);
                        for (int i = 1; i <= 4; ++i) {
                            const float y = histoT + stripH *
                                static_cast<float>(i) / 5.0f;
                            q[i] = makeQuad(histoL, y, fullW, 1.0f, kGridDash);
                        }
                        const float by =
                            histoT + stripH * budgetLineFraction();
                        q[5] = makeQuad(histoL, by, fullW, 1.0f, kBudgetLine);
                        m_ctx->Unmap(m_quadInstances.Get(), 0);
                    }
                }

                // --- constant buffers ---
                {
                    BarConstants bc{};
                    bc.texSize[0] = static_cast<float>(kTexW);
                    bc.texSize[1] = static_cast<float>(kTexH);
                    bc.histoTL[0] = histoL; bc.histoTL[1] = histoT;
                    bc.histoBR[0] = histoR; bc.histoBR[1] = histoB;
                    bc.barWidth = kBarPx;   // fixed 4-px integer width
                    bc.dashHeight = kDashPlaceholderH;  // shared with D2D path
                    copyColor(bc.gradTop,    isGpu ? kGpuGradTop : kCpuGradTop);
                    copyColor(bc.gradBottom, isGpu ? kGpuGradBot : kCpuGradBot);
                    copyColor(bc.orange, kOrange);
                    copyColor(bc.red,    kRed);
                    copyColor(bc.dash,   kGridDash);
                    upload(m_barCB, &bc, sizeof(bc));
                    // m_quadCB is IMMUTABLE (texSize is invariant — see
                    // init()), so no re-upload here.
                }

                // --- pipeline state (D2D clobbered it; set everything) ---
                m_ctx->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
                D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kTexW),
                                  static_cast<float>(kTexH), 0.0f, 1.0f};
                m_ctx->RSSetViewports(1, &vp);
                const D3D11_RECT sc{
                    static_cast<LONG>(std::floor(histoL)),
                    static_cast<LONG>(std::floor(histoT)),
                    static_cast<LONG>(std::ceil(histoR)),
                    static_cast<LONG>(std::ceil(histoB))};
                m_ctx->RSSetScissorRects(1, &sc);
                m_ctx->RSSetState(m_raster.Get());
                const float bf[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                m_ctx->OMSetBlendState(m_blend.Get(), bf, 0xffffffffu);
                m_ctx->IASetPrimitiveTopology(
                    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

                // Three draw passes per panel. Layering relies on D3D11's
                // documented in-order primitive submission: bg + 4 grid
                // lines (alpha-over) → opaque bars → budget line (alpha-
                // over). Without that guarantee the alpha-blended grid and
                // budget could land on the wrong side of the bars.
                //
                // quad pass 1 — bg (instance 0) + 4 grid lines (1..4).
                bindQuadPipeline();
                m_ctx->DrawInstanced(4, 5, 0, 0);

                // bar pass — samples on top of bg/grid (instances 0..barCount).
                if (barCount > 0) {
                    bindBarPipeline();
                    m_ctx->DrawInstanced(4, barCount, 0, 0);
                }

                // quad pass 2 — budget line on top of the bars (instance 5).
                bindQuadPipeline();
                m_ctx->DrawInstanced(4, 1, 0, 5);
            }

          private:
            // Per-instance / constant layouts — must match the HLSL byte for
            // byte (see overlay_bars.hlsli / overlay_quad.hlsli). The
            // static_asserts below freeze the C++ side's sizeof so a future
            // member insertion (e.g. a stray float between dashHeight and
            // gradTop) can't silently shift every colour off by 8 bytes.
            // Mirror cbuffer layout: 7×float4 registers (112 B) for bars,
            // 1×float4 register (16 B) for quads.
            struct BarInstance  { float xLeft; float height; uint32_t tier; };
            struct QuadInstance { float rect[4]; float color[4]; };
            struct BarConstants {
                float texSize[2]; float histoTL[2]; float histoBR[2];
                float barWidth; float dashHeight;
                float gradTop[4]; float gradBottom[4];
                float orange[4]; float red[4]; float dash[4];
            };
            struct QuadConstants { float texSize[2]; float pad[2]; };
            static_assert(sizeof(BarConstants)  == 112,
                          "BarConstants must mirror HLSL cbuffer packing "
                          "(7 × float4 = 112 B) — see overlay_bars.hlsli");
            static_assert(sizeof(QuadConstants) == 16,
                          "QuadConstants must mirror HLSL cbuffer packing "
                          "(1 × float4 = 16 B) — see overlay_quad.hlsli");

            static constexpr UINT kQuadSlots = 6;  // bg + 4 grid + budget

            // Colours (linear RGBA), copied from initBrushes().
            static constexpr float kPanelBg[4]    = {0.035f, 0.039f, 0.039f, 1.00f};
            static constexpr float kGridDash[4]   = {0.353f, 0.431f, 0.451f, 0.30f};
            static constexpr float kBudgetLine[4] = {0.949f, 0.957f, 0.961f, 0.45f};
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
                                          const float c[4]) noexcept {
                QuadInstance q{};
                q.rect[0] = x; q.rect[1] = y; q.rect[2] = w; q.rect[3] = h;
                copyColor(q.color, c);
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
            void upload(ComPtr<ID3D11Buffer>& cb, const void* src, UINT bytes) {
                D3D11_MAPPED_SUBRESOURCE map{};
                if (SUCCEEDED(m_ctx->Map(cb.Get(), 0,
                        D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                    std::memcpy(map.pData, src, bytes);
                    m_ctx->Unmap(cb.Get(), 0);
                }
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
            void bindBarPipeline() {
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
                m_ctx->VSSetConstantBuffers(0, 1, m_barCB.GetAddressOf());
                m_ctx->PSSetConstantBuffers(0, 1, m_barCB.GetAddressOf());
            }

            bool m_ready = false;
            ComPtr<ID3D11Device>           m_device;
            ComPtr<ID3D11DeviceContext>    m_ctx;
            ComPtr<ID3D11RenderTargetView> m_rtv;
            ComPtr<ID3D11VertexShader>     m_barsVS;
            ComPtr<ID3D11PixelShader>      m_barsPS;
            ComPtr<ID3D11VertexShader>     m_quadVS;
            ComPtr<ID3D11PixelShader>      m_quadPS;
            ComPtr<ID3D11InputLayout>      m_barsLayout;
            ComPtr<ID3D11InputLayout>      m_quadLayout;
            ComPtr<ID3D11Buffer>           m_quadVB;
            ComPtr<ID3D11Buffer>           m_barInstances;
            ComPtr<ID3D11Buffer>           m_quadInstances;
            ComPtr<ID3D11Buffer>           m_barCB;
            ComPtr<ID3D11Buffer>           m_quadCB;
            ComPtr<ID3D11BlendState>       m_blend;
            ComPtr<ID3D11RasterizerState>  m_raster;
        };

        // Watchdog: max frames between forced chrome repaints on the
        // shader path. K=30 (~0.33 s @ 90 Hz, ~0.21 s @ 144 Hz) only
        // fires if the aggregator's snap.version stops ticking — purely
        // defensive. Shared by both renderers so a tweak lands once.
        constexpr int kChromeWatchdogFrames = 30;

        // -------- Shared shader-path paint -----------------------------------
        //
        // D2D chrome (only when snap.version ticks or the watchdog fires)
        // + GPU instanced bars (every frame) into the shim. Used by both
        // D3D11OverlayRenderer and D3D12OverlayRenderer — the only piece
        // that differs across paths is the shim's render target, which
        // the caller passes in. `bars` already owns the matching D3D11
        // immediate context (captured at init() time on the same device
        // backing `shimRT`), so the two stages talk to one another
        // through that context with no extra plumbing here.
        //
        // Ordering across the D2D ↔ D3D11 boundary is the load-bearing
        // detail: CoreRenderer::paintChromeOnly's EndDraw flushes the D2D
        // command batch onto the SAME immediate context that bars.draw
        // Panel() submits its instanced draws to, so the GPU executes
        // [chrome → bars] in submission order — no fence, no flush, no
        // extra sync needed. On the D3D12 path the D3D11On12 wrapped-
        // resource Acquire/Release dance happens AFTER this returns (in
        // renderAndCompose) and ferries the shim into the D3D12 swapchain
        // image without disturbing this ordering.
        //
        // Returns false only when a needed chrome repaint failed; the
        // cadence then leaves lastPaintedVersion alone so the next frame
        // retries the static branch until it lands.
        inline bool paintShimViaShader(
                CoreRenderer&                       core,
                HistogramBarRenderer&               bars,
                PaintCadence&                       cadence,
                ID2D1RenderTarget*                  shimRT,
                const HistogramRing<kRingSize>&     cpuRing,
                const HistogramRing<kRingSize>&     gpuRing,
                const OverlaySnapshot&              snap) {
            const bool needStatic = needStaticPaint(
                cadence, snap.version, kChromeWatchdogFrames);

            bool ok = true;
            if (needStatic) {
                ok = core.paintChromeOnly(shimRT, snap);
            }

            if (ok) {
                // Panel Ys + histo rects from the shared layout constants;
                // identical geometry to drawHistoRegion / drawChrome so
                // the GPU bars line up under the D2D titles.
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
                bars.drawPanel(gpuRing, budgetNs,
                                histoL, gpuT, histoR, gpuB,
                                /*isGpu=*/true);
                bars.drawPanel(cpuRing, budgetNs,
                                histoL, cpuT, histoR, cpuB,
                                /*isGpu=*/false);
            }

            commitPaint(cadence, needStatic, ok, snap.version);
            return ok;
        }

        // -------- D3D11 native renderer --------------------------------------
        //
        // App uses D3D11 directly. Each swapchain image is an ID3D11Texture2D
        // we obtain via xrEnumerateSwapchainImages, and we create one D2D
        // render target per image (cached for the swapchain's lifetime).
        class D3D11OverlayRenderer final : public OverlayRenderer {
          public:
            D3D11OverlayRenderer(OpenXrApi* api, XrSession session,
                                  ID3D11Device* device)
                : m_api(api), m_session(session), m_device(device) {
                if (m_device) m_device->GetImmediateContext(m_context.GetAddressOf());
                m_ready = init();
            }

            ~D3D11OverlayRenderer() override {
                // Session summary for the keyed-mutex timeouts. The
                // per-occurrence log throttles to first + every
                // kMutexTimeoutLogStride-th, so a user reporting a
                // glitchy HUD may have only one or two log lines
                // even after hours of timeouts. Always emit a
                // session-end total when non-zero — support reads
                // this to triage "intermittent sync issue" vs
                // "isolated startup hiccup".
                if (m_myMutexTimeouts || m_appMutexTimeouts) {
                    Log(fmt::format(
                        "xr_telemetry: session ended with {} paint-mutex + "
                        "{} copy-mutex acquire timeouts.\n",
                        m_myMutexTimeouts, m_appMutexTimeouts));
                }
                if (m_swapchain != XR_NULL_HANDLE && m_api) {
                    m_api->xrDestroySwapchain(m_swapchain);
                }
                // The shared NT handle outlives the resource pair — close
                // it so a long-running app doesn't leak kernel handles
                // session-after-session.
                if (m_sharedHandle) {
                    ::CloseHandle(m_sharedHandle);
                    m_sharedHandle = nullptr;
                }
            }

            bool isReady() const noexcept override { return m_ready; }

            void pushFrameSample(int64_t cpu_per_cycle_ns,
                                  int64_t gpu_time_ns) override {
                m_cpuRing.push(cpu_per_cycle_ns);
                m_gpuRing.push(gpu_time_ns);
            }

            const XrCompositionLayerBaseHeader* renderAndCompose(
                XrSpace viewSpace,
                const OverlaySnapshot& snap,
                const std::string& position,
                float scale) override {
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

                // 2. Paint into the shim texture (typed BGRA8 we own)
                //    on OUR private D3D11 device. The app's device may
                //    not have been created with D3D11_CREATE_DEVICE_
                //    BGRA_SUPPORT (verified empirically: D2D returns
                //    E_INVALIDARG on CreateDxgiSurfaceRenderTarget on
                //    any texture from a non-BGRA-flag device, even when
                //    the texture itself is BGRA8_UNORM). The shim
                //    lives on m_myDevice (always created with BGRA
                //    support) and is shared into the app's device via
                //    DXGI_RESOURCE_MISC_SHARED_NTHANDLE so we can copy
                //    it to the runtime's swapchain image without
                //    crossing process boundaries.
                //
                //    Cross-device sync uses a keyed mutex pair:
                //      key=0 : owned by app side (initial state after
                //              CreateSharedHandle / OpenSharedResource1)
                //      key=1 : owned by our render side after we paint
                //    Each side AcquireSyncs the key it expects, then
                //    ReleaseSyncs the OTHER key once it's done, handing
                //    over to the next side.
                //
                //    CRITICAL INVARIANT: every frame must end with the
                //    mutex back at key=0. If we transition 0→1 (via a
                //    successful paint-side AcquireSync+ReleaseSync) but
                //    fail to perform the matching 1→0 transition on the
                //    copy side, the next frame's AcquireSync(0,50) will
                //    time out forever and the HUD freezes for the rest
                //    of the session — and the one-shot timeout log hides
                //    the symptom from us. So the copy block below treats
                //    the 1→0 handover as MANDATORY whenever we hold the
                //    key, falling back to an empty handover if the
                //    happy-path copy is gated off.
                bool painted     = false;
                bool weHaveKey1  = false;  // we owe a 1→0 transition to the mutex pair
                if (m_myShimMutex && m_myShimRenderTarget) {
                    // Bounded acquire so an upstream sync failure on
                    // the app side doesn't hang the frame thread
                    // forever. 50 ms is well above any realistic copy
                    // cost on the previous frame.
                    HRESULT hr = m_myShimMutex->AcquireSync(0, 50);
                    if (hr == S_OK) {
                        painted = m_useShaderBars
                            ? paintShimViaShader(m_core, m_bars, m_barsCadence,
                                                  m_myShimRenderTarget.Get(),
                                                  m_cpuRing, m_gpuRing, snap)
                            : m_core.paint(m_myShimRenderTarget.Get(), snap,
                                            m_cpuRing, m_gpuRing);
                        m_myShimMutex->ReleaseSync(1);
                        weHaveKey1 = true;
                    } else {
                        // Periodic log: first occurrence + every
                        // kMutexTimeoutLogStride-th after that. A
                        // recurring hiccup at e.g. 1 % of frames at
                        // 144 Hz produces one log line every ~12 s,
                        // visible enough to triage but not flooding.
                        ++m_myMutexTimeouts;
                        if (m_myMutexTimeouts == 1 ||
                            m_myMutexTimeouts % kMutexTimeoutLogStride == 0) {
                            Log(fmt::format(
                                "xr_telemetry: overlay paint mutex acquire timed "
                                "out (HRESULT={:#x}, key=0, total={}). HUD will "
                                "skip frames until sync recovers.\n",
                                static_cast<unsigned int>(hr),
                                m_myMutexTimeouts));
                        }
                    }
                }

                // 3. Copy the painted shim into the OpenXR swapchain
                //    image via the app's device. The two textures are
                //    in the same "type group" (B8G8R8A8 family), so
                //    D3D11 allows the copy even though one side is
                //    typeless. If anything in the copy gate is missing
                //    (no app context, runtime returned a bogus image
                //    index, or paint itself returned false), we still
                //    MUST perform the empty 1→0 handover to satisfy
                //    the keyed-mutex invariant — see the long comment
                //    in step 2.
                if (weHaveKey1) {
                    const bool canCopy = painted && m_context &&
                                          imageIdx < m_images.size();
                    bool handedBack = false;
                    if (m_appShimMutex) {
                        HRESULT hr = m_appShimMutex->AcquireSync(1, 50);
                        if (hr == S_OK) {
                            if (canCopy) {
                                m_context->CopyResource(m_images[imageIdx].Get(),
                                                         m_appShim.Get());
                            }
                            m_appShimMutex->ReleaseSync(0);
                            handedBack = true;
                        } else {
                            ++m_appMutexTimeouts;
                            if (m_appMutexTimeouts == 1 ||
                                m_appMutexTimeouts % kMutexTimeoutLogStride == 0) {
                                Log(fmt::format(
                                    "xr_telemetry: overlay copy mutex acquire timed "
                                    "out (HRESULT={:#x}, key=1, total={}). HUD will "
                                    "skip frames until sync recovers.\n",
                                    static_cast<unsigned int>(hr),
                                    m_appMutexTimeouts));
                            }
                        }
                    }
                    if (!handedBack) {
                        // Recovery path: the happy-path handover failed
                        // (app-side acquire timed out, or m_appShimMutex
                        // is somehow null after a successful init). The
                        // mutex pair is still at key=1 — fix it via the
                        // paint-side handle. We own the key (we just
                        // ReleaseSync(1)'d it three lines up), so the
                        // AcquireSync(1) should be instant; we then
                        // ReleaseSync(0) WITHOUT copying. The swapchain
                        // image keeps whatever it had last frame; we
                        // suppress this frame's composition layer
                        // below so the runtime doesn't repaint stale
                        // bytes as fresh telemetry.
                        if (m_myShimMutex &&
                            m_myShimMutex->AcquireSync(1, 50) == S_OK) {
                            m_myShimMutex->ReleaseSync(0);
                        }
                        // If even this fails (the mutex is genuinely
                        // broken — kernel object corrupted, app-side
                        // device removed mid-handover, etc.), the next
                        // frame's AcquireSync(0) will time out and we
                        // log + bail cleanly. No spin, no deadlock.
                        painted = false;
                    }
                }

                // 4. Release regardless of paint success — the runtime
                //    needs the image released to keep the swapchain
                //    cycling.
                XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                m_api->xrReleaseSwapchainImage(m_swapchain, &releaseInfo);

                if (!painted) return nullptr;

                // 4. Build the composition layer pose. The immutable
                //    fields (type, layerFlags, swapchain ref, imageRect,
                //    identity orientation) were filled once in init();
                //    here we just write the three values that vary per
                //    frame: the head-locked view space (a session-
                //    scoped XrSpace that the caller may rotate via
                //    settings if we ever expose it), and the
                //    position/size pair derived from the user's
                //    settings.overlay.position + scale.
                const auto geo = geometryForPosition(position, scale);
                m_quadLayer.space = viewSpace;
                m_quadLayer.pose.position = {geo.pos_x, geo.pos_y, geo.pos_z};
                m_quadLayer.size = {geo.width_m, geo.height_m};

                return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_quadLayer);
            }

          private:
            bool init() {
                if (!m_device || !m_api || m_session == XR_NULL_HANDLE) return false;
                if (!m_core.init()) {
                    Log("xr_telemetry: overlay D2D/DirectWrite init failed; HUD disabled\n");
                    return false;
                }
                const int64_t format = pickSwapchainFormat(m_api, m_session);
                if (format == 0) {
                    Log("xr_telemetry: overlay disabled — runtime doesn't advertise "
                        "DXGI_FORMAT_B8G8R8A8_UNORM among supported swapchain formats\n");
                    return false;
                }

                XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                  XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                sci.format = format;
                sci.sampleCount = 1;
                sci.width = static_cast<uint32_t>(kTexW);
                sci.height = static_cast<uint32_t>(kTexH);
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

                // Stash the OpenXR swapchain images. We don't create a
                // D2D render target on each one (Pimax hands back
                // DXGI_FORMAT_B8G8R8A8_TYPELESS textures, which D2D
                // refuses), instead we paint into a shim BGRA8_UNORM
                // texture we own and CopyResource it into the OpenXR
                // image per frame. The diagnostic log on image 0 keeps
                // around for future format / bindFlags regressions.
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

                // Diagnostic for the app's D3D11 device — D2D requires
                // it to have been created with D3D11_CREATE_DEVICE_BGRA
                // _SUPPORT (= 0x20). If the app didn't ask for that
                // flag (LMU / DR2 / etc. don't, since they don't use
                // D2D), every D2D RT-creation against textures from
                // this device returns E_INVALIDARG. Below we sidestep
                // this by allocating our OWN D3D11 device with the
                // flag set, paint with D2D on that device, and share
                // the result back to the app's device via a shared
                // NT handle + keyed mutex.
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

                // 1. Reach the adapter the app's device sits on; we
                //    want our private device on the SAME GPU so the
                //    shared-resource path stays in-VRAM (driver-side
                //    optimisation; cross-adapter shares fall back to
                //    a CPU bounce).
                ComPtr<IDXGIDevice> appDxgi;
                if (FAILED(m_device.As(&appDxgi))) {
                    Log("xr_telemetry: overlay disabled — app device "
                        "QueryInterface IDXGIDevice failed\n");
                    return false;
                }
                ComPtr<IDXGIAdapter> adapter;
                if (FAILED(appDxgi->GetAdapter(adapter.GetAddressOf()))) {
                    Log("xr_telemetry: overlay disabled — IDXGIDevice "
                        "GetAdapter failed\n");
                    return false;
                }

                // 2. Create our private D3D11 device on the same
                //    adapter, force BGRA_SUPPORT so D2D works on
                //    anything we allocate. SINGLETHREADED is the
                //    sensible default since only this object's
                //    renderAndCompose ever touches it.
                D3D_FEATURE_LEVEL outLevel{};
                HRESULT hr = ::D3D11CreateDevice(
                    adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                        D3D11_CREATE_DEVICE_SINGLETHREADED,
                    nullptr, 0, D3D11_SDK_VERSION,
                    m_myDevice.GetAddressOf(),
                    &outLevel,
                    nullptr);   // we never invoke ID3D11DeviceContext
                                // directly — D2D drives it through
                                // the RT — so don't even capture it.
                if (FAILED(hr) || !m_myDevice) {
                    Log(fmt::format(
                        "xr_telemetry: overlay disabled — D3D11CreateDevice "
                        "(BGRA-capable secondary) failed (HRESULT={:#x})\n",
                        static_cast<unsigned int>(hr)));
                    return false;
                }

                // 3. Create the shim texture on OUR device with the
                //    shared-NT-handle flag + keyed mutex. The keyed
                //    mutex serialises access between our paint side
                //    and the app's copy side per frame.
                D3D11_TEXTURE2D_DESC shimDesc{};
                shimDesc.Width = static_cast<UINT>(kTexW);
                shimDesc.Height = static_cast<UINT>(kTexH);
                shimDesc.MipLevels = 1;
                shimDesc.ArraySize = 1;
                shimDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                shimDesc.SampleDesc.Count = 1;
                shimDesc.Usage = D3D11_USAGE_DEFAULT;
                shimDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
                shimDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                                      D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
                hr = m_myDevice->CreateTexture2D(
                    &shimDesc, nullptr, m_myShim.GetAddressOf());
                if (FAILED(hr)) {
                    Log(fmt::format(
                        "xr_telemetry: overlay disabled — CreateTexture2D "
                        "(shared shim) failed (HRESULT={:#x})\n",
                        static_cast<unsigned int>(hr)));
                    return false;
                }

                // 4. Get the shared NT handle from our shim.
                ComPtr<IDXGIResource1> myShimResource;
                if (FAILED(m_myShim.As(&myShimResource))) {
                    Log("xr_telemetry: overlay disabled — shim QueryInterface "
                        "IDXGIResource1 failed\n");
                    return false;
                }
                if (FAILED(myShimResource->CreateSharedHandle(
                        nullptr,
                        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                        nullptr,
                        &m_sharedHandle))) {
                    Log("xr_telemetry: overlay disabled — CreateSharedHandle "
                        "failed\n");
                    return false;
                }

                // 5. Open the shared handle on the app's device so we
                //    can CopyResource from it via the app's context.
                ComPtr<ID3D11Device1> appDevice1;
                if (FAILED(m_device.As(&appDevice1))) {
                    Log("xr_telemetry: overlay disabled — app device "
                        "QueryInterface ID3D11Device1 failed\n");
                    return false;
                }
                if (FAILED(appDevice1->OpenSharedResource1(
                        m_sharedHandle, __uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(m_appShim.GetAddressOf())))) {
                    Log("xr_telemetry: overlay disabled — OpenSharedResource1 "
                        "failed\n");
                    return false;
                }

                // 6. Cache the keyed-mutex interfaces on both sides.
                if (FAILED(m_myShim.As(&m_myShimMutex)) ||
                    FAILED(m_appShim.As(&m_appShimMutex))) {
                    Log("xr_telemetry: overlay disabled — keyed mutex "
                        "QueryInterface failed on shim\n");
                    return false;
                }

                // 7. Create the D2D render target on our shim (BGRA-
                //    capable device, no typeless surface, no E_INVALID
                //    ARG this time).
                ComPtr<IDXGISurface> shimSurface;
                if (FAILED(m_myShim.As(&shimSurface))) {
                    Log("xr_telemetry: overlay disabled — myShim QueryInterface "
                        "IDXGISurface failed\n");
                    return false;
                }
                D2D1_RENDER_TARGET_PROPERTIES props =
                    D2D1::RenderTargetProperties(
                        D2D1_RENDER_TARGET_TYPE_DEFAULT,
                        D2D1::PixelFormat(
                            DXGI_FORMAT_B8G8R8A8_UNORM,
                            D2D1_ALPHA_MODE_PREMULTIPLIED));
                hr = m_core.d2d()->CreateDxgiSurfaceRenderTarget(
                    shimSurface.Get(), &props,
                    m_myShimRenderTarget.GetAddressOf());
                if (FAILED(hr)) {
                    Log(fmt::format(
                        "xr_telemetry: overlay disabled — CreateDxgiSurfaceRender"
                        "Target (private-device shim) failed (HRESULT={:#x})\n",
                        static_cast<unsigned int>(hr)));
                    return false;
                }
                if (!m_core.initBrushes(m_myShimRenderTarget.Get())) {
                    Log("xr_telemetry: overlay disabled — initBrushes (private "
                        "device) failed\n");
                    return false;
                }

                // GPU histogram path. Capture m_myDevice's immediate
                // context (D2D doesn't expose it) and init the bar
                // renderer against the same shim the D2D RT paints
                // into. If anything here fails we DON'T disable the
                // overlay — m_useShaderBars stays false and the paint
                // path falls back to the full D2D paint(), so the HUD
                // still works, just without the GPU fast path.
                m_myDevice->GetImmediateContext(m_myContext.GetAddressOf());
                m_useShaderBars =
                    m_myContext &&
                    m_bars.init(m_myDevice.Get(), m_myContext.Get(),
                                 m_myShim.Get());
                if (!m_useShaderBars) {
                    Log("xr_telemetry: overlay GPU histogram path "
                        "unavailable — falling back to D2D bar rendering\n");
                }

                // Fill the immutable XrCompositionLayerQuad fields
                // now that m_swapchain is alive. SOURCE_ALPHA blending
                // + identity orientation are documented in the long
                // comment up in renderAndCompose; everything else
                // (space, pose.position, size) is per-frame.
                initQuadLayerConstants();

                Log(fmt::format(
                    "xr_telemetry: overlay D3D11 renderer ready ({} swapchain "
                    "images, private-device shim BGRA8 RT, feature_level={:#x})\n",
                    imgCount, static_cast<unsigned int>(outLevel)));
                return true;
            }

            // Shader-path paint is the free function paintShimViaShader()
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
                m_quadLayer.subImage.imageRect.extent = {kTexW, kTexH};
                m_quadLayer.subImage.imageArrayIndex = 0;
                m_quadLayer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            }

            OpenXrApi*                  m_api = nullptr;
            XrSession                   m_session = XR_NULL_HANDLE;
            // App's D3D11 device + context (whatever flags it was
            // created with — typically no BGRA_SUPPORT). Used only for
            // CopyResource'ing into the OpenXR swapchain image.
            ComPtr<ID3D11Device>        m_device;
            ComPtr<ID3D11DeviceContext> m_context;
            XrSwapchain                 m_swapchain = XR_NULL_HANDLE;
            // OpenXR swapchain images. Typically DXGI_FORMAT_B8G8R8A8_
            // TYPELESS on Pimax OpenXR 0.1.0; we never render directly
            // to these, only CopyResource into them from our shared
            // shim once the D2D paint is done.
            std::vector<ComPtr<ID3D11Texture2D>>   m_images;

            // Our private D3D11 device — same adapter as the app's,
            // but explicitly created with D3D11_CREATE_DEVICE_BGRA
            // _SUPPORT so D2D works. Holds the shim texture we paint.
            // The immediate context is deliberately not captured:
            // every draw goes through D2D's RT, never through a raw
            // ID3D11DeviceContext API we'd own.
            ComPtr<ID3D11Device>        m_myDevice;
            // The shim, twice. m_myShim lives on m_myDevice and is the
            // D2D render target. m_appShim is the SAME underlying
            // GPU resource, opened on the app's device via OpenShared
            // Resource1, so we can CopyResource from it on the app's
            // context.
            ComPtr<ID3D11Texture2D>     m_myShim;
            ComPtr<ID3D11Texture2D>     m_appShim;
            // Keyed mutex pair, one interface per side of the shim,
            // serialises paint vs copy. AcquireSync(key) waits for the
            // other side to ReleaseSync(key); the two sides toggle
            // between key=0 (app side owns) and key=1 (our side owns).
            ComPtr<IDXGIKeyedMutex>     m_myShimMutex;
            ComPtr<IDXGIKeyedMutex>     m_appShimMutex;
            HANDLE                      m_sharedHandle = nullptr;
            // D2D render target on m_myShim — the actual paint surface
            // for the static chrome.
            ComPtr<ID2D1RenderTarget>   m_myShimRenderTarget;
            // m_myDevice's immediate context. D2D drives its own RT
            // internally, but HistogramBarRenderer needs a raw context
            // to issue the instanced bar/quad draws into the shim after
            // the D2D chrome lands.
            ComPtr<ID3D11DeviceContext> m_myContext;
            // GPU histogram renderer: paints the histo region (bg +
            // grid + bars + budget) into m_myShim via instanced draws,
            // replacing the per-bar D2D FillRectangle loop. m_useShader
            // Bars gates whether it initialised; on failure the path
            // falls back to the full D2D paint().
            HistogramBarRenderer        m_bars;
            bool                        m_useShaderBars = false;
            // Chrome cadence for the shader path: the bars redraw every
            // frame on the GPU, the D2D chrome only when snap.version
            // ticks or the watchdog fires (kChromeWatchdogFrames lives
            // at namespace scope, shared with the D3D12 renderer).
            PaintCadence                m_barsCadence;

            CoreRenderer                m_core;
            HistogramRing<kRingSize>    m_cpuRing;
            HistogramRing<kRingSize>    m_gpuRing;
            XrCompositionLayerQuad      m_quadLayer{};
            bool                        m_ready = false;
            // Periodic-log counters for the bounded keyed-mutex
            // acquires. A sync glitch from the runtime / app side
            // is logged on first occurrence and then every Nth
            // recurrence (kMutexTimeoutLogStride) so a sustained
            // issue doesn't silently freeze the HUD for the rest
            // of the session — support can see "the timeouts kept
            // happening" instead of "1 timeout 4 hours ago".
            //
            // Also rolled up at destructor time so the session
            // summary records the total — useful when a user
            // submits a bug report after closing the game.
            static constexpr uint64_t   kMutexTimeoutLogStride = 1000;
            uint64_t                    m_myMutexTimeouts  = 0;
            uint64_t                    m_appMutexTimeouts = 0;
        };

        // -------- D3D12 renderer (via D3D11On12 bridge) ---------------------
        //
        // App uses D3D12. D2D is D3D11-only, so we wrap the app's D3D12
        // device into an ID3D11Device via D3D11On12CreateDevice. Each
        // swapchain image (ID3D12Resource*) is then bridged to an
        // ID3D11Resource via CreateWrappedResource — the wrapped
        // resource exposes a DXGI surface D2D can paint into.
        //
        // Per-frame dance:
        //   1. Acquire+wait the OpenXR swapchain image.
        //   2. D3D11On12::AcquireWrappedResources(wrapped) — flips the
        //      wrapped resource's state so D3D11 can write to it.
        //   3. D2D BeginDraw / DrawTextW / FillRectangle / EndDraw.
        //   4. D3D11On12::ReleaseWrappedResources(wrapped) — flips
        //      state back so D3D12 can read it for compositing.
        //   5. ID3D11DeviceContext::Flush() — pumps the D3D11On12
        //      command list into the underlying D3D12 queue.
        //   6. xrReleaseSwapchainImage.
        class D3D12OverlayRenderer final : public OverlayRenderer {
          public:
            D3D12OverlayRenderer(OpenXrApi* api, XrSession session,
                                  ID3D12Device* device,
                                  ID3D12CommandQueue* queue)
                : m_api(api), m_session(session),
                  m_d3d12Device(device), m_d3d12Queue(queue) {
                m_ready = init();
            }

            ~D3D12OverlayRenderer() override {
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
                XrSpace viewSpace,
                const OverlaySnapshot& snap,
                const std::string& position,
                float scale) override {
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

                // Paint into the typed BGRA8 shim we own (D2D refuses
                // the typeless surface the runtime exposes via the
                // wrapped resource). After paint, AcquireWrappedResources
                // around the CopyResource so the underlying D3D12
                // texture goes through the D3D11On12 state transition
                // correctly, then ReleaseWrappedResources hands control
                // back to the runtime's compositor.
                //
                // Shader path (m_useShaderBars): D2D chrome only when
                // stale + HistogramBarRenderer.drawPanel every frame,
                // both via the shared paintShimViaShader() helper. The
                // AcquireWrapped/CopyResource/Flush dance below is
                // unchanged; it ferries whatever the shim ended up with
                // into the D3D12 swapchain image.
                const bool painted = m_useShaderBars
                    ? paintShimViaShader(m_core, m_bars, m_barsCadence,
                                          m_shimRenderTarget.Get(),
                                          m_cpuRing, m_gpuRing, snap)
                    : m_core.paint(m_shimRenderTarget.Get(), snap,
                                    m_cpuRing, m_gpuRing);
                ID3D11Resource* wrapped = m_wrappedResources[imageIdx].Get();
                if (painted && wrapped && m_d3d11Context) {
                    m_d3d11On12->AcquireWrappedResources(&wrapped, 1);
                    m_d3d11Context->CopyResource(wrapped, m_shimTexture.Get());
                    m_d3d11On12->ReleaseWrappedResources(&wrapped, 1);
                }
                // Flush the D3D11On12 command list into the D3D12 queue
                // so the runtime sees the painted content when it
                // composites this frame.
                if (m_d3d11Context) m_d3d11Context->Flush();

                XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                m_api->xrReleaseSwapchainImage(m_swapchain, &releaseInfo);

                if (!painted) return nullptr;

                // Immutable quad fields filled once in init(); per-
                // frame writes only the view-space + the position/
                // size pair (see the D3D11 path's longer comment).
                const auto geo = geometryForPosition(position, scale);
                m_quadLayer.space = viewSpace;
                m_quadLayer.pose.position = {geo.pos_x, geo.pos_y, geo.pos_z};
                m_quadLayer.size = {geo.width_m, geo.height_m};

                return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_quadLayer);
            }

          private:
            bool init() {
                if (!m_d3d12Device || !m_d3d12Queue || !m_api ||
                    m_session == XR_NULL_HANDLE) return false;

                // 1. Wrap the D3D12 device via D3D11On12CreateDevice.
                //    The D3D11 device this returns shares the same
                //    underlying D3D12 device, so D2D's draws end up in
                //    the D3D12 queue's command stream.
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

                if (!m_core.init()) {
                    Log("xr_telemetry: overlay D2D/DirectWrite init failed; HUD disabled\n");
                    return false;
                }
                const int64_t format = pickSwapchainFormat(m_api, m_session);
                if (format == 0) {
                    Log("xr_telemetry: overlay disabled — runtime doesn't advertise "
                        "BGRA8 for the D3D12 swapchain\n");
                    return false;
                }

                XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                  XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                sci.format = format;
                sci.sampleCount = 1;
                sci.width = static_cast<uint32_t>(kTexW);
                sci.height = static_cast<uint32_t>(kTexH);
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

                // Create the shim texture on the BRIDGED D3D11 device
                // (not the app's D3D12 device — we paint with D2D, which
                // is D3D11-only). Same approach as the D3D11 path:
                // typed BGRA8 we own + CopyResource per frame to the
                // wrapped D3D12 image.
                D3D11_TEXTURE2D_DESC shimDesc{};
                shimDesc.Width = static_cast<UINT>(kTexW);
                shimDesc.Height = static_cast<UINT>(kTexH);
                shimDesc.MipLevels = 1;
                shimDesc.ArraySize = 1;
                shimDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                shimDesc.SampleDesc.Count = 1;
                shimDesc.Usage = D3D11_USAGE_DEFAULT;
                shimDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
                HRESULT hr = m_d3d11Device->CreateTexture2D(
                    &shimDesc, nullptr, m_shimTexture.GetAddressOf());
                if (FAILED(hr)) {
                    Log(fmt::format(
                        "xr_telemetry: overlay disabled — D3D12 path CreateTexture2D"
                        " (shim) failed (HRESULT={:#x})\n",
                        static_cast<unsigned int>(hr)));
                    return false;
                }
                ComPtr<IDXGISurface> shimSurface;
                if (FAILED(m_shimTexture.As(&shimSurface))) {
                    Log("xr_telemetry: overlay disabled — D3D12 path shim "
                        "QueryInterface IDXGISurface failed\n");
                    return false;
                }
                D2D1_RENDER_TARGET_PROPERTIES props =
                    D2D1::RenderTargetProperties(
                        D2D1_RENDER_TARGET_TYPE_DEFAULT,
                        D2D1::PixelFormat(
                            DXGI_FORMAT_B8G8R8A8_UNORM,
                            D2D1_ALPHA_MODE_PREMULTIPLIED));
                hr = m_core.d2d()->CreateDxgiSurfaceRenderTarget(
                    shimSurface.Get(), &props,
                    m_shimRenderTarget.GetAddressOf());
                if (FAILED(hr)) {
                    Log(fmt::format(
                        "xr_telemetry: overlay disabled — D3D12 path Create"
                        "DxgiSurfaceRenderTarget (shim) failed (HRESULT={:#x})\n",
                        static_cast<unsigned int>(hr)));
                    return false;
                }
                if (!m_core.initBrushes(m_shimRenderTarget.Get())) {
                    Log("xr_telemetry: overlay disabled — initBrushes (D3D12 "
                        "path) failed\n");
                    return false;
                }

                // GPU histogram path. The HistogramBarRenderer is the
                // same class the D3D11 path uses; it accepts any D3D11
                // device/context pair backing a BIND_RENDER_TARGET
                // texture, which here is the D3D11On12 bridge. On
                // failure we DON'T disable the overlay — m_useShader
                // Bars stays false and the paint path falls back to
                // m_core.paint() (full D2D), so the HUD still works.
                m_useShaderBars = m_bars.init(
                    m_d3d11Device.Get(), m_d3d11Context.Get(),
                    m_shimTexture.Get());
                if (!m_useShaderBars) {
                    Log("xr_telemetry: D3D12 overlay GPU histogram path "
                        "unavailable — falling back to D2D bar rendering\n");
                }

                // One-time fill of the immutable quad-layer fields;
                // mirror of the D3D11 path's helper (no need to share
                // since each renderer owns its own m_swapchain and
                // m_quadLayer).
                initQuadLayerConstants();

                Log("xr_telemetry: overlay D3D12 renderer ready ("
                    + std::to_string(imgCount) +
                    " swapchain images, D3D11On12 bridge + shim BGRA8 RT)\n");
                return true;
            }

            // Shader-path paint is the free function paintShimViaShader()
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
                m_quadLayer.subImage.imageRect.extent = {kTexW, kTexH};
                m_quadLayer.subImage.imageArrayIndex = 0;
                m_quadLayer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            }

            OpenXrApi*                            m_api = nullptr;
            XrSession                             m_session = XR_NULL_HANDLE;
            ComPtr<ID3D12Device>                  m_d3d12Device;
            ComPtr<ID3D12CommandQueue>            m_d3d12Queue;
            ComPtr<ID3D11Device>                  m_d3d11Device;
            ComPtr<ID3D11DeviceContext>           m_d3d11Context;
            ComPtr<ID3D11On12Device>              m_d3d11On12;
            XrSwapchain                           m_swapchain = XR_NULL_HANDLE;
            std::vector<ComPtr<ID3D11Resource>>   m_wrappedResources;
            // Shim — same role as in D3D11OverlayRenderer. Created on
            // the bridged D3D11 device (not the app's D3D12 device);
            // D2D paints into it, then CopyResource sends the result
            // to the wrapped D3D12 swapchain image each frame.
            ComPtr<ID3D11Texture2D>               m_shimTexture;
            ComPtr<ID2D1RenderTarget>             m_shimRenderTarget;
            // GPU histogram path. Same HistogramBarRenderer the D3D11
            // path uses (device-agnostic — it just needs an
            // ID3D11Device + context + a BIND_RENDER_TARGET texture).
            // Here the device IS the D3D11On12 bridge: our instanced
            // bar/quad draws sit on m_d3d11Context alongside the D2D
            // chrome, and the existing wrapped-resource CopyResource +
            // Flush dance in renderAndCompose ferries the result into
            // the D3D12 swapchain image. The D3D11On12 Flush() stays
            // (incompressible — that's the D3D12 queue handoff), so
            // the perf win is the D2D bar work moved to the GPU; the
            // bridge overhead is unchanged.
            HistogramBarRenderer                  m_bars;
            bool                                  m_useShaderBars = false;
            // Chrome cadence for the shader path — paints D2D only on
            // snap.version ticks or the watchdog. Same kChromeWatchdog
            // Frames constant as the D3D11 path, hoisted to namespace
            // scope so a tweak lands once.
            PaintCadence                          m_barsCadence;
            CoreRenderer                          m_core;
            HistogramRing<kRingSize>              m_cpuRing;
            HistogramRing<kRingSize>              m_gpuRing;
            XrCompositionLayerQuad                m_quadLayer{};
            bool                                  m_ready = false;
        };

    } // anonymous namespace

    // -------- Factory functions ----------------------------------------------

    std::unique_ptr<OverlayRenderer> makeD3D11OverlayRenderer(
        OpenXrApi* api, XrSession session, ID3D11Device* device) {
        auto r = std::make_unique<D3D11OverlayRenderer>(api, session, device);
        return r->isReady() ? std::unique_ptr<OverlayRenderer>(std::move(r))
                            : nullptr;
    }

    std::unique_ptr<OverlayRenderer> makeD3D12OverlayRenderer(
        OpenXrApi* api, XrSession session,
        ID3D12Device* device, ID3D12CommandQueue* queue) {
        auto r = std::make_unique<D3D12OverlayRenderer>(api, session, device, queue);
        return r->isReady() ? std::unique_ptr<OverlayRenderer>(std::move(r))
                            : nullptr;
    }

    // -------- Snapshot entry point ----------------------------------------
    //
    // One-shot render to an externally-owned ID2D1RenderTarget. Used by
    // the visual-regression snapshot tool — caller provides a WIC bitmap
    // render target, this function paints the HUD into it, caller saves
    // the bitmap to PNG. Reuses the same CoreRenderer as the in-engine
    // path, so the snapshot reflects EXACTLY what the user sees in the
    // headset (modulo font availability — see the bundled-Barlow
    // fallback in init()).
    //
    // Not suitable for per-frame use: every call allocates a fresh
    // CoreRenderer + its brushes / fonts. The cost (~5 ms of D2D/
    // DirectWrite init) is fine for a once-per-CI-run snapshot tool
    // but would be wasteful in production.
    bool renderOverlayToTarget(
        ID2D1RenderTarget* rt,
        const OverlaySnapshot& snap,
        const HistogramRing<kOverlayHistoRingSize>& cpuRing,
        const HistogramRing<kOverlayHistoRingSize>& gpuRing,
        std::string* errOut) {
        // CoreRenderer is in this TU's anonymous namespace (nested in
        // openxr_api_layer::detail), so the unqualified name resolves
        // here from the enclosing detail namespace.
        //
        // errOut is an optional diagnostic outparam used by the
        // snapshot test — when a step fails it captures which one,
        // since paint()'s EndDraw return value collapses every
        // possible draw-time queued error into a single bool. The
        // production callers leave it null.
        auto fail = [&](const char* msg) {
            if (errOut) *errOut = msg;
            return false;
        };
        if (!rt)                     return fail("rt is null");
        CoreRenderer core;
        if (!core.init())            return fail("core.init() failed (D2D / DWrite factory or text format creation)");
        if (!core.initBrushes(rt))   return fail("core.initBrushes(rt) failed (brush / gradient / stroke style creation)");
        if (!core.paint(rt, snap, cpuRing, gpuRing)) {
            return fail("core.paint(rt, ...) failed (EndDraw returned an HRESULT — usually a cross-factory or queued-draw error)");
        }
        return true;
    }

} // namespace openxr_api_layer::detail
