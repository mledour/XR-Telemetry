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
#include <cwchar>      // std::wcslen for the wide-literal degree-symbol path
#include <iterator>    // std::size for the dash-style array
#include <string>
#include <vector>

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

        // -------- Layout constants (fpsVR redesign, 720×480) ----------------
        //
        // Texture stays at this fixed size regardless of `scale` — the
        // QUAD in 3D scales, not the resolution. 720×480 (3:2) packs
        // the four sections of the redesigned HUD:
        //   - header bar (FPS / FPS AVG / P95 / P99, 4 cells)
        //   - GPU FRAMETIME MS panel with histogram + current value
        //   - CPU FRAMETIME MS panel with histogram + current value
        //   - bottom row split into two panels (TEMP & UTILISATION)
        //     each with chip + thermometer icons and a circular gauge
        //
        // The texture is shorter than the previous 720×540 (4:3) layout
        // because the frametime panels are now 90 px each (was 160→120
        // in earlier revisions). With less vertical real estate, the
        // histogram strip is ~54 px after the title row — short
        // enough that a normal frame at ~50 % budget fills most of
        // the strip visually, no empty top region.
        //
        // Vertical budget (top → bottom), all in pixels at native
        // texture resolution:
        //   kOuterPad           (10)
        //   kFrameStroke         (2)
        //   inner padding        (4)
        //   kHeaderHeight       (66)
        //   kSectionGap         (14)
        //   kFrametimeHeight    (90) — GPU panel
        //   kSectionGap         (14)
        //   kFrametimeHeight    (90) — CPU panel
        //   kSectionGap         (14)
        //   kBottomHeight      (130)
        //   inner padding        (4)
        //   kFrameStroke         (2)
        //   kOuterPad           (10)
        //   Total = 450, with ~30 px of breathing-room slack against
        //   the 480-px texture height.
        constexpr int32_t kTexW = 720;
        constexpr int32_t kTexH = 480;

        constexpr float kOuterPad       = 10.0f;
        constexpr float kFrameStroke    = 2.0f;
        constexpr float kSectionGap     = 14.0f;
        constexpr float kSectionInnerPad = 12.0f;  // padding INSIDE each panel

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
        constexpr float kBottomHeight     = 130.0f;  // tall enough for the chip + thermo
                                                      // + 22-px gauge font + label row

        // Histogram strip metrics — sits inside the frametime panel,
        // below the title row.
        constexpr float kHistoTitleH     = 24.0f;
        constexpr float kHistoBarGap     = 2.0f;
        // 120 bars × (~3.6 px each + 2 px gap) ≈ 670 px of strip width.
        // Roughly half the bar width of the previous version (~7 px) —
        // matches the denser, thinner-bar visual of the reference
        // design. Window covered = 120 samples ≈ 1.3 s @ 90 Hz / 1 s @
        // 120 Hz, still long enough to catch a stutter spike but short
        // enough that the histogram reacts within the same second.
        constexpr std::size_t kRingSize  = openxr_api_layer::detail::kOverlayHistoRingSize;
        static_assert(kRingSize == 120,
                       "kOverlayHistoRingSize must match the in-engine ring size; "
                       "bump both in lockstep if tuning the histogram length");

        // Font sizes — calibrated against the design spec's
        // hierarchy. Sizes in pixels at the 720×480 native texture
        // resolution; rendered to the head-locked quad they read at
        // their natural visual weight in the HMD.
        constexpr float kFontTinyLabel    = 17.0f;  // "FPS", "P95", "TEMP", "VRAM"
        constexpr float kFontSectionTitle = 22.0f;  // "GPU FRAMETIME MS"
        constexpr float kFontMs           = 26.0f;  // GPU panel "6.7 ms" current value
        constexpr float kFontMsCompound   = 18.0f;  // CPU panel compound string
                                                     // ("App ms X / Render ms Y") —
                                                     // smaller so the longer
                                                     // string still fits in the
                                                     // top-right region.
        constexpr float kFontBigNumber    = 52.0f;  // "142" FPS number — the
                                                     // single biggest text on
                                                     // the HUD, primary anchor.
        constexpr float kFontAccentNumber = 32.0f;  // "138", "124", "108", "98"
        constexpr float kFontTemp         = 43.0f;  // bottom panel TEMP / LOAD /
                                                     // VRAM values

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

                // Load the BUNDLED Rajdhani font (subset to ASCII +
                // °/µ/× glyphs). The TTF files live as RT_BUNDLED_FONT
                // resources in the DLL (see openxr-api-layer.rc.in);
                // we feed their bytes into an IDWriteInMemoryFontFile
                // Loader and build a custom IDWriteFontCollection
                // around them. The collection is then referenced by
                // family name ("Rajdhani") in every CreateTextFormat
                // call below — DirectWrite resolves through the
                // custom collection, never falling back to system
                // fonts.
                //
                // Why bundled: the design spec specifies Rajdhani,
                // and shipping the font as a 22 KB total resource
                // guarantees the HUD looks identical on every machine
                // (vs. relying on whatever condensed sans the system
                // happens to ship — Bahnschrift on Win10+, but a
                // different glyph shape on older versions).
                //
                // On failure (resource missing, factory QI fails,
                // collection creation fails), we proceed without the
                // custom collection and let CreateTextFormat fall
                // back to system default. Graceful degrade — never
                // crash the host process for a cosmetic font.
                const wchar_t* kFontFamily = L"Rajdhani";
                IDWriteFontCollection* customCollection = nullptr;
                if (!loadBundledFontCollection(m_dwriteFactory.Get(),
                                                 m_customFontCollection)) {
                    // Fallback: leave m_customFontCollection null and
                    // let DirectWrite use system fonts. Bahnschrift
                    // is the next-best match where available.
                    kFontFamily = L"Bahnschrift";
                } else {
                    customCollection = m_customFontCollection.Get();
                }

                // The redesign packs labels, big FPS numbers, accent
                // numbers, ms-suffix labels, temp readouts, and gauge
                // percentages — each at a different point size and
                // alignment. DirectWrite formats are immutable once
                // created (alignment changes require a new format);
                // we cache one format per (font, size, alignment)
                // combination at init time so paint() never allocates
                // a format.
                if (!makeFormat(kFontFamily, customCollection, kFontBigNumber,    DWRITE_FONT_WEIGHT_BOLD,
                                 DWRITE_TEXT_ALIGNMENT_CENTER, m_fmtBigNumber)) return false;
                if (!makeFormat(kFontFamily, customCollection, kFontAccentNumber, DWRITE_FONT_WEIGHT_BOLD,
                                 DWRITE_TEXT_ALIGNMENT_CENTER, m_fmtAccentNumber)) return false;
                // m_fmtTemp is CENTER-aligned: the bottom panel
                // stacks each value (TEMP / LOAD / VRAM) centred
                // under its column label.
                if (!makeFormat(kFontFamily, customCollection, kFontTemp,         DWRITE_FONT_WEIGHT_BOLD,
                                 DWRITE_TEXT_ALIGNMENT_CENTER, m_fmtTemp)) return false;
                if (!makeFormat(kFontFamily, customCollection, kFontMs,           DWRITE_FONT_WEIGHT_BOLD,
                                 DWRITE_TEXT_ALIGNMENT_TRAILING, m_fmtMsValue)) return false;
                if (!makeFormat(kFontFamily, customCollection, kFontMsCompound,   DWRITE_FONT_WEIGHT_BOLD,
                                 DWRITE_TEXT_ALIGNMENT_TRAILING, m_fmtMsCompound)) return false;
                if (!makeFormat(kFontFamily, customCollection, kFontTinyLabel,    DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                 DWRITE_TEXT_ALIGNMENT_CENTER, m_fmtTinyLabelCenter)) return false;
                if (!makeFormat(kFontFamily, customCollection, kFontSectionTitle, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                 DWRITE_TEXT_ALIGNMENT_LEADING, m_fmtSectionTitle)) return false;
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
                // Bevel highlight (lighter, top edge of each panel)
                // and shadow (darker, bottom edge). 1-px lines at
                // these colours give the "raised metal" impression
                // around every panel rim — see drawPanelBg.
                if (!make(D2D1::ColorF(0.322f, 0.349f, 0.361f, 1.00f), m_brushBevelHighlight)) return false; // #52595C
                if (!make(D2D1::ColorF(0.110f, 0.122f, 0.125f, 1.00f), m_brushBevelShadow)) return false;    // #1C1F20
                // Carbon-fibre hatch — drawn as low-alpha diagonal
                // lines across the panel backgrounds. ~6 % white so
                // the texture is just visible without becoming
                // visible stripes.
                if (!make(D2D1::ColorF(1.000f, 1.000f, 1.000f, 0.06f), m_brushCarbonHatch)) return false;
                // Text — slightly cooler white than before. Matches
                // the spec's #EBF0F2 (was #F2F4F5).
                if (!make(D2D1::ColorF(0.922f, 0.941f, 0.949f, 1.00f), m_brushTextWhite)) return false;  // #EBF0F2
                if (!make(D2D1::ColorF(0.620f, 0.659f, 0.671f, 1.00f), m_brushTextLabel)) return false;  // #9EA8AB
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
            //   - Outer frame: dark grey 2-px stroke around a slightly
            //     lighter dark-blue background. 10-px padding to the
            //     texture edge so the OpenXR runtime's bilinear filter
            //     doesn't soften the corner anti-alias.
            //   - Header bar: 4 cells (FPS / FPS AVG / P95 / P99),
            //     thin vertical separators. Big white FPS number,
            //     cyan accent numbers on the right three cells.
            //   - GPU FRAMETIME MS panel: title + current value
            //     (top-right) + dashed grid + teal histogram.
            //   - CPU FRAMETIME MS panel: same, light-blue bars.
            //   - Bottom row (2 panels):
            //       GPU TEMP & UTILISATION — chip icon, thermometer +
            //         "TEMP 67 °C", "GPU UTIL" + red circular gauge.
            //       CPU TEMP & UTILISATION — same, "-- °C" until
            //         PawnIO lands, cyan gauge.
            //
            // The cached display values are re-formatted only when the
            // snapshot's `version` changes (the aggregator publishes
            // ~10×/s; paint() runs at the host's 90-144 Hz frame rate,
            // so the cache saves ~13 redundant snprintf passes per
            // frame in the steady state).
            bool paint(ID2D1RenderTarget* rt,
                       const OverlaySnapshot& snap,
                       const HistogramRing<kRingSize>& cpuRing,
                       const HistogramRing<kRingSize>& gpuRing) {
                if (!rt) return false;
                if (!m_brushBg || !m_strokeDashed) return false;  // brushes not init'd

                rt->BeginDraw();
                // Fully transparent clear — we paint our own opaque
                // panel backgrounds, so the corners outside the frame
                // composite through to the game underneath as
                // transparent pixels.
                rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

                // Re-format display values on snapshot version change.
                if (snap.version != m_cachedSnapshotVersion) {
                    m_cachedValues = formatOverlayDisplayValues(snap);
                    m_cachedSnapshotVersion = snap.version;
                }
                const auto& v = m_cachedValues;

                const float texW = static_cast<float>(kTexW);
                const float texH = static_cast<float>(kTexH);

                // Outer frame: octagonal-ish shape with chamfered
                // corners — matches the design spec's "cut corners"
                // industrial-HUD look. 12-px diagonal cut at each
                // corner; the 4 sides connect with straight edges.
                // Built once per paint via ID2D1PathGeometry (8
                // line segments). The geometry is closed so a single
                // FillGeometry / DrawGeometry pair handles both the
                // background fill and the metal-line stroke.
                const D2D1_RECT_F frameOuter = D2D1::RectF(
                    kOuterPad, kOuterPad,
                    texW - kOuterPad, texH - kOuterPad);
                drawChamferedRect(rt, frameOuter, 12.0f,
                                   m_brushBg.Get(),
                                   m_brushFrameLine.Get(), kFrameStroke);

                // Inner content rectangle (everything sits inside this).
                const float innerL = kOuterPad + kFrameStroke + 4.0f;
                const float innerR = texW - kOuterPad - kFrameStroke - 4.0f;
                const float innerT = kOuterPad + kFrameStroke + 4.0f;
                const float innerB = texH - kOuterPad - kFrameStroke - 4.0f;

                // Section Y positions, top → bottom.
                const float headerY    = innerT;
                const float gpuPanelY  = headerY + kHeaderHeight + kSectionGap;
                const float cpuPanelY  = gpuPanelY + kFrametimeHeight + kSectionGap;
                const float bottomY    = cpuPanelY + kFrametimeHeight + kSectionGap;
                (void)innerB;  // bottomY + kBottomHeight ≤ innerB by construction

                drawHeaderBar(rt, innerL, headerY, innerR,
                               headerY + kHeaderHeight, v);
                // GPU panel: single value top-right ("6.7 ms").
                // CPU panel: dual value "App ms X.X ms / Render ms Y.Y
                // ms" — the App / Render split is the diagnostic
                // value of the design (see comment in
                // overlay_aggregator.h for what each metric covers).
                // An empty secondary string suppresses the App / Render
                // composition and falls back to the single-value
                // rendering — same code path as the GPU panel.
                drawFrametimePanel(rt, innerL, gpuPanelY, innerR,
                                    gpuPanelY + kFrametimeHeight,
                                    L"GPU FRAMETIME MS",
                                    v.gpu_frametime_ms,
                                    /*secondaryValue=*/std::string{},
                                    gpuRing, snap.target_fps,
                                    m_brushGpuTealGrad.Get());
                drawFrametimePanel(rt, innerL, cpuPanelY, innerR,
                                    cpuPanelY + kFrametimeHeight,
                                    L"CPU FRAMETIME MS",
                                    v.cpu_frametime_ms,
                                    v.cpu_app_ms,
                                    cpuRing, snap.target_fps,
                                    m_brushCpuBlueGrad.Get());

                // Bottom row: 60/40 split between the GPU and CPU
                // panels — GPU gets 3 cells (TEMP / LOAD / VRAM),
                // CPU gets 2 cells (TEMP / LOAD). With 5 equal cells
                // across the row this gives every cell the same
                // pixel width (≈ 135 px), so the labels and values
                // align perfectly across both panels.
                //
                // VRAM-cell tier-colour (cyan / orange / red) follows
                // the same gaugeTierForUtilisation thresholds as
                // GPU LOAD / CPU LOAD: < 80 % = cyan default, 80–89 %
                // = orange, ≥ 90 % = red. Coherent palette = the
                // user reads "anything orange / red = headroom
                // problem" without per-cell legends.
                const float bottomCellW = (innerR - innerL - kSectionGap) / 5.0f;
                const float gpuPanelL   = innerL;
                const float gpuPanelR   = gpuPanelL + 3.0f * bottomCellW;
                const float cpuPanelL   = gpuPanelR + kSectionGap;
                const float cpuPanelR   = innerR;
                drawBottomPanel(rt, gpuPanelL, bottomY,
                                 gpuPanelR, bottomY + kBottomHeight,
                                 L"GPU",
                                 v.gpu_temp_c, v.gpu_util_pct,
                                 v.gpu_util_fraction,
                                 /*vramValue=*/v.vram_pct,
                                 /*vramFraction=*/v.vram_fraction);
                drawBottomPanel(rt, cpuPanelL, bottomY,
                                 cpuPanelR, bottomY + kBottomHeight,
                                 L"CPU",
                                 v.cpu_temp_c, v.cpu_util_pct,
                                 v.cpu_util_fraction,
                                 /*vramValue=*/std::string{},
                                 /*vramFraction=*/0.0f);

                return SUCCEEDED(rt->EndDraw());
            }

          private:
            // -------- Format / brush helpers --------------------------------
            //
            // makeFormat takes an optional IDWriteFontCollection — non-null
            // when the renderer loaded the bundled Rajdhani fonts, null when
            // we fell back to system fonts. CreateTextFormat resolves the
            // family name through the supplied collection (or the system
            // collection on null).
            bool makeFormat(const wchar_t* family,
                            IDWriteFontCollection* collection,
                            float size,
                            DWRITE_FONT_WEIGHT weight,
                            DWRITE_TEXT_ALIGNMENT alignment,
                            ComPtr<IDWriteTextFormat>& out) {
                if (FAILED(m_dwriteFactory->CreateTextFormat(
                        family, collection, weight,
                        DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,
                        size, L"en-US", out.GetAddressOf()))) {
                    return false;
                }
                out->SetTextAlignment(alignment);
                out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                return true;
            }

            // Loads the bundled Rajdhani font resources (RT_BUNDLED_FONT,
            // IDs 200 + 201 in openxr-api-layer.rc.in) into a custom
            // IDWriteFontCollection. Uses IDWriteFactory5's in-memory
            // font-file-loader API (Windows 10 1703+) to avoid the
            // boilerplate of implementing IDWriteFontFileLoader /
            // IDWriteFontFileStream / IDWriteFontCollectionLoader by hand.
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
                    // the file (single face per file for our
                    // Rajdhani subsets, but the API is robust
                    // either way).
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

            // -------- Header bar --------------------------------------------
            //
            // Layout: 5 cells of equal width separated by 1-px vertical
            // bars. Each cell has a small uppercase label at the top
            // and a big number below. The FPS cell uses the white
            // brush; the four accent cells (AVG / P95 / P99 / P99.9)
            // use cyan.
            //
            // Cell width = 688 / 5 ≈ 137 px on the 720-wide texture.
            // Bahnschrift Bold 42 px on "142" measures ~70 px (the
            // font is condensed so 3-digit numbers stay narrow);
            // labels at 14 px SemiBold stay well under 50 px even
            // for "P99.9". Comfortable margins.
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
                        D2D1::Point2F(x, t + 8.0f),
                        D2D1::Point2F(x, b - 8.0f),
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
                          labelRect, m_brushTextLabel.Get());
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
            void drawFrametimePanel(ID2D1RenderTarget* rt, float l, float t,
                                     float r, float b,
                                     const wchar_t* title,
                                     const std::string& currentValue,
                                     const std::string& secondaryValue,
                                     const HistogramRing<kRingSize>& ring,
                                     float targetFps,
                                     ID2D1Brush* barBrush) const {
                drawPanelBg(rt, l, t, r, b);

                // Title bar — top inner padding.
                const float titleT = t + 6.0f;
                const float titleB = titleT + kHistoTitleH;
                const D2D1_RECT_F titleRect = D2D1::RectF(
                    l + kSectionInnerPad, titleT,
                    r - kSectionInnerPad, titleB);
                drawWide(rt, title, m_fmtSectionTitle.Get(), titleRect,
                          m_brushTextLabel.Get());

                // Current value (top-right). Two shapes depending on
                // whether `secondaryValue` is empty:
                //
                //   GPU panel (secondaryValue == "")   : "6.7 ms"
                //   CPU panel (secondaryValue == "4.3"): "App ms 4.3 ms / Render ms 7.4 ms"
                //
                // Both render as a SINGLE right-aligned trailing-
                // aligned string in m_fmtMsValue (Bahnschrift Bold
                // 22 px).
                // Drawing the labels and values in separate rects /
                // colours was tried in an earlier revision and caused
                // visible baseline mismatch between the two panels —
                // the single-string approach keeps right edges and
                // baselines identical across GPU / CPU and across
                // resize events. The colour split between "labels"
                // and "values" is sacrificed for layout robustness;
                // can be revisited via IDWriteTextLayout if the
                // visual hierarchy needs more separation.
                std::string topRightStr;
                float valueRectWidth;
                IDWriteTextFormat* topRightFmt;
                if (secondaryValue.empty()) {
                    // GPU panel: short "6.7 ms" at the larger
                    // kFontMs (26 px) — primary frametime read-out.
                    topRightStr = currentValue + " ms";
                    valueRectWidth = 160.0f;
                    topRightFmt = m_fmtMsValue.Get();
                } else {
                    // CPU panel: compound "App ms X / Render ms Y"
                    // at the smaller kFontMsCompound (18 px). The
                    // string is ~32 chars so the size drop keeps
                    // it inside the panel without crowding the
                    // section title on the left. Both labels and
                    // values render in cyan accent — colour-coded
                    // sub-grouping deferred (would need multi-run
                    // text layout, see earlier commit's caveat).
                    topRightStr = "App ms " + secondaryValue +
                                   " ms / Render ms " + currentValue + " ms";
                    valueRectWidth = 360.0f;
                    topRightFmt = m_fmtMsCompound.Get();
                }
                const D2D1_RECT_F valueRect = D2D1::RectF(
                    r - kSectionInnerPad - valueRectWidth, titleT - 4.0f,
                    r - kSectionInnerPad,                  titleB + 6.0f);
                drawAscii(rt, topRightStr, topRightFmt, valueRect,
                           m_brushAccentCyan.Get());

                // Histogram region — below the title row, with inner
                // padding on all sides.
                const float histoL = l + kSectionInnerPad;
                const float histoR = r - kSectionInnerPad;
                const float histoT = titleB + 6.0f;
                const float histoB = b - kSectionInnerPad;

                drawDashedGrid(rt, histoL, histoT, histoR, histoB,
                                /*lineCount=*/4);

                const int64_t budgetNs = targetFps > 0.0f
                    ? static_cast<int64_t>(1.0e9f / targetFps)
                    : 0;
                drawHistogramBars(rt, ring, budgetNs,
                                   histoL, histoT, histoR, histoB,
                                   barBrush);

                // Budget reference line — drawn AFTER the bars so
                // bars that cross it (overruns ≥ budget) visually
                // overlap the line and the user sees the breach.
                // Position from the top: budgetLineFraction (= 1/6).
                // Brighter than the dashed grid so it reads as the
                // "this is your budget" marker rather than just one
                // more grid line.
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
                                    ID2D1Brush* accentBrush) const {
                if (r <= l || b <= t || budgetNs <= 0) return;
                const std::size_t n = ring.size();
                if (n == 0) return;
                const float fullW = r - l;
                const float stripH = b - t;
                const float barW =
                    (fullW - kHistoBarGap * static_cast<float>(n - 1)) /
                    static_cast<float>(n);
                if (barW <= 0.0f) return;

                // If the caller passed a linear-gradient brush
                // (m_brushGpuTealGrad / m_brushCpuBlueGrad), update
                // its endpoints to span the strip's vertical range
                // before drawing. Each bar then samples the gradient
                // from its own position within the strip — a bar
                // that only reaches halfway up gets only the bottom
                // half of the gradient, matching how the spec's
                // mock-up renders.
                {
                    ComPtr<ID2D1LinearGradientBrush> grad;
                    if (SUCCEEDED(accentBrush->QueryInterface(
                            __uuidof(ID2D1LinearGradientBrush),
                            reinterpret_cast<void**>(grad.GetAddressOf())))) {
                        grad->SetStartPoint(D2D1::Point2F((l + r) * 0.5f, t));
                        grad->SetEndPoint(D2D1::Point2F((l + r) * 0.5f, b));
                    }
                }

                constexpr float kDashPlaceholderH = 2.0f;
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
                                  const wchar_t* prefix,
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
                        D2D1::Point2F(x, t + 14.0f),
                        D2D1::Point2F(x, b - 14.0f),
                        m_brushSeparator.Get(), 1.0f);
                }

                // Build the per-column labels. TEMP and LOAD include
                // the panel prefix ("GPU TEMP" / "CPU LOAD"); VRAM is
                // a singleton label ("VRAM") because it's
                // implicitly GPU-side — no "GPU VRAM" because that
                // would be redundant and use a full extra word of
                // horizontal space.
                std::wstring tempLabel = std::wstring(prefix) + L" TEMP";
                std::wstring loadLabel = std::wstring(prefix) + L" LOAD";

                // Vertical positions inside the panel:
                //   labelY : small label (m_fmtTinyLabelCenter, 14 px)
                //   valueY : big value (m_fmtTemp, 30 px) — extends
                //            down to the panel bottom and paragraph-
                //            centres for the "label above / value
                //            below" stack the design asks for.
                const float labelY = t + (b - t) * 0.18f;
                const float valueY = t + (b - t) * 0.40f;

                auto drawCell = [&](float cellL, float cellR,
                                      const wchar_t* label,
                                      const std::string& asciiValue,
                                      const wchar_t* unitSuffix,
                                      ID2D1Brush* valueBrush,
                                      bool useWideValue) {
                    drawWide(rt, label, m_fmtTinyLabelCenter.Get(),
                              D2D1::RectF(cellL, labelY, cellR,
                                           labelY + 22.0f),
                              m_brushTextLabel.Get());
                    if (useWideValue) {
                        // For the °C suffix the whole string must be
                        // wide. tempValue is ASCII, so byte-widening
                        // it and concatenating L" °C" works.
                        std::wstring wide(asciiValue.begin(),
                                           asciiValue.end());
                        wide += unitSuffix;
                        drawWide(rt, wide.c_str(), m_fmtTemp.Get(),
                                  D2D1::RectF(cellL, valueY, cellR, b - 8.0f),
                                  valueBrush);
                    } else {
                        // % and other ASCII-safe suffixes — single
                        // ASCII drawAscii call, no wide conversion.
                        std::string full = asciiValue;
                        // Append the suffix as narrow chars (we
                        // already know it's ASCII by contract here).
                        for (const wchar_t* p = unitSuffix; *p; ++p) {
                            full.push_back(static_cast<char>(*p));
                        }
                        drawAscii(rt, full, m_fmtTemp.Get(),
                                   D2D1::RectF(cellL, valueY, cellR, b - 8.0f),
                                   valueBrush);
                    }
                };

                // Cell 0 — <prefix> TEMP / "67 °C"
                drawCell(l, l + colW,
                          tempLabel.c_str(), tempValue, L" °C",
                          m_brushTextWhite.Get(), /*useWideValue=*/true);
                // Cell 1 — <prefix> LOAD / "92 %"
                drawCell(l + colW, l + colW * 2.0f,
                          loadLabel.c_str(), utilValue, L" %",
                          tierBrush(utilFraction), /*useWideValue=*/false);
                // Cell 2 — VRAM / "76 %" (GPU panel only)
                if (hasVram) {
                    drawCell(l + colW * 2.0f, r,
                              L"VRAM", vramValue, L" %",
                              tierBrush(vramFraction), /*useWideValue=*/false);
                }
            }

            // Panel background — slightly raised dark-blue panel with
            // a 1-px separator stroke on the inside. Used for the
            // header, both frametime panels, and the bottom row pair.
            // Adds a top-edge bevel highlight (lighter) and a
            // bottom-edge bevel shadow (darker) so each panel reads
            // as a slightly raised metal surface, matching the
            // design spec's industrial-HUD look. The corner-radius
            // strokes are subtle enough that a straight horizontal
            // line crossing the panel doesn't break visually.
            void drawPanelBg(ID2D1RenderTarget* rt, float l, float t,
                              float r, float b) const {
                const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
                    D2D1::RectF(l, t, r, b), 4.0f, 4.0f);
                rt->FillRoundedRectangle(panel, m_brushPanelBg.Get());

                // Subtle carbon-fibre-ish diagonal hatch: thin
                // diagonal lines at ~6 % alpha across the panel
                // body. Spaced 6 px apart; at the design's
                // resolution they read as a very faint texture
                // hint, never as actual stripes. Drawn BEFORE the
                // panel separator stroke so the stroke covers the
                // line ends at the panel edge.
                drawCarbonHatch(rt, l, t, r, b);

                rt->DrawRoundedRectangle(panel, m_brushSeparator.Get(), 1.0f);
                // Bevel highlight (top edge) and shadow (bottom
                // edge). Inset by 1.5 px from the panel border so
                // the bevel lines sit JUST inside the separator
                // stroke without overlapping it.
                const float bevelInset = 1.5f;
                rt->DrawLine(
                    D2D1::Point2F(l + 6.0f, t + bevelInset),
                    D2D1::Point2F(r - 6.0f, t + bevelInset),
                    m_brushBevelHighlight.Get(), 1.0f);
                rt->DrawLine(
                    D2D1::Point2F(l + 6.0f, b - bevelInset),
                    D2D1::Point2F(r - 6.0f, b - bevelInset),
                    m_brushBevelShadow.Get(), 1.0f);
            }

            // Carbon-fibre hatch — diagonal 45° lines spanning the
            // panel, at very low alpha so they read as a subtle
            // texture rather than as visible stripes. The lines are
            // clipped to the panel's rounded-rect bounds by axis-
            // aligned clipping — close enough for the visual intent
            // at this opacity.
            //
            // Cost: ~(w + h) / spacing DrawLine calls per panel.
            // For a 280-px-wide × 90-px-tall frametime panel at
            // 6 px spacing that's ~62 lines. At 4-5 panels per
            // frame and 144 Hz that's ~45k DrawLine/s — well within
            // D2D's budget for a 720×480 target. Could be cached to
            // an offscreen bitmap brush later if it ever shows up
            // in a profile.
            void drawCarbonHatch(ID2D1RenderTarget* rt, float l, float t,
                                   float r, float b) const {
                constexpr float kSpacing = 6.0f;
                // Clip rect: panel inner area. Slight inset so the
                // hatch doesn't bleed into the rounded corners.
                D2D1_RECT_F clip = D2D1::RectF(l + 1.0f, t + 1.0f,
                                                r - 1.0f, b - 1.0f);
                rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                // Diagonal lines going down-right. We need to cover
                // the panel from its top-right corner to its bottom-
                // left corner, so the start positions range from
                // (l - panel_h, t) to (r, t). The diagonal length is
                // bounded by the panel diagonal.
                const float panelH = b - t;
                const float startX0 = l - panelH;
                const float endX0   = r;
                for (float x = startX0; x < endX0; x += kSpacing) {
                    rt->DrawLine(
                        D2D1::Point2F(x, t),
                        D2D1::Point2F(x + panelH, b),
                        m_brushCarbonHatch.Get(), 0.6f);
                }
                rt->PopAxisAlignedClip();
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
            ComPtr<IDWriteTextFormat> m_fmtMsValue;          // "6.7 ms" (GPU, 26 px)
            ComPtr<IDWriteTextFormat> m_fmtMsCompound;       // "App ms ... / Render ms ..." (CPU, 18 px)
            ComPtr<IDWriteTextFormat> m_fmtTinyLabelCenter;  // "FPS", "GPU TEMP", "GPU LOAD"
            ComPtr<IDWriteTextFormat> m_fmtSectionTitle;     // "GPU FRAMETIME MS"

            // Brushes — the 12-colour palette.
            ComPtr<ID2D1SolidColorBrush> m_brushBg;
            ComPtr<ID2D1SolidColorBrush> m_brushPanelBg;
            ComPtr<ID2D1SolidColorBrush> m_brushFrameLine;
            ComPtr<ID2D1SolidColorBrush> m_brushSeparator;
            ComPtr<ID2D1SolidColorBrush> m_brushBevelHighlight;
            ComPtr<ID2D1SolidColorBrush> m_brushBevelShadow;
            ComPtr<ID2D1SolidColorBrush> m_brushCarbonHatch;
            ComPtr<ID2D1SolidColorBrush> m_brushTextWhite;
            ComPtr<ID2D1SolidColorBrush> m_brushTextLabel;
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

            // Formatting cache. paint() refreshes the strings only
            // when m_cachedSnapshotVersion != snap.version — typically
            // ~10× per second on a 144 Hz HMD, not 144×. Not `mutable`
            // because paint() is already non-const (it mutates this
            // cache by design).
            OverlayDisplayValues m_cachedValues;
            uint64_t             m_cachedSnapshotVersion = 0;
        };

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
                        painted = m_core.paint(m_myShimRenderTarget.Get(), snap,
                                                m_cpuRing, m_gpuRing);
                        m_myShimMutex->ReleaseSync(1);
                        weHaveKey1 = true;
                    } else if (!m_loggedMyMutexTimeout) {
                        // One-shot log: enough to point a support
                        // report at the symptom without spamming the
                        // file when a recurring sync hiccup happens.
                        m_loggedMyMutexTimeout = true;
                        Log(fmt::format(
                            "xr_telemetry: overlay paint mutex acquire timed "
                            "out (HRESULT={:#x}, key=0). HUD will skip frames "
                            "until sync recovers; subsequent timeouts silenced.\n",
                            static_cast<unsigned int>(hr)));
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
                        } else if (!m_loggedAppMutexTimeout) {
                            m_loggedAppMutexTimeout = true;
                            Log(fmt::format(
                                "xr_telemetry: overlay copy mutex acquire timed "
                                "out (HRESULT={:#x}, key=1). HUD will skip frames "
                                "until sync recovers; subsequent timeouts silenced.\n",
                                static_cast<unsigned int>(hr)));
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
            // D2D render target on m_myShim — the actual paint surface.
            ComPtr<ID2D1RenderTarget>   m_myShimRenderTarget;

            CoreRenderer                m_core;
            HistogramRing<kRingSize>    m_cpuRing;
            HistogramRing<kRingSize>    m_gpuRing;
            XrCompositionLayerQuad      m_quadLayer{};
            bool                        m_ready = false;
            // One-shot guards for the bounded keyed-mutex acquires —
            // a sync glitch from the runtime / app side is logged
            // once, then suppressed so a sustained issue doesn't
            // flood the file. Cleared at construction.
            bool                        m_loggedMyMutexTimeout = false;
            bool                        m_loggedAppMutexTimeout = false;
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
                const bool painted =
                    m_core.paint(m_shimRenderTarget.Get(), snap,
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
    // headset (modulo font availability — see the bundled-Rajdhani
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
