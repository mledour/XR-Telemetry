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

    // The four rows the HUD displays, top-to-bottom. Each row is a
    // monospace string with column-aligned fields. fpsvr-style:
    //
    //   FPS    89.8 / 90.0
    //   AVG    90.1
    //   CPU    6.78 ms (61%)
    //   GPU    5.18 ms (47%)
    //
    // Returns an empty vector if the snapshot hasn't finalised yet —
    // the caller should not draw anything in that case.
    inline std::vector<std::string> formatOverlayLines(const OverlaySnapshot& snap) {
        if (!snap.valid) return {};
        std::vector<std::string> out;
        out.reserve(4);

        // Small string helpers without dragging in fmt/format.h.
        auto fmtFps = [](float fps) {
            // 4-char-wide right-aligned float (matches monospace columns).
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%5.1f", fps);
            return std::string(buf);
        };
        auto fmtMs = [](float ms) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%5.2f", ms);
            return std::string(buf);
        };
        auto fmtPct = [](float pct) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%3.0f", pct);
            return std::string(buf);
        };

        out.push_back("FPS  " + fmtFps(snap.fps_instant) + " / " + fmtFps(snap.target_fps));
        out.push_back("AVG  " + fmtFps(snap.fps_avg));
        out.push_back("CPU  " + fmtMs(snap.cpu_frame_ms) + " ms (" + fmtPct(snap.cpu_utilisation_pct) + "%)");
        out.push_back("GPU  " + fmtMs(snap.gpu_frame_ms) + " ms (" + fmtPct(snap.gpu_utilisation_pct) + "%)");
        return out;
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
    // by the renderer to draw mini histograms under each metric.
    //
    // Returns 0 if the ring is empty or sample is non-positive. Capped
    // at 1.0 so a single spike doesn't overflow the bar area visually.
    inline float normaliseBar(int64_t sampleNs, int64_t maxNs) noexcept {
        if (maxNs <= 0 || sampleNs <= 0) return 0.0f;
        const float ratio = static_cast<float>(sampleNs) / static_cast<float>(maxNs);
        return std::clamp(ratio, 0.0f, 1.0f);
    }

} // namespace openxr_api_layer::detail
