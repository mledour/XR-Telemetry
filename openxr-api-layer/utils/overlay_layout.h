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
//   * formatOverlayLines: snapshot → list of monospace strings to draw
//   * geometryForPosition: settings.position string → quad pose constants
//   * applyScale: multiply default quad dimensions by user-config scale
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
#include <vector>

namespace openxr_api_layer::detail {

    // One row of the side-by-side fpsVR-style HUD: a (left, right)
    // pair the renderer draws in the two column rectangles. The
    // current rows (top-to-bottom):
    //
    //   row 0:  FPS instant / target     |  AVG fps
    //   row 1:  GPU frametime ms          |  CPU frametime ms
    //   ── histograms drawn between row 1 and row 2 ──
    //   row 2:  GPU util %                |  CPU util %
    //
    // Empty vector if the snapshot hasn't finalised yet — the
    // caller skips text drawing in that case.
    struct OverlayRow {
        std::string left;
        std::string right;
    };

    inline std::vector<OverlayRow> formatOverlayRows(const OverlaySnapshot& snap) {
        if (!snap.valid) return {};
        std::vector<OverlayRow> rows;
        rows.reserve(3);

        // Small string helpers without dragging in fmt/format.h.
        //
        // Each helper checks std::isfinite() before %f-printing: the
        // aggregator already guards against division by zero, but a
        // momentarily-negative `frame_total_ns - wait_block_ns` (caused
        // by a missed QPC sample or by a runtime mis-ordering the
        // begin/wait pair) can accumulate into a NaN or ±Inf, which
        // %5.1f renders as " nan" / " inf" / "-inf" — breaking the
        // monospace alignment of the right-aligned columns. Rendering
        // "  --.-" / "  --" instead keeps the grid intact and signals
        // "no data" visually, at zero extra cost. The fix sits here
        // rather than in the aggregator because the aggregator's job is
        // to publish what it sees; the renderer's job is to keep the
        // HUD readable even when the input is briefly garbage.
        auto fmtFps = [](float fps) {
            char buf[16];
            if (std::isfinite(fps)) {
                std::snprintf(buf, sizeof(buf), "%5.1f", fps);
            } else {
                std::snprintf(buf, sizeof(buf), " --.-");
            }
            return std::string(buf);
        };
        auto fmtMs = [](float ms) {
            char buf[16];
            if (std::isfinite(ms)) {
                std::snprintf(buf, sizeof(buf), "%5.2f", ms);
            } else {
                std::snprintf(buf, sizeof(buf), " --.-");
            }
            return std::string(buf);
        };
        auto fmtPct = [](float pct) {
            char buf[8];
            if (std::isfinite(pct)) {
                std::snprintf(buf, sizeof(buf), "%3.0f", pct);
            } else {
                std::snprintf(buf, sizeof(buf), " --");
            }
            return std::string(buf);
        };

        rows.push_back({
            "FPS " + fmtFps(snap.fps_instant) + " / " + fmtFps(snap.target_fps),
            "AVG " + fmtFps(snap.fps_avg)
        });
        rows.push_back({
            "GPU " + fmtMs(snap.gpu_frame_ms) + " ms",
            "CPU " + fmtMs(snap.cpu_frame_ms) + " ms"
        });
        rows.push_back({
            "GPU " + fmtPct(snap.gpu_utilisation_pct) + " %",
            "CPU " + fmtPct(snap.cpu_utilisation_pct) + " %"
        });
        return rows;
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
        // Quad dimensions in metres at the chosen distance. The 0.20 ×
        // 0.075 m default at z = -1 m is ~11° × 4° of FOV. Matches
        // fpsvr's discreet placement.
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
        // Default size at scale=1.0. Tweak in one place if fpsvr-style
        // proves too small/big on real HMDs.
        constexpr float kBaseWidth  = 0.20f;
        constexpr float kBaseHeight = 0.075f;
        constexpr float kZ          = -1.0f;          // 1 m forward
        constexpr float kCornerOffX = 0.16f;          // ≈ 9° off-axis at 1 m
        constexpr float kCornerOffY = 0.08f;          // ≈ 5° off-axis at 1 m

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

    // Colour tier for a histogram bar — fpsvr / OpenXR Toolkit
    // convention: green when comfortably under budget, yellow when
    // approaching the limit, red when the frame busts the period (it
    // either dropped or got reprojected). Matches the "you can see the
    // overruns at a glance" UX users expect.
    enum class BarTier { Green, Yellow, Red };

    struct BarVisual {
        // Fraction of the strip height the bar should take (0..1).
        // The renderer multiplies this by the strip's pixel height to
        // get the actual rectangle to fill.
        float   heightFraction;
        BarTier tier;
    };

    // Anchored normalisation: the Y axis spans `0..2 × budgetNs`, with
    // the budget itself at the strip midpoint. A frame at 100 % of
    // budget shows a bar exactly to the budget line; a doubled-budget
    // frame fills the strip; everything beyond saturates at the top.
    // Returning a fixed Y range (instead of auto-normalising to the
    // ring's max) is what makes the budget line visually stable —
    // users see "am I over the line" without rescaling artefacts when
    // a single stutter spikes the max.
    //
    // Budget <= 0 (no display-period info yet from the runtime) yields
    // a green zero-height bar — the renderer skips the strip entirely
    // in that case, but the helper stays defensive.
    inline BarVisual barVisualForSample(int64_t sampleNs, int64_t budgetNs) noexcept {
        if (budgetNs <= 0 || sampleNs <= 0) return {0.0f, BarTier::Green};
        const float ratio = static_cast<float>(sampleNs) / static_cast<float>(budgetNs);
        const float height = std::clamp(ratio * 0.5f, 0.0f, 1.0f);
        BarTier tier = BarTier::Green;
        if (ratio >= 1.0f) {
            tier = BarTier::Red;
        } else if (ratio >= 0.8f) {
            tier = BarTier::Yellow;
        }
        return {height, tier};
    }

    // The Y position of the budget reference line, expressed as a
    // fraction of the strip height (0 = top, 1 = bottom). With the
    // 0..2× budget scale above, the line sits at 50 % of the strip's
    // pixel height regardless of how big the strip is — keeps the
    // budget marker visually anchored even if we resize the texture.
    inline constexpr float budgetLineFraction() noexcept {
        return 0.5f;
    }

} // namespace openxr_api_layer::detail
