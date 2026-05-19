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
// overlay_aggregator.h — pure moving-average helper that turns the per-frame
// FrameRecord stream into a periodic Snapshot suitable for an in-headset HUD.
//
// Why "aggregator" and not just "render once per frame":
//   - Per-frame numbers shimmer at 90/120 Hz; the user can't read them.
//   - fpsvr-style displays refresh at ~5-10 Hz, averaging the noise out.
//   - The renderer (PR2 — coming later) can call snapshot() in its own
//     refresh tick and get whatever the most recent finalised aggregate
//     was. No coupling between renderer cadence and aggregation cadence.
//
// Inputs: FrameRecord (from telemetry_internals.h). Outputs: Snapshot —
// fps_instant / fps_avg / cpu_frame_ms / gpu_frame_ms / cpu_utilisation_pct
// / gpu_utilisation_pct / target_fps + a `valid` flag that goes true after
// the first refresh tick.
//
// Stays self-contained — depends only on telemetry_internals.h (for the
// FrameRecord type + qpcToNs). No OpenXR or Windows headers, so tests
// drive it on macOS / Linux without the SDK stack.
// =============================================================================

#include "../telemetry_internals.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace openxr_api_layer::detail {

    // What the future renderer (PR2) will draw into the head-locked quad.
    // All fields are floats so the rendering code can pass them straight to
    // DirectWrite / a string formatter without further conversion.
    struct OverlaySnapshot {
        float fps_instant = 0;          // 1e9 / last frame_total_ns
        float fps_avg = 0;              // 1e9 / mean(frame_total_ns over the refresh window)
        // CPU time the app spends PER CYCLE (frame_total - wait_block).
        // Matches the metric used by `cpu_utilisation_pct` and by the
        // CSV's `headroom_pct` column, so the displayed time and util%
        // are coherent. Falls back to app_cpu_ns on the first frame
        // (no previous cycle yet), same as xrEndFrame does for
        // headroom computation.
        float cpu_frame_ms = 0;
        float gpu_frame_ms = 0;         // mean(gpu_time_ns) / 1e6
        float cpu_utilisation_pct = 0;  // 100 - mean(headroom_pct), clamped [0, 100]
        float gpu_utilisation_pct = 0;  // 100 - mean(gpu_headroom_pct), clamped [0, 100]
        float target_fps = 0;           // 1e9 / mean(period_ns), the runtime's predicted display rate
        // FPS percentiles over a sliding window (kPercentileWindowSize
        // samples — 5 s @ 144 Hz). Computed by sorting the recent
        // frame_total_ns ring and indexing at the 95th and 99th
        // percentile of the ASCENDING-sorted frametimes — the FPS
        // value reported is `1e9 / frametime_at_that_index`, so
        // smaller percentile-FPS means "worse N% of frames".
        //
        //   fps_p95 = 95% of frames hit at least this FPS
        //             (the slow 5% drop below)
        //   fps_p99 = 99% of frames hit at least this FPS
        //             (the worst 1% drop below)
        //
        // Same convention as fpsVR / SteamVR "Frame Timing".
        // 0 until the percentile window has at least one sample.
        float fps_p95 = 0;
        float fps_p99 = 0;
        // GPU telemetry, populated from GpuTelemetryReader::poll() via
        // OverlayAggregator::pushGpuTelemetry. NaN / 0 sentinels mean
        // "source unavailable" — see gpu_telemetry.h for the full
        // safety argument that 0 bytes is a usable VRAM "no data"
        // marker. The aggregator LATCHES the latest reading rather
        // than averaging across the refresh window: drivers only
        // update these counters at ~1 Hz, so averaging step-function
        // holds adds smearing without any new signal.
        float gpu_temp_c = std::numeric_limits<float>::quiet_NaN();
        uint64_t vram_used_bytes = 0;
        uint64_t vram_budget_bytes = 0;
        bool  valid = false;            // false until the first refresh tick has finalised
        // Monotonic publish counter — bumped by OverlayAggregator on
        // every successful refresh tick. Lets the renderer cheaply
        // detect "did the snapshot actually change?" (version compare
        // = 8 bytes) and reuse cached formatting work the rest of
        // the frame budget. Default 0 (matches the cache's default-
        // initialised state, so the first valid snapshot triggers
        // a fresh format).
        uint64_t version = 0;
    };

    class OverlayAggregator {
      public:
        // refreshIntervalNs: time between snapshot recomputes. Default 100 ms
        //                    (10 Hz). The choice matches the README's
        //                    "Overlay refresh rate" decision — fast enough
        //                    that the user feels the numbers track reality,
        //                    slow enough that they don't shimmer.
        // qpcFrequency:      pre-cached QueryPerformanceFrequency, used to
        //                    convert FrameRecord::timestamp_qpc into the
        //                    nanoseconds we use for the refresh deadline.
        //                    Defaulted to 1 GHz (1 tick = 1 ns) so unit
        //                    tests can pass timestamp_qpc as a plain
        //                    nanosecond count without computing a fake
        //                    frequency. Production always passes the
        //                    real QueryPerformanceFrequency cached on
        //                    OpenXrLayer.
        //
        //                    NOTE: do not default to 1 — qpcToNs treats
        //                    `ticks / freq` as seconds, so freq=1 would
        //                    interpret `timestamp_qpc=10_000_000` (10 ms
        //                    in ns) as 10 million SECONDS and trip every
        //                    refresh deadline immediately. The clamp
        //                    below catches that case and substitutes
        //                    10 MHz (matching OpenXrLayer's broken-HAL
        //                    fallback, see comment near the clamp).
        explicit OverlayAggregator(int64_t refreshIntervalNs = 100'000'000LL,
                                   int64_t qpcFrequency = 1'000'000'000LL) noexcept
            : m_intervalNs(refreshIntervalNs > 0 ? refreshIntervalNs : 1),
              // Clamp anything < 1 kHz to the 10 MHz fallback. Real
              // Windows QPC is always at least 1 MHz, usually exactly
              // 10 MHz on modern hardware; a caller somehow passing
              // 1 / 2 / 100 is either a test bug or a broken HAL clock
              // falling through OpenXrLayer's own fallback. Picking
              // 10 MHz here (vs. 1 GHz historically) matches the
              // OpenXrLayer constructor's identical broken-clock
              // fallback — so the CSV's qpcToNs path and the
              // aggregator's refresh-deadline path stay numerically
              // coherent (same period_ns → same fps_target). The
              // default argument above stays at 1 GHz on purpose: it's
              // the "1 tick = 1 ns" identity test convention, and no
              // production caller hits that path (OpenXrLayer always
              // passes the cached QueryPerformanceFrequency).
              m_qpcFrequency(qpcFrequency >= 1000 ? qpcFrequency : 10'000'000LL) {}

        // Push a single GpuTelemetrySample-equivalent reading. The
        // aggregator stores the latest valid temperature / VRAM
        // values and weaves them into the snapshot on the next
        // publish. Decoupled from pushFrame() because GPU telemetry
        // refreshes at the aggregator cadence (~10 Hz), not per
        // frame — calling NvAPI 90+ times/second would be wasted
        // work since the driver itself only updates these counters
        // ~once per second.
        //
        // NaN / 0 inputs are accepted (and re-published as NaN / 0
        // downstream); the format helpers handle the placeholders.
        void pushGpuTelemetry(float gpu_temp_c,
                              uint64_t vram_used_bytes,
                              uint64_t vram_budget_bytes) noexcept {
            m_latestGpuTempC      = gpu_temp_c;
            m_latestVramUsed      = vram_used_bytes;
            m_latestVramBudget    = vram_budget_bytes;
            m_gotGpuTelemetry     = true;
        }

        // Push one fully-resolved FrameRecord (post-GPU-patch — gpu_time_ns
        // is its final value, not the in-flight 0). The aggregator
        // accumulates internally; the snapshot is refreshed when the QPC
        // delta since the previous refresh exceeds refreshIntervalNs.
        void pushFrame(const FrameRecord& rec) noexcept {
            // Accumulate every per-frame field we'll need for the snapshot.
            //
            // `cpu_frame_ms` must align with `cpu_utilisation_pct` so the
            // HUD reads coherently (fpsvr convention: same metric for
            // displayed time + util%). headroom_pct is derived from
            // `app_per_cycle_ns = frame_total - wait_block` in
            // OpenXrLayer::xrEndFrame, so we accumulate THE SAME quantity
            // here — NOT rec.app_cpu_ns (which is just the wait→end
            // sub-window and lands much smaller than the per-cycle
            // total). Fallback to app_cpu_ns for the first frame of a
            // session where frame_total_ns is still 0 (matches the
            // identical fallback inside xrEndFrame).
            const int64_t perCycleCpuNs = (rec.frame_total_ns > 0)
                ? (rec.frame_total_ns - rec.wait_block_ns)
                : rec.app_cpu_ns;
            m_sumCpuNs        += perCycleCpuNs;
            m_sumGpuNs        += rec.gpu_time_ns;
            m_sumFrameTotalNs += rec.frame_total_ns;
            m_sumPeriodNs     += rec.period_ns;
            m_sumHeadroomPct      += rec.headroom_pct;
            m_sumGpuHeadroomPct   += rec.gpu_headroom_pct;
            m_lastFrameTotalNs = rec.frame_total_ns;
            ++m_count;

            // Push the frame_total_ns into the percentile ring. Drops
            // the oldest sample once the ring is full (~5 s at 144 Hz
            // = 720 samples). Only positive frametimes are accepted —
            // first-frame sentinels (frame_total=0 when there's no
            // prior tEnd) would otherwise rank as "infinitely fast"
            // (= INT64_MAX FPS after the inversion) and corrupt the
            // P99 estimate.
            if (rec.frame_total_ns > 0) {
                m_percentileRing[m_percentileHead] = rec.frame_total_ns;
                m_percentileHead = (m_percentileHead + 1) % kPercentileWindowSize;
                if (m_percentileCount < kPercentileWindowSize) {
                    ++m_percentileCount;
                }
            }

            const int64_t nowNs = qpcToNs(rec.timestamp_qpc, m_qpcFrequency);
            if (!m_armed) {
                // First frame seen: arm the refresh window. We deliberately
                // do NOT publish a snapshot on a single sample — averaging
                // over one frame defeats the purpose.
                //
                // We use a dedicated boolean rather than a sentinel value
                // on m_lastRefreshNs because qpcToNs(0, *) == 0 — the
                // tests legitimately pass timestamp_qpc=0 for the first
                // frame, and using "lastRefreshNs == 0" as the sentinel
                // would leave the aggregator perpetually re-arming
                // instead of publishing.
                m_lastRefreshNs = nowNs;
                m_armed = true;
                return;
            }
            if (nowNs - m_lastRefreshNs >= m_intervalNs && m_count > 0) {
                publishAndReset(nowNs);
            }
        }

        // The most-recently finalised snapshot. Stays at the previous tick's
        // values between refreshes. `valid` is false until the first refresh
        // has happened (≈ refreshIntervalNs after the first pushFrame, i.e.
        // ~9 frames into the session at 90 Hz / 10 Hz refresh).
        const OverlaySnapshot& snapshot() const noexcept { return m_snapshot; }

        // The refresh interval the renderer should poll at. Exposed mostly
        // for tests + future "render at the same cadence as we refresh".
        int64_t refreshIntervalNs() const noexcept { return m_intervalNs; }

      private:
        void publishAndReset(int64_t nowNs) noexcept {
            const float countF = static_cast<float>(m_count);

            // fps_instant uses the LATEST frame's frame_total (matches what
            // a player would call "current fps"); fps_avg uses the mean
            // across the window (the smoothed line a fpsvr-style HUD shows).
            m_snapshot.fps_instant =
                m_lastFrameTotalNs > 0 ? 1.0e9f / static_cast<float>(m_lastFrameTotalNs) : 0.0f;

            const float avgFrameTotal = static_cast<float>(m_sumFrameTotalNs) / countF;
            m_snapshot.fps_avg = avgFrameTotal > 0.0f ? 1.0e9f / avgFrameTotal : 0.0f;

            m_snapshot.cpu_frame_ms = (static_cast<float>(m_sumCpuNs) / countF) / 1.0e6f;
            m_snapshot.gpu_frame_ms = (static_cast<float>(m_sumGpuNs) / countF) / 1.0e6f;

            const float avgPeriod = static_cast<float>(m_sumPeriodNs) / countF;
            m_snapshot.target_fps = avgPeriod > 0.0f ? 1.0e9f / avgPeriod : 0.0f;

            // Percentiles — sort a copy of the (up to 720) ring entries
            // and pick at index ceil(p * (N-1)). std::nth_element would
            // be O(N) vs sort's O(N log N), but we need both 95% AND
            // 99% so we'd nth_element twice anyway, and 720 ints sort
            // in well under 100 µs even debug-build (verified via the
            // local timing harness). Done once per publish (~10 Hz),
            // so the cost amortises trivially.
            if (m_percentileCount > 0) {
                std::vector<int64_t> sorted(m_percentileRing.begin(),
                                             m_percentileRing.begin() + m_percentileCount);
                std::sort(sorted.begin(), sorted.end());
                auto frametimeAtPct = [&](float p) -> int64_t {
                    const std::size_t idx = static_cast<std::size_t>(
                        std::round(p * static_cast<double>(sorted.size() - 1)));
                    return sorted[std::min(idx, sorted.size() - 1)];
                };
                // P95-FPS = FPS such that 95% of frames are AT LEAST
                // this fast. sorted ASCENDING by frametime, so the
                // 95% quantile of frametimes gives the slowest
                // frametime among the fastest 95% — invert to FPS.
                const int64_t ft_p95 = frametimeAtPct(0.95f);
                const int64_t ft_p99 = frametimeAtPct(0.99f);
                m_snapshot.fps_p95 = ft_p95 > 0 ? 1.0e9f / static_cast<float>(ft_p95) : 0.0f;
                m_snapshot.fps_p99 = ft_p99 > 0 ? 1.0e9f / static_cast<float>(ft_p99) : 0.0f;
            } else {
                m_snapshot.fps_p95 = 0.0f;
                m_snapshot.fps_p99 = 0.0f;
            }

            // utilisation = 100 - headroom. Clamp to [0, 100] so a single
            // CPU-bound frame's negative headroom doesn't spike the
            // displayed util above 100 (visually that would suggest the
            // GPU is somehow doing >100% of its budget — confusing).
            const float headroomMean    = static_cast<float>(m_sumHeadroomPct)    / countF;
            const float gpuHeadroomMean = static_cast<float>(m_sumGpuHeadroomPct) / countF;
            m_snapshot.cpu_utilisation_pct = std::clamp(100.0f - headroomMean,    0.0f, 100.0f);
            m_snapshot.gpu_utilisation_pct = std::clamp(100.0f - gpuHeadroomMean, 0.0f, 100.0f);

            // Latch the latest GPU telemetry reading. We deliberately
            // do NOT average temperature across the window — drivers
            // update these counters only ~1 Hz, so any "averaging"
            // would just be averaging step-function holds. Show the
            // user the latest poll instead. If pushGpuTelemetry has
            // never been called (non-NVIDIA host with NvAPI absent),
            // m_latestGpuTempC stays NaN and vram_used stays 0 — the
            // renderer falls back to the "--" placeholder cleanly.
            m_snapshot.gpu_temp_c        = m_latestGpuTempC;
            m_snapshot.vram_used_bytes   = m_latestVramUsed;
            m_snapshot.vram_budget_bytes = m_latestVramBudget;

            m_snapshot.valid = true;
            // Bump the version monotonically — starting at 1 the very
            // first time so the renderer's default-zero cache always
            // sees a mismatch on the first valid snapshot. Wraps at
            // 2^64 ticks (≈ 5.8e9 years at 100 ms cadence — not a
            // worry).
            m_snapshot.version = ++m_publishCount;

            // Reset the window. lastRefreshNs ratchets to `nowNs` rather
            // than `lastRefreshNs + intervalNs` to avoid catch-up bursts
            // after a stall (a few hundred ms of GPU work would otherwise
            // queue several pending refreshes back-to-back).
            m_sumCpuNs = m_sumGpuNs = m_sumFrameTotalNs = m_sumPeriodNs = 0;
            m_sumHeadroomPct = m_sumGpuHeadroomPct = 0.0;
            m_count = 0;
            m_lastRefreshNs = nowNs;
        }

        int64_t m_intervalNs;
        int64_t m_qpcFrequency;
        int64_t m_lastRefreshNs = 0;
        bool    m_armed = false;
        int64_t m_sumCpuNs = 0;
        int64_t m_sumGpuNs = 0;
        int64_t m_sumFrameTotalNs = 0;
        int64_t m_sumPeriodNs = 0;
        double  m_sumHeadroomPct = 0;        // double because per-frame
        double  m_sumGpuHeadroomPct = 0;     // floats accumulate rounding error
        int64_t m_lastFrameTotalNs = 0;
        int     m_count = 0;
        // Monotonic publish counter — increments by 1 each time
        // publishAndReset runs. Stored into m_snapshot.version so
        // downstream caches can detect "this is the same data I
        // already formatted last frame" without comparing every
        // numeric field for equality.
        uint64_t m_publishCount = 0;
        // Latest GPU telemetry reading from pushGpuTelemetry. We
        // latch the most recent poll rather than averaging across the
        // refresh window — see the long comment in publishAndReset.
        // Defaults represent "no source available yet"; NaN
        // propagates through publishAndReset and the renderer's
        // isfinite() guard surfaces it as "--°C".
        float    m_latestGpuTempC = std::numeric_limits<float>::quiet_NaN();
        uint64_t m_latestVramUsed = 0;
        uint64_t m_latestVramBudget = 0;
        bool     m_gotGpuTelemetry = false;

        // Sliding window of frame_total_ns samples for P95/P99
        // computation. 720 samples = 5 s @ 144 Hz / 8 s @ 90 Hz —
        // long enough that a single stutter doesn't dominate, short
        // enough to track sustained performance changes (turn from
        // an open road into a town in DR2, the percentiles react
        // within ~3 s). Stored as a fixed-size std::array to keep
        // the aggregator allocation-free in the steady state.
        static constexpr std::size_t kPercentileWindowSize = 720;
        std::array<int64_t, kPercentileWindowSize> m_percentileRing{};
        std::size_t m_percentileHead = 0;
        std::size_t m_percentileCount = 0;

        OverlaySnapshot m_snapshot;
    };

} // namespace openxr_api_layer::detail
