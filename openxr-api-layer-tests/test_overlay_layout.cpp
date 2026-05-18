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
//   - formatOverlayRows (snapshot → 3 (left, right) pairs)
//   - geometryForPosition (settings.position → quad pose/size)
//   - normaliseBar (sample / max → [0, 1] bar height, legacy helper)
//   - barVisualForSample (sample / budget → (heightFraction, BarTier))
//   - budgetLineFraction (reference line position in the strip)
// =============================================================================

#include <doctest/doctest.h>

#include <limits>

#include "utils/overlay_layout.h"

using openxr_api_layer::detail::OverlaySnapshot;
using openxr_api_layer::detail::OverlayRow;
using openxr_api_layer::detail::formatOverlayRows;
using openxr_api_layer::detail::geometryForPosition;
using openxr_api_layer::detail::normaliseBar;
using openxr_api_layer::detail::BarTier;
using openxr_api_layer::detail::barVisualForSample;
using openxr_api_layer::detail::budgetLineFraction;

// =============================================================================
// formatOverlayRows — two-column fpsVR-style text formatting.
// Returns 3 (left, right) pairs:
//   row 0: FPS / AVG
//   row 1: GPU frametime / CPU frametime
//   row 2: GPU util / CPU util
// =============================================================================

TEST_CASE("formatOverlayRows: invalid snapshot → empty vector") {
    OverlaySnapshot snap;  // valid=false by default
    CHECK(formatOverlayRows(snap).empty());
}

TEST_CASE("formatOverlayRows: nominal snapshot produces 4 rows × 2 columns") {
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant = 89.8f;
    snap.fps_avg = 90.1f;
    snap.target_fps = 90.0f;
    snap.cpu_frame_ms = 6.78f;
    snap.cpu_utilisation_pct = 61.0f;
    snap.gpu_frame_ms = 5.18f;
    snap.gpu_utilisation_pct = 47.0f;

    // 4 rows since the GPU-temp/VRAM row was added; row 3's specific
    // formatting has its own dedicated tests below. Here we just
    // assert the row count contract — analysis: the renderer guards
    // on `rows.size() >= 4` before drawing the new row, so the
    // formatter must always return 4 rows from a valid snapshot.
    const auto rows = formatOverlayRows(snap);
    REQUIRE(rows.size() == 4);

    // Row 0: FPS (left) / AVG (right). Renderer draws the left column
    // anchored to the left half of the quad — GPU stuff goes there
    // visually, but the FPS row pairs FPS-instant with FPS-avg, not
    // GPU-with-anything.
    CHECK(rows[0].left.find("FPS")   != std::string::npos);
    CHECK(rows[0].left.find("89.8")  != std::string::npos);
    CHECK(rows[0].left.find("90.0")  != std::string::npos);
    CHECK(rows[0].right.find("AVG")  != std::string::npos);
    CHECK(rows[0].right.find("90.1") != std::string::npos);

    // Row 1: GPU frametime (left) / CPU frametime (right).
    CHECK(rows[1].left.find("GPU")   != std::string::npos);
    CHECK(rows[1].left.find("5.18")  != std::string::npos);
    CHECK(rows[1].left.find("ms")    != std::string::npos);
    CHECK(rows[1].right.find("CPU")  != std::string::npos);
    CHECK(rows[1].right.find("6.78") != std::string::npos);
    CHECK(rows[1].right.find("ms")   != std::string::npos);

    // Row 2: GPU util (left) / CPU util (right).
    CHECK(rows[2].left.find("GPU")   != std::string::npos);
    CHECK(rows[2].left.find("47")    != std::string::npos);
    CHECK(rows[2].left.find("%")     != std::string::npos);
    CHECK(rows[2].right.find("CPU")  != std::string::npos);
    CHECK(rows[2].right.find("61")   != std::string::npos);
    CHECK(rows[2].right.find("%")    != std::string::npos);
}

TEST_CASE("formatOverlayRows: zero target_fps still renders without crashing") {
    // Edge case: a runtime that briefly fails to advertise display
    // period → snapshot.target_fps stays 0. The label should still
    // render (the renderer prefers a stable layout over hiding the row).
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant = 60.0f;
    snap.target_fps = 0.0f;
    const auto rows = formatOverlayRows(snap);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].left.find("FPS") != std::string::npos);
    // 0.0 must appear (the "/ 0.0" component of "FPS  60.0 /  0.0").
    CHECK(rows[0].left.find("0.0") != std::string::npos);
}

TEST_CASE("formatOverlayRows: row 3 carries GPU temp + VRAM when both sources are present") {
    // GPU temp + VRAM are populated by the aggregator from
    // pushGpuTelemetry (NvAPI + DXGI on Windows). The layout's row 3
    // mirrors fpsVR's per-column convention: temp under the GPU
    // column, VRAM under the CPU column. Both cells must render with
    // unit suffixes so the user reads them without a legend.
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant = 90.0f;
    snap.fps_avg = 90.0f;
    snap.target_fps = 90.0f;
    snap.cpu_frame_ms = 8.0f;
    snap.gpu_frame_ms = 9.0f;
    snap.cpu_utilisation_pct = 70.0f;
    snap.gpu_utilisation_pct = 75.0f;
    snap.gpu_temp_c = 68.0f;
    snap.vram_used_bytes   = 6'000'000'000ULL;   // ~5.6 GB
    snap.vram_budget_bytes = 8'000'000'000ULL;   // ~7.5 GB

    const auto rows = formatOverlayRows(snap);
    REQUIRE(rows.size() == 4);
    // Left cell: "GPU 68 C". The temperature comes through %3.0f
    // (no decimals — 1° resolution is below human-meaningful flicker).
    CHECK(rows[3].left.find("GPU") != std::string::npos);
    CHECK(rows[3].left.find("68") != std::string::npos);
    CHECK(rows[3].left.find("C") != std::string::npos);
    // Right cell: "VRAM 5.6 / 7.5 GB" — used / budget with 1
    // decimal place each.
    CHECK(rows[3].right.find("VRAM") != std::string::npos);
    CHECK(rows[3].right.find("5.6") != std::string::npos);
    CHECK(rows[3].right.find("7.5") != std::string::npos);
    CHECK(rows[3].right.find("GB") != std::string::npos);
}

TEST_CASE("formatOverlayRows: row 3 placeholder when neither source is available") {
    // Non-NVIDIA host: NvAPI absent → gpu_temp_c stays NaN, and the
    // DXGI VRAM path may fail too on pre-RS1 Win10 → vram_used_bytes
    // stays 0. Both cells should render the "--" placeholder; no
    // "nan" / "inf" tokens leak through.
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant = 90.0f;
    snap.fps_avg = 90.0f;
    snap.target_fps = 90.0f;
    // gpu_temp_c default = NaN.
    // vram_used_bytes default = 0.

    const auto rows = formatOverlayRows(snap);
    REQUIRE(rows.size() == 4);
    CHECK(rows[3].left.find("--") != std::string::npos);
    CHECK(rows[3].left.find("nan") == std::string::npos);
    CHECK(rows[3].right.find("--") != std::string::npos);
    // No "0.0 GB" — the formatter must distinguish "unknown" from
    // "zero usage".
    CHECK(rows[3].right.find("0.0 GB") == std::string::npos);
}

TEST_CASE("formatOverlayRows: row 3 with VRAM used but no budget shows used alone") {
    // Some adapters / driver versions return CurrentUsage but not a
    // sensible Budget (e.g. headless test adapters). The formatter
    // falls back to "VRAM x.x GB" without the slash separator.
    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant = 90.0f;
    snap.fps_avg = 90.0f;
    snap.target_fps = 90.0f;
    snap.gpu_temp_c = 65.0f;
    snap.vram_used_bytes   = 2'000'000'000ULL;  // ~1.9 GB
    snap.vram_budget_bytes = 0;

    const auto rows = formatOverlayRows(snap);
    REQUIRE(rows.size() == 4);
    CHECK(rows[3].right.find("1.9") != std::string::npos);
    // No slash separator when budget is missing.
    CHECK(rows[3].right.find("/") == std::string::npos);
}

TEST_CASE("formatOverlayRows: non-finite snapshot floats render as '--' placeholders") {
    // Guard contract — the aggregator can briefly accumulate a NaN or
    // ±Inf if QPC samples are mis-ordered (negative
    // `frame_total - wait_block`), and %5.1f would render that as
    // " nan" / " inf" / "-inf" — wrong width, breaks the monospace
    // column grid. formatOverlayRows is the single chokepoint that
    // catches the bad floats and substitutes a fixed-width sentinel.
    const float kNan = std::numeric_limits<float>::quiet_NaN();
    const float kInf = std::numeric_limits<float>::infinity();

    OverlaySnapshot snap;
    snap.valid = true;
    snap.fps_instant         = kNan;
    snap.fps_avg             = kInf;
    snap.cpu_frame_ms        = kNan;
    snap.gpu_frame_ms        = -kInf;
    snap.cpu_utilisation_pct = kNan;
    snap.gpu_utilisation_pct = kInf;
    snap.target_fps          = kNan;

    const auto rows = formatOverlayRows(snap);
    REQUIRE(rows.size() == 4);
    // No "nan" / "inf" tokens anywhere in the formatted output —
    // the placeholder strings replace them. Row 3 (GPU temp / VRAM)
    // also gets "--" cells because gpu_temp_c stays NaN and
    // vram_used_bytes stays 0 in this fixture.
    for (const auto& row : rows) {
        CHECK(row.left.find("nan")  == std::string::npos);
        CHECK(row.left.find("inf")  == std::string::npos);
        CHECK(row.right.find("nan") == std::string::npos);
        CHECK(row.right.find("inf") == std::string::npos);
        // The "--" placeholder is present in every cell.
        CHECK(row.left.find("--")  != std::string::npos);
        CHECK(row.right.find("--") != std::string::npos);
    }
}

// =============================================================================
// geometryForPosition — quad pose lookup.
// =============================================================================

TEST_CASE("geometryForPosition: default head_top_right → +X, +Y, -Z") {
    const auto g = geometryForPosition("head_top_right", 1.0f);
    CHECK(g.pos_x > 0.0f);      // right of view origin
    CHECK(g.pos_y > 0.0f);      // above
    CHECK(g.pos_z < 0.0f);      // in front
    CHECK(g.width_m > 0.0f);
    CHECK(g.height_m > 0.0f);
    // Default dimensions documented in the header. The 0.091 m
    // height is the proportional bump from the 96 → 116 px texture
    // resize (added GPU temp / VRAM row 4 in this PR); pixel density
    // is preserved so the text reads at the same apparent size in
    // the HMD.
    CHECK(g.width_m  == doctest::Approx(0.20f).epsilon(0.001));
    CHECK(g.height_m == doctest::Approx(0.091f).epsilon(0.001));
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
// barVisualForSample — budget-anchored fpsvr-style normalisation.
//
// Y axis spans 0..2× budget. Budget itself sits at the midpoint
// (heightFraction = 0.5). Tier transitions are 80 % and 100 % of budget.
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

TEST_CASE("barVisualForSample: sample == budget → midpoint + red") {
    const auto v = barVisualForSample(11'111'111, 11'111'111);
    CHECK(v.heightFraction == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(v.tier == BarTier::Red);
}

TEST_CASE("barVisualForSample: sample at half-budget → quarter-height + green") {
    const auto v = barVisualForSample(5'555'555, 11'111'111);
    CHECK(v.heightFraction == doctest::Approx(0.25f).epsilon(0.001));
    CHECK(v.tier == BarTier::Green);
}

TEST_CASE("barVisualForSample: tier thresholds are strict (80% yellow, 100% red)") {
    // Use a budget divisible by 100 so the percentage→ns conversion
    // stays in integer arithmetic — multiplying by 0.80 then
    // casting to int64 truncates 11'111'111 × 0.80 to 8'888'888,
    // which works out to 0.79999... of budget and slips into the
    // Green tier on a `ratio >= 0.8` strict-greater comparison.
    constexpr int64_t budget = 10'000'000;  // 10 ms — clean tenths

    // 79 % → green
    CHECK(barVisualForSample(7'900'000, budget).tier == BarTier::Green);
    // exactly 80 % → yellow
    CHECK(barVisualForSample(8'000'000, budget).tier == BarTier::Yellow);
    // 99 % → still yellow
    CHECK(barVisualForSample(9'900'000, budget).tier == BarTier::Yellow);
    // exactly 100 % → red
    CHECK(barVisualForSample(budget,    budget).tier == BarTier::Red);
    // 200 % → red
    CHECK(barVisualForSample(2 * budget, budget).tier == BarTier::Red);
}

TEST_CASE("barVisualForSample: 2× budget saturates at full height") {
    const auto v = barVisualForSample(22'222'222, 11'111'111);
    CHECK(v.heightFraction == doctest::Approx(1.0f));
}

TEST_CASE("barVisualForSample: 4× budget still clamped at full height") {
    // A genuine stutter spike (e.g., loading screen on top of the
    // running frame loop) must not overflow the strip area visually.
    const auto v = barVisualForSample(44'444'444, 11'111'111);
    CHECK(v.heightFraction == doctest::Approx(1.0f));
    CHECK(v.tier == BarTier::Red);
}

TEST_CASE("budgetLineFraction: anchored at the strip midpoint") {
    // The reference line MUST sit where a budget-equal bar would
    // reach. Both come from the same 2× budget Y-axis convention.
    CHECK(budgetLineFraction() == doctest::Approx(0.5f));
}
