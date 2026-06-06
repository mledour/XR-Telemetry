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
// test_overlay_layout.cpp — unit tests on the pure HUD-layout helpers:
//   - formatOverlayDisplayValues (snapshot → per-cell formatted strings)
//   - geometryForPosition (settings.position → quad pose/size)
//   - normaliseBar (sample / max → [0, 1] bar height, legacy helper)
//   - barVisualForSample (sample / budget → (heightFraction, BarTier))
//   - budgetLineFraction (reference line position in the strip)
// =============================================================================

#include <doctest/doctest.h>

#include <limits>

#include "utils/overlay_layout.h"

using openxr_api_layer::detail::OverlaySnapshot;
using openxr_api_layer::detail::OverlayDisplayValues;
using openxr_api_layer::detail::formatOverlayDisplayValues;
using openxr_api_layer::detail::geometryForPosition;
using openxr_api_layer::detail::OverlayVec3;
using openxr_api_layer::detail::OverlayQuat;
using openxr_api_layer::detail::OverlayPose;
using openxr_api_layer::detail::rotateByQuat;
using openxr_api_layer::detail::yawOnlyQuat;
using openxr_api_layer::detail::composeAnchorPose;
using openxr_api_layer::detail::normaliseBar;
using openxr_api_layer::detail::BarTier;
using openxr_api_layer::detail::barVisualForSample;
using openxr_api_layer::detail::budgetLineFraction;

// =============================================================================
// formatOverlayDisplayValues — snapshot → POD of per-cell formatted strings.
// Replaces the old row-based formatOverlayRows after the fpsVR redesign:
// the new HUD has a 4-section layout (header bar / GPU panel / CPU panel /
// bottom row) that doesn't fit a left-right column grid, so we pre-format
// every cell of the design into a flat POD that the renderer indexes by name.
// =============================================================================

TEST_CASE("formatOverlayDisplayValues: invalid snapshot → valid=false POD") {
    OverlaySnapshot snap;  // valid=false by default
    const auto v = formatOverlayDisplayValues(snap);
    CHECK_FALSE(v.valid);
    // Strings keep their default "--" / "--.-" placeholders so the
    // renderer can still paint something stable on the very first
    // frames before the aggregator publishes.
    CHECK(v.fps_instant == "--");
    CHECK(v.fps_avg     == "--");
    CHECK(v.fps_p95     == "--");
    CHECK(v.fps_p99     == "--");
    CHECK(v.fps_p99_9   == "--");
    CHECK(v.gpu_frametime_ms == "--.-");
    CHECK(v.cpu_frametime_ms == "--.-");
    CHECK(v.cpu_app_ms       == "--.-");
    CHECK(v.gpu_temp_c  == "--");
    CHECK(v.cpus_max_pct == "--");
    CHECK(v.gpu_util_pct == "--");
    CHECK(v.cpu_util_pct == "--");
    CHECK(v.vram_pct     == "--");
    CHECK(v.gpu_util_fraction == 0.0f);
    CHECK(v.cpu_util_fraction == 0.0f);
    CHECK(v.vram_fraction     == 0.0f);
    CHECK(v.cpus_max_fraction == 0.0f);
}

TEST_CASE("formatOverlayDisplayValues: nominal snapshot populates every cell") {
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant         = 142.0f;
    snap.fps_avg             = 138.0f;
    snap.fps_p95             = 124.0f;
    snap.fps_p99             = 108.0f;
    snap.fps_p99_9           = 98.0f;
    snap.target_fps          = 144.0f;
    snap.gpu_frame_ms        = 6.7f;
    snap.cpu_frame_ms        = 7.4f;   // Render ms (per-cycle)
    snap.cpu_app_ms          = 4.3f;   // App ms (wait→end window)
    snap.gpu_temp_c          = 67.0f;
    snap.gpu_utilisation_pct = 92.0f;
    snap.cpu_utilisation_pct = 78.0f;
    snap.cpus_max_pct        = 98.0f;  // busiest single core

    const auto v = formatOverlayDisplayValues(snap);
    REQUIRE(v.valid);

    // Header bar — integer FPS, no decimals.
    CHECK(v.fps_instant == "142");
    CHECK(v.fps_avg     == "138");
    CHECK(v.fps_p95     == "124");
    CHECK(v.fps_p99     == "108");
    CHECK(v.fps_p99_9   == "98");

    // Frametime panels — one decimal.
    CHECK(v.gpu_frametime_ms == "6.7");
    CHECK(v.cpu_frametime_ms == "7.4");
    CHECK(v.cpu_app_ms       == "4.3");

    // Bottom row — GPU temp (integer °C) and "CPUs" (busiest-core %).
    CHECK(v.gpu_temp_c == "67");
    CHECK(v.cpus_max_pct == "98");
    CHECK(v.cpus_max_fraction == doctest::Approx(0.98f).epsilon(0.001));

    // Bottom row utilisation — integer percent + matching fraction.
    CHECK(v.gpu_util_pct == "92");
    CHECK(v.cpu_util_pct == "78");
    CHECK(v.gpu_util_fraction == doctest::Approx(0.92f).epsilon(0.001));
    CHECK(v.cpu_util_fraction == doctest::Approx(0.78f).epsilon(0.001));
}

TEST_CASE("formatOverlayDisplayValues: vram percentage from used/budget bytes") {
    // 6 GB used out of 8 GB budget → 75 %.
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant         = 144.0f;
    snap.vram_used_bytes     = 6'000'000'000ULL;
    snap.vram_budget_bytes   = 8'000'000'000ULL;
    const auto v = formatOverlayDisplayValues(snap);
    CHECK(v.vram_pct == "75");
    CHECK(v.vram_fraction == doctest::Approx(0.75f).epsilon(0.001));
}

TEST_CASE("formatOverlayDisplayValues: vram pct = '--' when DXGI didn't answer") {
    // No VRAM data → both bytes fields are 0 (the gpu_telemetry.h
    // sentinel contract). Formatter must NOT divide by zero — sets
    // vram_pct to "--" and vram_fraction to 0.0.
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant       = 90.0f;
    snap.vram_used_bytes   = 0;
    snap.vram_budget_bytes = 0;
    const auto v = formatOverlayDisplayValues(snap);
    CHECK(v.vram_pct == "--");
    CHECK(v.vram_fraction == doctest::Approx(0.0f).epsilon(0.001));
}

TEST_CASE("formatOverlayDisplayValues: vram clamps at 100 % even with used > budget") {
    // Rare but possible — driver returns CurrentUsage briefly above
    // Budget (the budget recalibrates ~1 Hz, usage can spike). The
    // formatter clamps to keep tier semantics sane (no "121 %"
    // displayed).
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant       = 90.0f;
    snap.vram_used_bytes   = 9'000'000'000ULL;
    snap.vram_budget_bytes = 8'000'000'000ULL;
    const auto v = formatOverlayDisplayValues(snap);
    CHECK(v.vram_fraction == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(v.vram_pct == "100");
}

TEST_CASE("formatOverlayDisplayValues: rounding goes to nearest integer for FPS / temps / pct") {
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant         = 89.6f;   // → 90
    snap.fps_avg             = 89.4f;   // → 89
    snap.fps_p95             = 89.5f;   // → 90 (banker's? std::round goes away-from-zero, so 90)
    snap.fps_p99             = 0.4f;    // → 0
    snap.gpu_temp_c          = 67.7f;   // → 68
    snap.gpu_utilisation_pct = 92.49f;  // → 92
    snap.cpu_utilisation_pct = 92.51f;  // → 93
    const auto v = formatOverlayDisplayValues(snap);
    CHECK(v.fps_instant == "90");
    CHECK(v.fps_avg     == "89");
    CHECK(v.fps_p95     == "90");
    CHECK(v.fps_p99     == "0");
    CHECK(v.gpu_temp_c  == "68");
    CHECK(v.gpu_util_pct == "92");
    CHECK(v.cpu_util_pct == "93");
}

TEST_CASE("formatOverlayDisplayValues: gpu_temp_c NaN renders as '--'") {
    // Non-NVIDIA host: NvAPI absent → snap.gpu_temp_c stays NaN.
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant = 90.0f;
    snap.gpu_temp_c  = std::numeric_limits<float>::quiet_NaN();
    const auto v = formatOverlayDisplayValues(snap);
    CHECK(v.gpu_temp_c == "--");
    // "CPUs" renders "--" when the sampler has no reading — NaN is the
    // source-absent / no-baseline sentinel (CpuUsageReader didn't init,
    // or this is the first poll). Same string, same width, so the bottom
    // row stays aligned.
    CHECK(v.cpus_max_pct == "--");
    CHECK(v.cpus_max_fraction == 0.0f);
}

TEST_CASE("formatOverlayDisplayValues: CPUs cell formats, rounds, clamps, and falls back") {
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant = 90.0f;

    // Nominal: rounds to nearest integer, fraction tracks the percent.
    snap.cpus_max_pct = 87.4f;  // → 87
    auto v = formatOverlayDisplayValues(snap);
    CHECK(v.cpus_max_pct == "87");
    CHECK(v.cpus_max_fraction == doctest::Approx(0.874f).epsilon(0.001));

    // Pinned single core → "100 %", fraction saturates at 1.0.
    snap.cpus_max_pct = 100.0f;
    v = formatOverlayDisplayValues(snap);
    CHECK(v.cpus_max_pct == "100");
    CHECK(v.cpus_max_fraction == doctest::Approx(1.0f).epsilon(0.001));

    // Source absent (NaN) → "--", fraction 0 so the gauge draws nothing.
    snap.cpus_max_pct = std::numeric_limits<float>::quiet_NaN();
    v = formatOverlayDisplayValues(snap);
    CHECK(v.cpus_max_pct == "--");
    CHECK(v.cpus_max_fraction == 0.0f);

    // Defensive clamp if an out-of-range value somehow reaches the
    // formatter (the reader already clamps to [0,100]).
    snap.cpus_max_pct = 250.0f;
    CHECK(formatOverlayDisplayValues(snap).cpus_max_fraction ==
          doctest::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("formatOverlayDisplayValues: out-of-range temp clamps to '--' sentinel") {
    // A bogus NvAPI read (returns -50 or +999) shouldn't render as
    // "-50" or "999" — that would suggest a real reading. Clamp to
    // the "--" sentinel just like NaN.
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant = 90.0f;
    snap.gpu_temp_c  = 999.0f;
    CHECK(formatOverlayDisplayValues(snap).gpu_temp_c == "--");
    snap.gpu_temp_c  = -75.0f;
    CHECK(formatOverlayDisplayValues(snap).gpu_temp_c == "--");
    // Reasonable values pass through.
    snap.gpu_temp_c  = 75.0f;
    CHECK(formatOverlayDisplayValues(snap).gpu_temp_c == "75");
}

TEST_CASE("formatOverlayDisplayValues: non-finite floats become placeholder strings") {
    // Guard contract — the aggregator can briefly accumulate a NaN or
    // ±Inf if QPC samples are mis-ordered (negative
    // `frame_total - wait_block`). The formatter is the single
    // chokepoint: NO "nan" / "inf" tokens may leak into any cell.
    const float kNan = std::numeric_limits<float>::quiet_NaN();
    const float kInf = std::numeric_limits<float>::infinity();
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant         = kNan;
    snap.fps_avg             = kInf;
    snap.fps_p95             = -kInf;
    snap.fps_p99             = kNan;
    snap.gpu_frame_ms        = kInf;
    snap.cpu_frame_ms        = kNan;
    snap.gpu_temp_c          = kInf;
    snap.gpu_utilisation_pct = kNan;
    snap.cpu_utilisation_pct = kInf;

    const auto v = formatOverlayDisplayValues(snap);
    auto noBadToken = [](const std::string& s) {
        return s.find("nan") == std::string::npos &&
               s.find("inf") == std::string::npos &&
               s.find("NAN") == std::string::npos &&
               s.find("INF") == std::string::npos;
    };
    CHECK(noBadToken(v.fps_instant));
    CHECK(noBadToken(v.fps_avg));
    CHECK(noBadToken(v.fps_p95));
    CHECK(noBadToken(v.fps_p99));
    CHECK(noBadToken(v.gpu_frametime_ms));
    CHECK(noBadToken(v.cpu_frametime_ms));
    CHECK(noBadToken(v.gpu_temp_c));
    CHECK(noBadToken(v.gpu_util_pct));
    CHECK(noBadToken(v.cpu_util_pct));
    // Utilisation gauge fractions snap to 0 on non-finite — the
    // renderer's arc geometry needs a [0,1] number.
    CHECK(v.gpu_util_fraction == 0.0f);
    CHECK(v.cpu_util_fraction == 0.0f);
}

TEST_CASE("formatOverlayDisplayValues: utilisation fractions clamped to [0, 1]") {
    // The aggregator already clamps cpu/gpu_utilisation_pct to [0, 100]
    // before publish, but the formatter doubles up the safety in case
    // a future path bypasses the aggregator.
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant = 90.0f;
    snap.gpu_utilisation_pct = 250.0f;   // somehow above 100
    snap.cpu_utilisation_pct = -30.0f;   // somehow below 0
    const auto v = formatOverlayDisplayValues(snap);
    CHECK(v.gpu_util_fraction == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(v.cpu_util_fraction == doctest::Approx(0.0f).epsilon(0.001));
}

// =============================================================================
// geometryForPosition — quad pose lookup. Quad base width is fixed; the
// height tracks the texture aspect (kTexW/kTexH) so HMD pixels stay square.
// The expected dimensions below move whenever the texture height changes.
// =============================================================================

TEST_CASE("geometryForPosition: default head_top_right → +X, +Y, -Z") {
    const auto g = geometryForPosition("head_top_right", 1.0f);
    CHECK(g.pos_x > 0.0f);      // right of view origin
    CHECK(g.pos_y > 0.0f);      // above
    CHECK(g.pos_z < 0.0f);      // in front
    CHECK(g.width_m > 0.0f);
    CHECK(g.height_m > 0.0f);
    // Base dimensions tuned for the current 720×kTexH redesign;
    // height_m tracks the texture height so pixel density stays
    // square in the HMD. Values updated each time the layout's
    // vertical budget changes (e.g. section-gap tweaks freeing
    // texture height).
    CHECK(g.width_m  == doctest::Approx(0.28f).epsilon(0.001));
    CHECK(g.height_m == doctest::Approx(0.162f).epsilon(0.001));
}

TEST_CASE("geometryForPosition: aspect ratio matches the texture's native") {
    const auto g = geometryForPosition("head_top_right", 1.0f);
    const float aspect = g.width_m / g.height_m;
    CHECK(aspect == doctest::Approx(720.0f / 416.0f).epsilon(0.01));
}

TEST_CASE("geometryForPosition: head_top_left mirrors X") {
    const auto right = geometryForPosition("head_top_right", 1.0f);
    const auto left  = geometryForPosition("head_top_left", 1.0f);
    CHECK(left.pos_x == doctest::Approx(-right.pos_x).epsilon(0.001));
    CHECK(left.pos_y == doctest::Approx(right.pos_y).epsilon(0.001));
    CHECK(left.pos_z == doctest::Approx(right.pos_z).epsilon(0.001));
}

TEST_CASE("geometryForPosition: head_top_center has zero X offset") {
    const auto g = geometryForPosition("head_top_center", 1.0f);
    CHECK(g.pos_x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(g.pos_y > 0.0f);   // still above the gaze line
}

TEST_CASE("geometryForPosition: head_center has zero X AND Y offset") {
    const auto g = geometryForPosition("head_center", 1.0f);
    CHECK(g.pos_x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(g.pos_y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(g.pos_z < 0.0f);
}

TEST_CASE("geometryForPosition: unknown string falls back to head_top_right") {
    const auto fallback = geometryForPosition("squiggle", 1.0f);
    const auto ref      = geometryForPosition("head_top_right", 1.0f);
    CHECK(fallback.pos_x == doctest::Approx(ref.pos_x).epsilon(0.001));
    CHECK(fallback.pos_y == doctest::Approx(ref.pos_y).epsilon(0.001));
}

TEST_CASE("geometryForPosition: scale multiplies width and height") {
    const auto half   = geometryForPosition("head_top_right", 0.5f);
    const auto normal = geometryForPosition("head_top_right", 1.0f);
    const auto double_ = geometryForPosition("head_top_right", 2.0f);

    CHECK(half.width_m   == doctest::Approx(normal.width_m  * 0.5f).epsilon(0.001));
    CHECK(half.height_m  == doctest::Approx(normal.height_m * 0.5f).epsilon(0.001));
    CHECK(double_.width_m  == doctest::Approx(normal.width_m  * 2.0f).epsilon(0.001));
    CHECK(double_.height_m == doctest::Approx(normal.height_m * 2.0f).epsilon(0.001));
    // Position offsets are NOT scaled — keeping the corner fixed makes
    // a 2× HUD still hug the same corner of the user's FOV.
    CHECK(half.pos_x == doctest::Approx(normal.pos_x).epsilon(0.001));
    CHECK(half.pos_y == doctest::Approx(normal.pos_y).epsilon(0.001));
}

TEST_CASE("geometryForPosition: offset_x / offset_y shift the centre, not the size") {
    const auto base    = geometryForPosition("head_top_right", 1.0f);
    const auto nudged  = geometryForPosition("head_top_right", 1.0f, 0.1f, -0.05f);
    CHECK(nudged.pos_x == doctest::Approx(base.pos_x + 0.1f).epsilon(0.0001));
    CHECK(nudged.pos_y == doctest::Approx(base.pos_y - 0.05f).epsilon(0.0001));
    CHECK(nudged.pos_z == doctest::Approx(base.pos_z).epsilon(0.0001));
    // Size is untouched by the nudge.
    CHECK(nudged.width_m  == doctest::Approx(base.width_m).epsilon(0.0001));
    CHECK(nudged.height_m == doctest::Approx(base.height_m).epsilon(0.0001));
}

TEST_CASE("geometryForPosition: negative offset_x pushes head_top_left further out") {
    // Top-left sits at -X; a negative offset_x pushes it even further left
    // (more negative), i.e. further into the corner.
    const auto base   = geometryForPosition("head_top_left", 1.0f);
    const auto pushed = geometryForPosition("head_top_left", 1.0f, -0.1f, 0.0f);
    CHECK(pushed.pos_x == doctest::Approx(base.pos_x - 0.1f).epsilon(0.0001));
    CHECK(pushed.pos_x < base.pos_x);
}

TEST_CASE("geometryForPosition: offset composes with head_center (zero preset)") {
    // head_center is the (0,0) preset, so the offset IS the resulting centre.
    const auto g = geometryForPosition("head_center", 1.0f, 0.2f, 0.15f);
    CHECK(g.pos_x == doctest::Approx(0.2f).epsilon(0.0001));
    CHECK(g.pos_y == doctest::Approx(0.15f).epsilon(0.0001));
}

TEST_CASE("geometryForPosition: default corner hugs the edge more than the old 0.22/0.14") {
    // Regression guard for the default bump: the stock corner offset must
    // stay pushed out toward the edge (was 0.22 / 0.14 before this change).
    const auto g = geometryForPosition("head_top_right", 1.0f);
    CHECK(g.pos_x > 0.22f);
    CHECK(g.pos_y > 0.14f);
}

// =============================================================================
// rotateByQuat / composeAnchorPose — world-anchor pose math. Pure, runtime-
// free; this is what freezes the head-locked quad into the play space when
// overlay.anchor == world. A regression here puts the world-locked panel in
// the wrong spot or facing the wrong way.
// =============================================================================

TEST_CASE("rotateByQuat: identity quaternion leaves the vector unchanged") {
    const OverlayQuat ident{0.0f, 0.0f, 0.0f, 1.0f};
    const OverlayVec3 v{0.3f, -1.2f, 0.7f};
    const auto r = rotateByQuat(ident, v);
    CHECK(r.x == doctest::Approx(v.x).epsilon(0.0001));
    CHECK(r.y == doctest::Approx(v.y).epsilon(0.0001));
    CHECK(r.z == doctest::Approx(v.z).epsilon(0.0001));
}

TEST_CASE("rotateByQuat: +90° about Y maps forward (-Z) to -X") {
    // Right-handed rotation about +Y by +90°: -Z → -X.
    constexpr float s = 0.70710678f;   // sin/cos of 45°
    const OverlayQuat yaw90{0.0f, s, 0.0f, s};
    const auto r = rotateByQuat(yaw90, {0.0f, 0.0f, -1.0f});
    CHECK(r.x == doctest::Approx(-1.0f).epsilon(0.0001));
    CHECK(r.y == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(r.z == doctest::Approx(0.0f).epsilon(0.0001));
}

TEST_CASE("composeAnchorPose: identity head → orientation identity, position = head + offset") {
    const OverlayPose head{{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.5f, -2.0f}};
    const OverlayVec3 offset{0.22f, 0.14f, -1.0f};
    const auto a = composeAnchorPose(head, offset);
    CHECK(a.orientation.x == doctest::Approx(0.0f));
    CHECK(a.orientation.y == doctest::Approx(0.0f));
    CHECK(a.orientation.z == doctest::Approx(0.0f));
    CHECK(a.orientation.w == doctest::Approx(1.0f));
    CHECK(a.position.x == doctest::Approx(1.0f + 0.22f).epsilon(0.0001));
    CHECK(a.position.y == doctest::Approx(1.5f + 0.14f).epsilon(0.0001));
    CHECK(a.position.z == doctest::Approx(-2.0f - 1.0f).epsilon(0.0001));
}

TEST_CASE("composeAnchorPose: rotated head places the offset in world space") {
    // Head at origin, yawed +90° about Y. A quad 1 m "forward" in the
    // head's local frame (offset -Z) must land 1 m along world -X, and
    // keep the head's yaw so it still faces the user.
    constexpr float s = 0.70710678f;
    const OverlayPose head{{0.0f, s, 0.0f, s}, {0.0f, 0.0f, 0.0f}};
    const auto a = composeAnchorPose(head, {0.0f, 0.0f, -1.0f});
    CHECK(a.position.x == doctest::Approx(-1.0f).epsilon(0.0001));
    CHECK(a.position.y == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(a.position.z == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(a.orientation.y == doctest::Approx(s).epsilon(0.0001));
    CHECK(a.orientation.w == doctest::Approx(s).epsilon(0.0001));
}

TEST_CASE("yawOnlyQuat: identity stays identity") {
    const auto y = yawOnlyQuat({0.0f, 0.0f, 0.0f, 1.0f});
    CHECK(y.x == doctest::Approx(0.0f));
    CHECK(y.y == doctest::Approx(0.0f));
    CHECK(y.z == doctest::Approx(0.0f));
    CHECK(y.w == doctest::Approx(1.0f));
}

TEST_CASE("yawOnlyQuat: pure pitch (about X) collapses to identity") {
    // 90° about X — no yaw component → upright (identity) panel.
    constexpr float s = 0.70710678f;
    const auto y = yawOnlyQuat({s, 0.0f, 0.0f, s});
    CHECK(y.x == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(y.y == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(y.z == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(y.w == doctest::Approx(1.0f).epsilon(0.0001));
}

TEST_CASE("yawOnlyQuat: keeps the yaw component and renormalises") {
    // Pure +90° yaw must pass through unchanged and stay unit-length.
    constexpr float s = 0.70710678f;
    const auto y = yawOnlyQuat({0.0f, s, 0.0f, s});
    CHECK(y.x == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(y.y == doctest::Approx(s).epsilon(0.0001));
    CHECK(y.z == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(y.w == doctest::Approx(s).epsilon(0.0001));
    const float norm = std::sqrt(y.x*y.x + y.y*y.y + y.z*y.z + y.w*y.w);
    CHECK(norm == doctest::Approx(1.0f).epsilon(0.0001));
}

TEST_CASE("composeAnchorPose: pitch raises/lowers the panel but keeps it upright") {
    // Head looking straight UP (90° about X) at eye height. The panel must
    // be upright (identity orientation — roll/pitch dropped from the
    // orientation), but its POSITION follows the gaze: the 1 m forward
    // offset now points up, so the panel anchors a metre HIGHER, not at eye
    // level. This is the "aim its height with your gaze" behaviour (#10).
    constexpr float s = 0.70710678f;
    const OverlayPose head{{s, 0.0f, 0.0f, s}, {0.0f, 1.6f, 0.0f}};
    const OverlayVec3 offset{0.0f, 0.0f, -1.0f};   // 1 m forward in head frame
    const auto a = composeAnchorPose(head, offset);
    // Upright panel (yaw-only orientation; pure pitch → identity).
    CHECK(a.orientation.x == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(a.orientation.y == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(a.orientation.z == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(a.orientation.w == doctest::Approx(1.0f).epsilon(0.0001));
    // Position followed the gaze upward: +1 m in Y, no longer 1 m forward.
    CHECK(a.position.x == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(a.position.y == doctest::Approx(2.6f).epsilon(0.0001));
    CHECK(a.position.z == doctest::Approx(0.0f).epsilon(0.0001));
}

TEST_CASE("composeAnchorPose: level head places the panel forward at eye height") {
    // Sanity counterpart: looking straight ahead, the panel sits 1 m forward
    // at the head's height (no vertical shift) and upright.
    const OverlayPose head{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.6f, 0.0f}};
    const auto a = composeAnchorPose(head, {0.0f, 0.0f, -1.0f});
    CHECK(a.position.x == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(a.position.y == doctest::Approx(1.6f).epsilon(0.0001));
    CHECK(a.position.z == doctest::Approx(-1.0f).epsilon(0.0001));
    CHECK(a.orientation.w == doctest::Approx(1.0f).epsilon(0.0001));
}

// =============================================================================
// normaliseBar — sample → [0, 1] for histogram drawing.
// =============================================================================

TEST_CASE("normaliseBar: empty ring (max=0) returns 0") {
    CHECK(normaliseBar(/*sample=*/123, /*max=*/0) == doctest::Approx(0.0f));
}

TEST_CASE("normaliseBar: sample == max → 1.0") {
    CHECK(normaliseBar(50, 50) == doctest::Approx(1.0f));
}

TEST_CASE("normaliseBar: half-of-max → 0.5") {
    CHECK(normaliseBar(50, 100) == doctest::Approx(0.5f).epsilon(0.001));
}

TEST_CASE("normaliseBar: zero sample → 0") {
    CHECK(normaliseBar(0, 100) == doctest::Approx(0.0f));
}

TEST_CASE("normaliseBar: negative sample (out-of-order ticks etc.) → 0") {
    CHECK(normaliseBar(-50, 100) == doctest::Approx(0.0f));
}

TEST_CASE("normaliseBar: sample > max clamps to 1.0 (no overflow)") {
    CHECK(normaliseBar(200, 100) == doctest::Approx(1.0f));
}

// =============================================================================
// barVisualForSample — budget-anchored fpsVR-style normalisation.
//
// Y axis spans 0..1.2× budget. A frame at exactly 100 % of budget fills
// 5/6 ≈ 0.833 of the strip height (= ratio / 1.2); the budget reference
// line sits at that same Y position (= 1/6 from the top). Frames above
// 120 % budget saturate at the strip's top — the visible TIER (orange /
// red) tells the user "how bad", the bar's SHAPE tells "how often".
// Tier transitions remain at 80 % (orange) and 90 % (red).
// =============================================================================

TEST_CASE("barVisualForSample: budget==0 returns zero-height green") {
    const auto v = barVisualForSample(/*sample=*/5'000'000, /*budget=*/0);
    CHECK(v.heightFraction == doctest::Approx(0.0f));
    CHECK(v.tier == BarTier::Green);
}

TEST_CASE("barVisualForSample: negative or zero sample returns zero-height green") {
    CHECK(barVisualForSample(0,     11'111'111).heightFraction == doctest::Approx(0.0f));
    CHECK(barVisualForSample(-100,  11'111'111).heightFraction == doctest::Approx(0.0f));
    CHECK(barVisualForSample(0,     11'111'111).tier           == BarTier::Green);
}

TEST_CASE("barVisualForSample: sample == budget → ~83% strip + red") {
    // ratio = 1.0; heightFraction = 1.0/1.2 = 5/6 ≈ 0.833.
    const auto v = barVisualForSample(11'111'111, 11'111'111);
    CHECK(v.heightFraction == doctest::Approx(1.0f / 1.2f).epsilon(0.001));
    CHECK(v.tier == BarTier::Red);
}

TEST_CASE("barVisualForSample: sample at half-budget → ~42% strip + green") {
    // ratio = 0.5; heightFraction = 0.5 / 1.2 ≈ 0.417.
    const auto v = barVisualForSample(5'555'555, 11'111'111);
    CHECK(v.heightFraction == doctest::Approx(0.5f / 1.2f).epsilon(0.001));
    CHECK(v.tier == BarTier::Green);
}

TEST_CASE("barVisualForSample: tier thresholds are 80% orange / 90% red (headroom-based)") {
    // The "headroom warning" tiering:
    //   ratio < 0.80 → Green   (≥ 20 % headroom — fine)
    //   ratio < 0.90 → Orange  (10–20 % headroom — warning)
    //   ratio ≥ 0.90 → Red     ( < 10 % headroom — critical)
    constexpr int64_t budget = 10'000'000;  // 10 ms — clean tenths
    CHECK(barVisualForSample(7'900'000, budget).tier == BarTier::Green);
    CHECK(barVisualForSample(8'000'000, budget).tier == BarTier::Orange);
    CHECK(barVisualForSample(8'900'000, budget).tier == BarTier::Orange);
    CHECK(barVisualForSample(9'000'000, budget).tier == BarTier::Red);
    CHECK(barVisualForSample(budget,    budget).tier == BarTier::Red);
    CHECK(barVisualForSample(2 * budget, budget).tier == BarTier::Red);
}

TEST_CASE("barVisualForSample: 1.2× budget saturates at full height (Y-axis cap)") {
    const auto v = barVisualForSample(13'333'333, 11'111'111);
    CHECK(v.heightFraction == doctest::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("barVisualForSample: 2× budget still clamped at full height") {
    // The 0..1.2× Y-axis trade-off: any frame above 1.2× budget
    // saturates visually at the strip's top, indistinguishable from
    // each other (2× looks the same as 4×). The TIER colour stays
    // Red regardless; the user reads "how bad" from the colour, not
    // from the bar height beyond 100 % budget.
    const auto v = barVisualForSample(22'222'222, 11'111'111);
    CHECK(v.heightFraction == doctest::Approx(1.0f));
    CHECK(v.tier == BarTier::Red);
}

TEST_CASE("barVisualForSample: 4× budget still clamped at full height") {
    const auto v = barVisualForSample(44'444'444, 11'111'111);
    CHECK(v.heightFraction == doctest::Approx(1.0f));
    CHECK(v.tier == BarTier::Red);
}

TEST_CASE("budgetLineFraction: at the bar-tip position for ratio=1.0 (1/6 from top)") {
    // The reference line MUST sit where a budget-equal bar's tip
    // lands. With heightFraction = 1/1.2 = 5/6 (= fraction of strip
    // height the bar occupies, growing from bottom), the tip Y is
    // at strip_top + (1 - 5/6) * stripH = strip_top + (1/6) *
    // stripH. Same math here keeps the marker visually aligned to
    // the bar tip at budget.
    CHECK(budgetLineFraction() == doctest::Approx(1.0f / 6.0f).epsilon(0.001));
}

// =============================================================================
// gaugeTierForUtilisation — drives the circular gauge's fill colour.
// Same threshold semantics as barVisualForSample's bar tier, but the input is
// a utilisation FRACTION (0..1) rather than a frametime/budget ratio. Same
// boundaries: 0.80 = warning (Orange), 0.90 = critical (Red).
// =============================================================================

using openxr_api_layer::detail::gaugeTierForUtilisation;

TEST_CASE("gaugeTierForUtilisation: <80 % → green (default healthy state)") {
    CHECK(gaugeTierForUtilisation(0.0f)  == BarTier::Green);
    CHECK(gaugeTierForUtilisation(0.5f)  == BarTier::Green);
    CHECK(gaugeTierForUtilisation(0.79f) == BarTier::Green);
}

TEST_CASE("gaugeTierForUtilisation: 80–89 % → orange (warning)") {
    CHECK(gaugeTierForUtilisation(0.80f) == BarTier::Orange);
    CHECK(gaugeTierForUtilisation(0.85f) == BarTier::Orange);
    CHECK(gaugeTierForUtilisation(0.89f) == BarTier::Orange);
}

TEST_CASE("gaugeTierForUtilisation: ≥ 90 % → red (critical)") {
    CHECK(gaugeTierForUtilisation(0.90f) == BarTier::Red);
    CHECK(gaugeTierForUtilisation(0.95f) == BarTier::Red);
    CHECK(gaugeTierForUtilisation(1.00f) == BarTier::Red);
    // Even out-of-range values stay clamped to the worst tier.
    CHECK(gaugeTierForUtilisation(2.50f) == BarTier::Red);
}

TEST_CASE("gaugeTierForUtilisation: NaN defaults to green (safe default)") {
    const float kNan = std::numeric_limits<float>::quiet_NaN();
    CHECK(gaugeTierForUtilisation(kNan) == BarTier::Green);
}
