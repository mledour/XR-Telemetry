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
// test_overlay_aggregator.cpp — unit tests on the moving-average aggregator.
// Drives synthetic FrameRecord sequences through OverlayAggregator and checks
// that the published Snapshot reflects the right averages at the right
// cadence. Pure tests — no QPC, no OpenXR, no rendering.
//
// Almost every test relies on the constructor's qpcFrequency default of
// 1 GHz (1 tick = 1 ns), which makes timestamp_qpc map directly to a
// nanosecond count. The one test that exercises a non-default frequency
// passes 10 MHz explicitly to confirm the tick → ns scaling math.
// =============================================================================

#include <doctest/doctest.h>

#include "utils/overlay_aggregator.h"

using openxr_api_layer::detail::FrameRecord;
using openxr_api_layer::detail::OverlayAggregator;
using openxr_api_layer::detail::OverlaySnapshot;

namespace {
    // Construct a FrameRecord with the fields the aggregator actually reads.
    // Other fields stay zero — the aggregator never reads them.
    //
    // wait_block_ns matters because cpu_frame_ms is computed as
    // (frame_total - wait_block), so tests asserting specific cpu_frame_ms
    // values need to set both. Defaults to 0 for tests that only care
    // about fps or refresh cadence.
    FrameRecord makeRecord(int64_t qpc_ticks,
                            int64_t app_cpu_ns,
                            int64_t gpu_time_ns,
                            int64_t frame_total_ns,
                            int64_t period_ns,
                            float headroom_pct,
                            float gpu_headroom_pct,
                            int64_t wait_block_ns = 0) {
        FrameRecord r{};
        r.timestamp_qpc   = qpc_ticks;
        r.app_cpu_ns      = app_cpu_ns;
        r.gpu_time_ns     = gpu_time_ns;
        r.frame_total_ns  = frame_total_ns;
        r.period_ns       = period_ns;
        r.headroom_pct    = headroom_pct;
        r.gpu_headroom_pct = gpu_headroom_pct;
        r.wait_block_ns   = wait_block_ns;
        return r;
    }
}

// =============================================================================
// Initial state — snapshot is invalid before any frame, and stays invalid
// before the first refresh tick has fired.
// =============================================================================

TEST_CASE("OverlayAggregator: snapshot is invalid before any pushFrame") {
    OverlayAggregator agg;
    CHECK_FALSE(agg.snapshot().valid);
    // Version stays at 0 until the first publish, so a fresh
    // CoreRenderer cache (also default-zero) matches and skips
    // formatting an empty snapshot. Documented contract.
    CHECK(agg.snapshot().version == 0);
}

TEST_CASE("OverlayAggregator: single frame does NOT publish a snapshot") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    agg.pushFrame(makeRecord(0, 4'000'000, 5'000'000, 11'111'111, 11'111'111, 64, 55));
    // The first frame arms the refresh clock; we never publish on N=1.
    CHECK_FALSE(agg.snapshot().valid);
}

TEST_CASE("OverlayAggregator: snapshot stays invalid when frames span < interval") {
    // 5 frames within ~55 ms, interval is 100 ms → no refresh.
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    for (int i = 0; i < 5; ++i) {
        agg.pushFrame(makeRecord(int64_t(i) * 11'111'111LL,
                                  4'000'000, 5'000'000,
                                  11'111'111, 11'111'111, 64, 55));
    }
    CHECK_FALSE(agg.snapshot().valid);
}

// =============================================================================
// First refresh — once enough wallclock has passed, the snapshot finalises.
// =============================================================================

TEST_CASE("OverlayAggregator: refresh fires once interval is crossed") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    // Frame 0: arms the clock at t=0. Frame 1: t=120 ms, crosses the 100 ms
    // window → publish.
    //
    // cpu_frame_ms = per_cycle = frame_total - wait_block. Setting
    // frame_total=11.11ms (90 fps target) and wait_block=7.11ms yields
    // per_cycle = 4ms, matching the expected assertion below.
    agg.pushFrame(makeRecord(0,
                              4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64.0f, 55.0f,
                              /*wait_block=*/7'111'111));
    agg.pushFrame(makeRecord(120'000'000,
                              4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64.0f, 55.0f,
                              /*wait_block=*/7'111'111));
    REQUIRE(agg.snapshot().valid);
    const auto& s = agg.snapshot();
    CHECK(s.cpu_frame_ms == doctest::Approx(4.0f).epsilon(0.001));
    CHECK(s.gpu_frame_ms == doctest::Approx(5.0f).epsilon(0.001));
    // 1e9 / 11.111e6 ≈ 90 fps.
    CHECK(s.fps_avg     == doctest::Approx(90.0f).epsilon(0.05));
    CHECK(s.fps_instant == doctest::Approx(90.0f).epsilon(0.05));
    CHECK(s.target_fps  == doctest::Approx(90.0f).epsilon(0.05));
    // utilisation = 100 - headroom.
    CHECK(s.cpu_utilisation_pct == doctest::Approx(36.0f).epsilon(0.001));
    CHECK(s.gpu_utilisation_pct == doctest::Approx(45.0f).epsilon(0.001));
}

// =============================================================================
// Moving average — values are averaged across the window, not the latest.
// =============================================================================

TEST_CASE("OverlayAggregator: first frame with frame_total_ns=0 falls back to app_cpu_ns") {
    // On the very first frame of a session, frame_total_ns is 0 (no
    // previous tEnd to subtract from). The aggregator must fall back to
    // app_cpu_ns for that frame's CPU contribution — otherwise the
    // window 1 average would include a `0 - wait_block` ≤ 0 value and
    // skew the displayed cpu_frame_ms low. Mirrors the identical fall
    // back inside xrEndFrame's headroom math.
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    // Frame 1: first-frame sentinel (frame_total=0, app_cpu=3ms).
    agg.pushFrame(makeRecord(0,
                              /*app_cpu=*/3'000'000, 5'000'000,
                              /*frame_total=*/0, 11'111'111, 64.0f, 55.0f));
    // Frame 2: full cycle established, per_cycle = 5ms via wait_block.
    agg.pushFrame(makeRecord(120'000'000,
                              /*app_cpu=*/3'000'000, 5'000'000,
                              11'111'111, 11'111'111, 55.0f, 55.0f,
                              /*wait_block=*/6'111'111));
    REQUIRE(agg.snapshot().valid);
    // (3 [from app_cpu fallback] + 5 [from per_cycle]) / 2 = 4 ms
    CHECK(agg.snapshot().cpu_frame_ms == doctest::Approx(4.0f).epsilon(0.001));
}

TEST_CASE("OverlayAggregator: averages across the window, not just last frame") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    // Vary wait_block so per_cycle differs across the two frames:
    // Frame 1: frame_total=11.11ms, wait_block=9.11ms → per_cycle=2ms.
    // Frame 2: frame_total=11.11ms, wait_block=5.11ms → per_cycle=6ms.
    // Average → 4ms, matching the assertion below.
    agg.pushFrame(makeRecord(0,
                              /*app_cpu=*/0, 5'000'000,
                              11'111'111, 11'111'111, 82.0f, 55.0f,
                              /*wait_block=*/9'111'111));
    // Second frame at t=120 ms (crosses 100 ms window).
    agg.pushFrame(makeRecord(120'000'000,
                              /*app_cpu=*/0, 5'000'000,
                              11'111'111, 11'111'111, 46.0f, 55.0f,
                              /*wait_block=*/5'111'111));
    REQUIRE(agg.snapshot().valid);
    // (2 + 6) / 2 = 4 ms
    CHECK(agg.snapshot().cpu_frame_ms == doctest::Approx(4.0f).epsilon(0.001));
    // (82 + 46) / 2 = 64 → utilisation 36%
    CHECK(agg.snapshot().cpu_utilisation_pct == doctest::Approx(36.0f).epsilon(0.001));
}

TEST_CASE("OverlayAggregator: fps_instant tracks the LATEST frame, fps_avg the window") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    // Push at t=0 (arms clock), t=50ms, t=110ms (refresh).
    agg.pushFrame(makeRecord(0,
                              4'000'000, 5'000'000,
                              /*frame_total=*/8'333'333,  // ~120 fps
                              8'333'333, 64.0f, 55.0f));
    agg.pushFrame(makeRecord(50'000'000,
                              4'000'000, 5'000'000,
                              /*frame_total=*/8'333'333,
                              8'333'333, 64.0f, 55.0f));
    agg.pushFrame(makeRecord(110'000'000,
                              4'000'000, 5'000'000,
                              /*frame_total=*/16'666'666,  // ~60 fps, single stutter frame
                              8'333'333, 64.0f, 55.0f));
    REQUIRE(agg.snapshot().valid);
    // Latest frame's frame_total = 16.67 ms → 60 fps.
    CHECK(agg.snapshot().fps_instant == doctest::Approx(60.0f).epsilon(0.5));
    // Mean frame_total = (8.33 + 8.33 + 16.67) / 3 ≈ 11.11 ms → ~90 fps.
    CHECK(agg.snapshot().fps_avg     == doctest::Approx(90.0f).epsilon(2.0));
    // target_fps comes from period_ns, NOT frame_total → still 120.
    CHECK(agg.snapshot().target_fps  == doctest::Approx(120.0f).epsilon(0.5));
}

// =============================================================================
// Clamping — pathological inputs don't escape the [0, 100] utilisation range.
// =============================================================================

TEST_CASE("OverlayAggregator: utilisation is clamped to [0, 100]") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    // Negative headroom (CPU-bound frame) → util > 100 without clamp.
    agg.pushFrame(makeRecord(0, 0, 0, 1, 1, /*headroom=*/-50.0f, /*gpu_headroom=*/-200.0f));
    agg.pushFrame(makeRecord(120'000'000, 0, 0, 1, 1, -50.0f, -200.0f));
    REQUIRE(agg.snapshot().valid);
    CHECK(agg.snapshot().cpu_utilisation_pct <= 100.0f);
    CHECK(agg.snapshot().gpu_utilisation_pct <= 100.0f);
    CHECK(agg.snapshot().cpu_utilisation_pct == doctest::Approx(100.0f).epsilon(0.001));
    CHECK(agg.snapshot().gpu_utilisation_pct == doctest::Approx(100.0f).epsilon(0.001));
}

TEST_CASE("OverlayAggregator: target_fps is 0 when period_ns is 0 (no measurement yet)") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    agg.pushFrame(makeRecord(0, 4'000'000, 5'000'000, 11'111'111, /*period=*/0, 100, 100));
    agg.pushFrame(makeRecord(120'000'000, 4'000'000, 5'000'000, 11'111'111, 0, 100, 100));
    REQUIRE(agg.snapshot().valid);
    CHECK(agg.snapshot().target_fps == doctest::Approx(0.0f).epsilon(0.001));
}

// =============================================================================
// Multiple refresh windows — the aggregator resets between publish ticks.
// =============================================================================

TEST_CASE("OverlayAggregator: second window's values do NOT include first window's frames") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    // Window 1: arm at t=0, mid-window frame at t=50 ms (no publish), then
    // a third frame at t=100 ms that's exactly on the boundary → publish
    // with avg(4, 4, 4) = 4 ms. wait_block=7.11ms keeps per_cycle = 4ms.
    agg.pushFrame(makeRecord(0,
                              4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64.0f, 55.0f,
                              /*wait_block=*/7'111'111));
    agg.pushFrame(makeRecord(50'000'000,
                              4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64.0f, 55.0f,
                              /*wait_block=*/7'111'111));
    agg.pushFrame(makeRecord(100'000'000,
                              4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64.0f, 55.0f,
                              /*wait_block=*/7'111'111));
    REQUIRE(agg.snapshot().valid);
    CHECK(agg.snapshot().cpu_frame_ms == doctest::Approx(4.0f).epsilon(0.001));

    // Window 2: first frame at t=150 ms — already 50 ms into window 2 but
    // still < 100 ms since last refresh → accumulates, no publish, the
    // public snapshot still reads 4 ms from window 1. wait_block=1.11ms
    // makes per_cycle = 10 ms for the window 2 frames.
    agg.pushFrame(makeRecord(150'000'000,
                              10'000'000, 5'000'000,
                              11'111'111, 11'111'111, 10.0f, 55.0f,
                              /*wait_block=*/1'111'111));
    CHECK(agg.snapshot().cpu_frame_ms == doctest::Approx(4.0f).epsilon(0.001));

    // Window 2 second frame at t=200 ms → 100 ms since last refresh
    // exactly, publish with avg(10, 10) = 10 ms (window 1's 4 ms NEVER
    // contributes here — that's the whole point of the reset).
    agg.pushFrame(makeRecord(200'000'000,
                              10'000'000, 5'000'000,
                              11'111'111, 11'111'111, 10.0f, 55.0f,
                              /*wait_block=*/1'111'111));
    CHECK(agg.snapshot().cpu_frame_ms == doctest::Approx(10.0f).epsilon(0.001));
}

// =============================================================================
// Refresh-interval boundary — exactly the interval should fire, just-under
// should not.
// =============================================================================

TEST_CASE("OverlayAggregator: a frame exactly at the interval boundary triggers publish") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    agg.pushFrame(makeRecord(0, 4'000'000, 5'000'000, 11'111'111, 11'111'111, 64, 55));
    agg.pushFrame(makeRecord(100'000'000,
                              4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    CHECK(agg.snapshot().valid);
}

TEST_CASE("OverlayAggregator: a frame at interval - 1 ns does NOT publish") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    agg.pushFrame(makeRecord(0, 4'000'000, 5'000'000, 11'111'111, 11'111'111, 64, 55));
    agg.pushFrame(makeRecord(99'999'999,
                              4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    CHECK_FALSE(agg.snapshot().valid);
}

// =============================================================================
// QPC frequency — when the caller passes a non-default frequency (e.g. the
// real Windows QueryPerformanceFrequency, typically 10 MHz on modern
// hardware), the aggregator must convert ticks to nanoseconds correctly.
// =============================================================================

TEST_CASE("OverlayAggregator: pathological qpcFrequency falls back to a sane value") {
    // qpcFrequency < 1000 is nonsense in production (real Windows QPC
    // is always at least 1 MHz). The aggregator hardens by substituting
    // 10 MHz — the typical Windows QPC frequency on modern hardware,
    // and identical to the OpenXrLayer ctor's broken-HAL fallback so
    // the CSV-side and overlay-side timing stay numerically coherent.
    //
    // Under the 10 MHz substitution (= 100 ns per tick):
    //   - 500_000 ticks = 50 ms    → under 100 ms refresh interval
    //   - 1_200_000 ticks = 120 ms → above the interval, triggers publish
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL, /*qpcFreq=*/1);
    agg.pushFrame(makeRecord(0, 4'000'000, 5'000'000, 11'111'111, 11'111'111, 64, 55));
    agg.pushFrame(makeRecord(500'000, 4'000'000, 5'000'000, 11'111'111, 11'111'111, 64, 55));
    // 50 ms span → no publish even though freq=1 was passed.
    CHECK_FALSE(agg.snapshot().valid);
    agg.pushFrame(makeRecord(1'200'000, 4'000'000, 5'000'000, 11'111'111, 11'111'111, 64, 55));
    // 120 ms span → publish, because the clamp substituted 10 MHz.
    CHECK(agg.snapshot().valid);
}

TEST_CASE("OverlayAggregator: respects qpcFrequency when converting ticks → ns") {
    // 10 MHz QPC clock: 1 tick = 100 ns. 1_200_000 ticks = 120 ms.
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL, /*qpcFreq=*/10'000'000LL);
    agg.pushFrame(makeRecord(0, 4'000'000, 5'000'000, 11'111'111, 11'111'111, 64, 55));
    // 1_200_000 ticks * 100 ns = 120 ms, well above the 100 ms interval.
    agg.pushFrame(makeRecord(1'200'000,
                              4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    CHECK(agg.snapshot().valid);
}

// =============================================================================
// version — monotonic publish counter. The renderer caches its formatted
// row strings keyed on this value, so the contract is:
//   * version 0 = "no valid snapshot yet" (default-constructed)
//   * first publish bumps to 1, second to 2, etc.
//   * never resets, never decreases
// =============================================================================

// =============================================================================
// FPS percentiles (P95 / P99) — computed from a 5s sliding ring of
// frame_total_ns samples, sorted on each publish. The contract:
//
//   fps_p95 = FPS such that 95% of frames hit AT LEAST this value
//             (the slow 5% drop below)
//   fps_p99 = FPS such that 99% of frames hit AT LEAST this value
//             (the worst 1% drop below)
//
// Frametime is sorted ASCENDING; fps_p95 = 1e9 / frametime[at ascending
// index 95%]. Smaller percentile-FPS = worse N% of frames.
// =============================================================================

TEST_CASE("OverlayAggregator: P95 / P99 default to 0 before any publish") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    CHECK(agg.snapshot().fps_p95 == doctest::Approx(0.0f));
    CHECK(agg.snapshot().fps_p99 == doctest::Approx(0.0f));
}

TEST_CASE("OverlayAggregator: P95/P99 reflect frame_total_ns distribution") {
    // Push 100 frames, all at exactly 11.111 ms (= 90 FPS). Every
    // frame is the same, so P95 and P99 of FPS are both exactly 90.
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    constexpr int64_t kFrameNs = 11'111'111;
    for (int i = 0; i < 100; ++i) {
        agg.pushFrame(makeRecord(int64_t(i) * kFrameNs,
                                   4'000'000, 5'000'000,
                                   kFrameNs, kFrameNs, 64, 55));
    }
    // Span = 100 × 11.1 ms ≈ 1.1 s — multiple refresh ticks have fired.
    REQUIRE(agg.snapshot().valid);
    CHECK(agg.snapshot().fps_p95 == doctest::Approx(90.0f).epsilon(0.01));
    CHECK(agg.snapshot().fps_p99 == doctest::Approx(90.0f).epsilon(0.01));
}

TEST_CASE("OverlayAggregator: P99 < P95 < FPS_AVG when stutters present") {
    // 90 frames at 11.111 ms (90 FPS) + 10 frames at 33.333 ms (30 FPS).
    // 10 % stutter rate — slightly above the 5 % P95 threshold so the
    // boundary index lands ROBUSTLY in the slow region regardless of
    // tiny off-by-one swings in N (the kind of swing you get when a
    // forced-publish push at the end adds one more sample to the
    // ring).
    //
    // Why force a final publish: the aggregator publishes when the
    // delta since last refresh crosses 100 ms. With 11.111-ms /
    // 33.333-ms inputs, the LAST publish naturally lands a few frames
    // before the final push (delta = 99,999,999 ns just shy of the
    // threshold on the very last frame). To make the snapshot
    // reflect the FULL distribution we explicitly push one extra
    // frame with a 200-ms QPC gap — way over the refresh interval —
    // which guarantees a publish whose ring state has all 100 + 1
    // samples.
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    constexpr int64_t kFastNs = 11'111'111;   // 90 FPS
    constexpr int64_t kSlowNs = 33'333'333;   // 30 FPS
    int64_t cumQpc = 0;
    for (int i = 0; i < 90; ++i) {
        cumQpc += kFastNs;
        agg.pushFrame(makeRecord(cumQpc, 4'000'000, 5'000'000,
                                   kFastNs, kFastNs, 64, 55));
    }
    for (int i = 0; i < 10; ++i) {
        cumQpc += kSlowNs;
        agg.pushFrame(makeRecord(cumQpc, 4'000'000, 5'000'000,
                                   kSlowNs, kSlowNs, 64, 55));
    }
    // Forced-publish push — kicks the aggregator to publish with the
    // ring carrying 91 fast + 10 slow = 101 samples.
    cumQpc += 200'000'000;
    agg.pushFrame(makeRecord(cumQpc, 4'000'000, 5'000'000,
                               kFastNs, kFastNs, 64, 55));

    REQUIRE(agg.snapshot().valid);
    const float p95 = agg.snapshot().fps_p95;
    const float p99 = agg.snapshot().fps_p99;
    const float avg = agg.snapshot().fps_avg;
    // sorted_ascending(frametime) has 91 fast at idx 0..90, 10 slow
    // at idx 91..100. integer index 950*101/1000 = 95 → slow region.
    // 990*101/1000 = 99 → slow region. Both percentiles read 30 FPS.
    CHECK(p95 < avg);
    CHECK(p99 <= p95);
    CHECK(p95 == doctest::Approx(30.0f).epsilon(0.05));
    CHECK(p99 == doctest::Approx(30.0f).epsilon(0.05));
}

TEST_CASE("OverlayAggregator: P95/P99 ignore frame_total_ns=0 first-frame sentinels") {
    // First-frame sentinels (frame_total=0) would map to infinite FPS
    // and corrupt the percentile estimate. The aggregator drops them
    // from the ring.
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    // 50 sentinel frames (frame_total=0) — must not enter the ring.
    for (int i = 0; i < 50; ++i) {
        agg.pushFrame(makeRecord(int64_t(i) * 11'111'111LL,
                                   4'000'000, 5'000'000,
                                   /*frame_total=*/0, 11'111'111, 64, 55));
    }
    // 50 real frames at 11.111 ms.
    for (int i = 0; i < 50; ++i) {
        agg.pushFrame(makeRecord(int64_t(50 + i) * 11'111'111LL,
                                   4'000'000, 5'000'000,
                                   11'111'111, 11'111'111, 64, 55));
    }
    REQUIRE(agg.snapshot().valid);
    // P95 must be 90 FPS — if the sentinels had entered the ring at
    // their "infinite FPS" interpretation, the slow 5% boundary would
    // shift wildly.
    CHECK(agg.snapshot().fps_p95 == doctest::Approx(90.0f).epsilon(0.01));
}

TEST_CASE("OverlayAggregator: P95/P99 window cap at 720 samples") {
    // The percentile ring is 5 s @ 144 Hz = 720 samples. We push
    // 300 slow frames + many fast frames; the slow ones must rotate
    // out of the ring entirely by the time the LAST publish fires.
    //
    // Sizing: publishes fire every ~15 fast frames (100 ms refresh /
    // 6.94 ms per fast frame ≈ 14.4 → 15). To guarantee the LAST
    // publish sees an all-fast ring, the ring must be fully
    // overwritten with fast frames BEFORE that publish. Frame 1020
    // is when the 300th fast push overwrites the last slow position;
    // the next publish lands at frame ~1025. Push 900 fast frames
    // total — that gives several publishes after the all-fast point,
    // any of which is the "latest" snapshot the test reads.
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    int64_t cumQpc = 0;
    constexpr int64_t kSlowNs = 33'333'333;
    constexpr int64_t kFastNs = 6'944'444;   // 144 FPS
    for (int i = 0; i < 300; ++i) {
        cumQpc += kSlowNs;
        agg.pushFrame(makeRecord(cumQpc, 4'000'000, 5'000'000,
                                   kSlowNs, kSlowNs, 64, 55));
    }
    for (int i = 0; i < 900; ++i) {
        cumQpc += kFastNs;
        agg.pushFrame(makeRecord(cumQpc, 4'000'000, 5'000'000,
                                   kFastNs, kFastNs, 64, 55));
    }
    REQUIRE(agg.snapshot().valid);
    // Slow frames flushed — P95 / P99 reflect only the recent fast
    // frames. With the 720-sample ring fully populated by fast
    // samples, all entries are 144 FPS.
    CHECK(agg.snapshot().fps_p95 == doctest::Approx(144.0f).epsilon(0.05));
    CHECK(agg.snapshot().fps_p99 == doctest::Approx(144.0f).epsilon(0.05));
}

// =============================================================================
// GPU telemetry — pushGpuTelemetry latches the latest reading and the next
// publish copies it into the snapshot. NaN / 0 sentinels propagate through
// unchanged so the renderer can treat "no source available" specially.
// =============================================================================

TEST_CASE("OverlayAggregator: snapshot defaults to NaN temperature + 0 VRAM") {
    // No pushGpuTelemetry calls at all — the snapshot's GPU-extras
    // fields should signal "no data" via NaN / 0 even after a full
    // refresh tick fires. The renderer's isfinite() / != 0 guards
    // turn this into a "--" placeholder.
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    agg.pushFrame(makeRecord(0, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    agg.pushFrame(makeRecord(120'000'000, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    REQUIRE(agg.snapshot().valid);
    CHECK_FALSE(std::isfinite(agg.snapshot().gpu_temp_c));
    CHECK(agg.snapshot().vram_used_bytes == 0);
    CHECK(agg.snapshot().vram_budget_bytes == 0);
}

TEST_CASE("OverlayAggregator: pushGpuTelemetry latches values into next publish") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    agg.pushGpuTelemetry(/*temp_c=*/68.5f,
                          /*vram_used=*/6'000'000'000ULL,
                          /*vram_budget=*/8'000'000'000ULL);
    agg.pushFrame(makeRecord(0, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    agg.pushFrame(makeRecord(120'000'000, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    REQUIRE(agg.snapshot().valid);
    CHECK(agg.snapshot().gpu_temp_c == doctest::Approx(68.5f).epsilon(0.001));
    CHECK(agg.snapshot().vram_used_bytes   == 6'000'000'000ULL);
    CHECK(agg.snapshot().vram_budget_bytes == 8'000'000'000ULL);
}

TEST_CASE("OverlayAggregator: pushGpuTelemetry uses LATEST value, not average") {
    // Drivers update thermal counters at ~1 Hz, so averaging the
    // values across the refresh window would just average step-
    // function holds. The aggregator deliberately latches the
    // latest poll; this test pins that contract.
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    agg.pushGpuTelemetry(60.0f, 1'000'000'000ULL, 8'000'000'000ULL);
    agg.pushFrame(makeRecord(0, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    // Newer telemetry arrives mid-window.
    agg.pushGpuTelemetry(72.0f, 5'000'000'000ULL, 8'000'000'000ULL);
    agg.pushFrame(makeRecord(120'000'000, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    REQUIRE(agg.snapshot().valid);
    // 72°C — the latest — not (60+72)/2.
    CHECK(agg.snapshot().gpu_temp_c == doctest::Approx(72.0f).epsilon(0.001));
    CHECK(agg.snapshot().vram_used_bytes == 5'000'000'000ULL);
}

TEST_CASE("OverlayAggregator: version is 1 after the first publish") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    CHECK(agg.snapshot().version == 0);
    agg.pushFrame(makeRecord(0, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    // First frame arms the clock — no publish yet, version unchanged.
    CHECK(agg.snapshot().version == 0);
    agg.pushFrame(makeRecord(120'000'000, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    CHECK(agg.snapshot().valid);
    CHECK(agg.snapshot().version == 1);
}

TEST_CASE("OverlayAggregator: version increments by exactly 1 per refresh tick") {
    OverlayAggregator agg(/*refreshIntervalNs=*/100'000'000LL);
    // Arm.
    agg.pushFrame(makeRecord(0, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    // First publish → version 1.
    agg.pushFrame(makeRecord(110'000'000, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    REQUIRE(agg.snapshot().version == 1);
    // Same window: no publish, version unchanged.
    agg.pushFrame(makeRecord(150'000'000, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    CHECK(agg.snapshot().version == 1);
    // Second publish → version 2.
    agg.pushFrame(makeRecord(220'000'000, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    CHECK(agg.snapshot().version == 2);
    // Third publish → version 3.
    agg.pushFrame(makeRecord(330'000'000, 4'000'000, 5'000'000,
                              11'111'111, 11'111'111, 64, 55));
    CHECK(agg.snapshot().version == 3);
}
