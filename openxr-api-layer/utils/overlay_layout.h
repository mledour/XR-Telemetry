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

#pragma once

// =============================================================================
// overlay_layout.h — pure helpers shared by overlay_renderer.cpp:
//   * formatOverlayDisplayValues: snapshot → POD of pre-formatted strings +
//     numeric values, one entry per cell of the new fpsVR-redesign HUD
//     (header bar with FPS/AVG/P95/P99, two frametime panels with current-
//     value labels, bottom row with TEMP / UTIL cells per side).
//   * geometryForPosition: settings.position string → quad pose constants
//     for the 4:3 720×540 texture layout.
//   * barVisualForSample / budgetLineFraction: per-bar colour + height
//     decisions for the histogram panels (unchanged contract).
//
// Stays self-contained — no OpenXR / D3D / DirectWrite headers. Lets the
// test binary exercise the formatting and geometry math on macOS / Linux
// without the Windows SDK stack.
// =============================================================================

#include "overlay_aggregator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace openxr_api_layer::detail {

    // Snapshot → pre-formatted strings for the new fpsVR-redesign HUD.
    // The renderer never calls snprintf at draw time — it only paints
    // pre-formatted cells. Numeric fields (the *_fraction members) are
    // exposed alongside the string versions so the circular gauges can
    // draw the arc geometry directly without re-parsing the percentage
    // text.
    //
    // Empty / default-constructed when the snapshot is still invalid
    // (caller skips text drawing in that case).
    //
    // Field naming mirrors the visible cell positions:
    //
    //   ── header bar (single row, 5 cells) ─────────────────────────
    //   FPS 142  FPS AVG 138  P95 124  P99 108  P99.9 98
    //   fps_instant fps_avg   fps_p95  fps_p99  fps_p99_9
    //
    //   ── GPU frametime panel ───────────────────────────────────────
    //   GPU FRAMETIME MS                          6.7 ms
    //                                     gpu_frametime_ms
    //   <histogram >
    //
    //   ── CPU frametime panel ───────────────────────────────────────
    //   CPU FRAMETIME MS                          7.4 ms
    //                                     cpu_frametime_ms
    //   <histogram>
    //
    //   ── bottom row (2 panels side-by-side) ────────────────────────
    //   GPU TEMP & UTILISATION             CPU TEMP & UTILISATION
    //   TEMP 67 °C   GPU UTIL [ 92% ]       TEMP -- °C   CPU UTIL [ 78% ]
    //   gpu_temp_c     gpu_util_pct          cpu_temp_c    cpu_util_pct
    //                  gpu_util_fraction                   cpu_util_fraction
    //
    // The cpu_temp_c cell always renders "--" until PawnIO support
    // lands in a follow-up PR (no anti-cheat-safe CPU temp source for
    // an in-process layer otherwise — see the project history for the
    // CPU-temp / WinRing0 / PawnIO analysis).
    struct OverlayDisplayValues {
        // Header bar
        std::string fps_instant      = "--";  // big white number
        std::string fps_avg          = "--";  // cyan accent
        std::string fps_p95          = "--";  // cyan accent
        std::string fps_p99          = "--";  // cyan accent
        std::string fps_p99_9        = "--";  // cyan accent — single
                                                // worst ~0.1 % of frames
                                                // over a 30 s sliding
                                                // window (see aggregator
                                                // for the window math)

        // Frametime panel current-value labels (top-right of each
        // panel — the histogram itself takes the rest of the space).
        std::string gpu_frametime_ms = "--.-";
        std::string cpu_frametime_ms = "--.-";

        // Bottom row — temperatures (integer °C, "--" sentinel when
        // source absent).
        std::string gpu_temp_c       = "--";
        std::string cpu_temp_c       = "--";   // always "--" until PawnIO

        // Bottom row — utilisation percentages (drawn both as text
        // inside the gauge AND geometrically as an arc filling 0..N%
        // of a 270° dial).
        std::string gpu_util_pct     = "--";
        std::string cpu_util_pct     = "--";

        // Gauge fractions (0..1) for the circular dial. Synced to the
        // *_util_pct strings above so the dial fill matches what the
        // text reads.
        float       gpu_util_fraction = 0.0f;
        float       cpu_util_fraction = 0.0f;

        // True when at least the FPS field carries a real value. The
        // renderer can skip the text/gauge passes (but still paint the
        // frame/background) on the very first ~100 ms of a session
        // before the aggregator publishes the first snapshot.
        bool        valid = false;
    };

    inline OverlayDisplayValues formatOverlayDisplayValues(const OverlaySnapshot& snap) {
        OverlayDisplayValues v;
        if (!snap.valid) return v;
        v.valid = true;

        // The aggregator guards against div-by-zero already, but a
        // momentarily-negative `frame_total - wait_block` can
        // accumulate into NaN / ±Inf. Every formatter here checks
        // std::isfinite() and substitutes "--" / "--.-" so the cells
        // stay at their fixed widths even when the input is briefly
        // garbage. The renderer relies on these fixed widths for
        // pixel-alignment of the right-side accent numbers in the
        // header bar — letting "nan" / "inf" leak through would
        // shift the right column rightward and break the grid.
        auto fmtFpsInt = [](float fps) {
            char buf[16];
            if (std::isfinite(fps) && fps >= 0.0f) {
                std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(fps)));
            } else {
                std::snprintf(buf, sizeof(buf), "--");
            }
            return std::string(buf);
        };
        auto fmtMsOneDecimal = [](float ms) {
            char buf[16];
            if (std::isfinite(ms) && ms >= 0.0f) {
                std::snprintf(buf, sizeof(buf), "%.1f", ms);
            } else {
                std::snprintf(buf, sizeof(buf), "--.-");
            }
            return std::string(buf);
        };
        auto fmtTempInt = [](float c) {
            char buf[16];
            if (std::isfinite(c) && c >= -50.0f && c <= 200.0f) {
                std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(c)));
            } else {
                std::snprintf(buf, sizeof(buf), "--");
            }
            return std::string(buf);
        };
        auto fmtPctInt = [](float p) {
            char buf[16];
            if (std::isfinite(p) && p >= 0.0f) {
                std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(p)));
            } else {
                std::snprintf(buf, sizeof(buf), "--");
            }
            return std::string(buf);
        };

        v.fps_instant      = fmtFpsInt(snap.fps_instant);
        v.fps_avg          = fmtFpsInt(snap.fps_avg);
        v.fps_p95          = fmtFpsInt(snap.fps_p95);
        v.fps_p99          = fmtFpsInt(snap.fps_p99);
        v.fps_p99_9        = fmtFpsInt(snap.fps_p99_9);

        v.gpu_frametime_ms = fmtMsOneDecimal(snap.gpu_frame_ms);
        v.cpu_frametime_ms = fmtMsOneDecimal(snap.cpu_frame_ms);

        v.gpu_temp_c       = fmtTempInt(snap.gpu_temp_c);
        // cpu_temp_c intentionally stays "--": there's no anti-cheat-
        // safe in-process CPU temp source until PawnIO lands in a
        // follow-up PR.
        v.cpu_temp_c       = "--";

        v.gpu_util_pct     = fmtPctInt(snap.gpu_utilisation_pct);
        v.cpu_util_pct     = fmtPctInt(snap.cpu_utilisation_pct);

        v.gpu_util_fraction = std::isfinite(snap.gpu_utilisation_pct)
            ? std::clamp(snap.gpu_utilisation_pct / 100.0f, 0.0f, 1.0f)
            : 0.0f;
        v.cpu_util_fraction = std::isfinite(snap.cpu_utilisation_pct)
            ? std::clamp(snap.cpu_utilisation_pct / 100.0f, 0.0f, 1.0f)
            : 0.0f;
        return v;
    }


    // The geometry of the head-locked quad. Coordinates are in OpenXR
    // view space (XR_REFERENCE_SPACE_TYPE_VIEW): +X right, +Y up,
    // -Z forward.
    //
    // The renderer turns these into an XrPosef + XrExtent2Df at draw
    // time — no OpenXR types here so this header stays portable.
    struct OverlayGeometry {
        // Quad CENTER position relative to the view origin (metres).
        float pos_x;
        float pos_y;
        float pos_z;  // negative = in front of the user
        // Quad dimensions in metres at the chosen distance. The 0.28 ×
        // 0.187 m default at z = -1 m is ~16° × 11° of FOV. Matches
        // the 720×480 (3:2) texture's native aspect so pixels stay
        // square in the HMD with no anisotropic stretching.
        float width_m;
        float height_m;
    };

    // Resolve a settings.overlay.position string into a concrete geometry.
    // Unknown strings fall back to head_top_right — same robustness
    // contract as parseHotkey's unknown-key fallback.
    //
    // `scale` multiplies the default quad dimensions (already clamped to
    // [0.5, 2.0] by parseSettings).
    inline OverlayGeometry geometryForPosition(const std::string& position,
                                                float scale) noexcept {
        // 3:2 aspect to match the 720×480 texture. 0.28 × 0.187 m at
        // 1 m view-space distance ≈ 16° × 11° angular size. Pixel
        // density stays square (720/480 = 0.28/0.187 = 1.5). Corner
        // offsets bumped proportionally so the quad's CORNER (not
        // centre) still lands near the previous off-axis target.
        constexpr float kBaseWidth  = 0.28f;
        constexpr float kBaseHeight = 0.187f;
        constexpr float kZ          = -1.0f;          // 1 m forward
        constexpr float kCornerOffX = 0.22f;
        constexpr float kCornerOffY = 0.14f;

        OverlayGeometry g;
        g.width_m  = kBaseWidth  * scale;
        g.height_m = kBaseHeight * scale;
        g.pos_z    = kZ;
        g.pos_x    = +kCornerOffX;
        g.pos_y    = +kCornerOffY;

        // String compare without iequalsAscii dependency: positions are
        // normalised in the parser to lowercase snake_case. Keep this
        // table small — every entry adds to the per-frame branching
        // budget of the renderer.
        if (position == "head_top_left") {
            g.pos_x = -kCornerOffX;
        } else if (position == "head_top_center") {
            g.pos_x = 0.0f;
        } else if (position == "head_center") {
            g.pos_x = 0.0f;
            g.pos_y = 0.0f;
        }
        // anything else → head_top_right (already set above)
        return g;
    }

    // Convert one histogram sample (nanoseconds) into a 0..1 normalised
    // bar height, given the highest sample currently in the ring. Used
    // by older fall-back paths; the current renderer uses
    // barVisualForSample below for budget-anchored bars + colour tiers.
    //
    // Returns 0 if the ring is empty or sample is non-positive. Capped
    // at 1.0 so a single spike doesn't overflow the bar area visually.
    inline float normaliseBar(int64_t sampleNs, int64_t maxNs) noexcept {
        if (maxNs <= 0 || sampleNs <= 0) return 0.0f;
        const float ratio = static_cast<float>(sampleNs) / static_cast<float>(maxNs);
        return std::clamp(ratio, 0.0f, 1.0f);
    }

    // Colour tier for a histogram bar — fpsVR-style "headroom
    // warning" tiering: the bar stays at the panel's accent colour
    // when there's healthy headroom (>20 % below the budget), turns
    // ORANGE as a warning when headroom falls under 20 %, and RED
    // when headroom is under 10 % (i.e. the frame is on the verge of
    // overruns / reprojection).
    //
    //   ratio = frametime / budget
    //   ratio < 0.80 → Green   (≥ 20 % headroom — fine)
    //   ratio < 0.90 → Orange  (10–20 % headroom — warning)
    //   ratio ≥ 0.90 → Red     ( < 10 % headroom — critical)
    //
    // The same tiering drives the circular utilisation gauge's fill
    // colour in the bottom panels — see drawCircularGauge in the
    // renderer.
    enum class BarTier { Green, Orange, Red };

    struct BarVisual {
        // Fraction of the strip height the bar should take (0..1).
        // The renderer multiplies this by the strip's pixel height to
        // get the actual rectangle to fill.
        float   heightFraction;
        BarTier tier;
    };

    // Anchored normalisation: the Y axis spans `0..1.2 × budgetNs`,
    // so a frame at 100 % of budget fills ~83 % of the strip height
    // (= 1/1.2) — the budget reference line sits at that same 83 %.
    // Bars that exceed 120 % of budget saturate at the strip's top.
    //
    // Why 1.2× and not 2×: the previous 0..2× range reserved the
    // entire UPPER half of the strip for overruns, which meant
    // typical light-load frames (40–70 % of budget) only filled
    // 20–35 % of the strip — leaving the top half empty under normal
    // operation. fpsVR uses 0..2× too, but with much taller strips,
    // so the absolute pixel space below "budget" stays visually
    // satisfying. At our 54-px strip height the trade-off flips —
    // fuller bars under normal load + clipped overruns reads
    // better than half-empty bars + visible overruns.
    //
    // Trade-off: a frame at 130 % budget (= -30 % headroom)
    // saturates visually at the top, indistinguishable from 200 %.
    // The TIER colour still tells the user "this frame is in red
    // (≥ 90 % budget)" — the SHAPE of the bar tells "how often",
    // the COLOUR tells "how bad". Most users care about the
    // first-derivative (am I getting overruns?) more than the
    // exact magnitude.
    //
    // Budget <= 0 (no display-period info yet from the runtime) yields
    // a green zero-height bar — the renderer skips the strip entirely
    // in that case, but the helper stays defensive.
    inline BarVisual barVisualForSample(int64_t sampleNs, int64_t budgetNs) noexcept {
        if (budgetNs <= 0 || sampleNs <= 0) return {0.0f, BarTier::Green};
        const float ratio = static_cast<float>(sampleNs) / static_cast<float>(budgetNs);
        // ratio / 1.2 == ratio * (5/6). Direct multiply chosen for
        // numerical stability (1.2f isn't exactly representable in
        // IEEE-754 but the division is well-conditioned for the
        // [0, 4] input range we'll see in practice).
        const float height = std::clamp(ratio / 1.2f, 0.0f, 1.0f);
        BarTier tier = BarTier::Green;
        if (ratio >= 0.9f) {
            tier = BarTier::Red;
        } else if (ratio >= 0.8f) {
            tier = BarTier::Orange;
        }
        return {height, tier};
    }

    // Reuse the same tiering for the circular utilisation gauge in
    // the bottom panels — keeps the visual language coherent (a bar
    // turning orange next to a gauge turning red reads as "GPU was
    // borderline last second, now it's pinned"). The input here is
    // a 0..1 utilisation FRACTION (not a frametime ratio), but the
    // semantics are the same: 0.80–0.89 = orange, 0.90+ = red.
    inline BarTier gaugeTierForUtilisation(float fraction) noexcept {
        if (!(fraction >= 0.0f)) return BarTier::Green;  // NaN → green default
        if (fraction >= 0.90f) return BarTier::Red;
        if (fraction >= 0.80f) return BarTier::Orange;
        return BarTier::Green;
    }

    // The Y position of the budget reference line, expressed as a
    // fraction of the strip height (0 = top, 1 = bottom). With the
    // 0..1.2× budget scale above, the line sits at 1/6 of the strip
    // (≈ 16.7 % from the top) — that's where the BAR TIP lands when
    // ratio = 1.0 (since heightFraction = 1/1.2 = 5/6, and the bar
    // grows upward from the bottom, so the tip is 5/6 from the
    // bottom = 1/6 from the top).
    inline constexpr float budgetLineFraction() noexcept {
        return 1.0f / 6.0f;
    }

} // namespace openxr_api_layer::detail
