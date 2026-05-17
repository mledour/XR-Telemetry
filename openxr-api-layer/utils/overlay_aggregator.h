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
#include <cstdint>

namespace openxr_api_layer::detail {

    // What the future renderer (PR2) will draw into the head-locked quad.
    // All fields are floats so the rendering code can pass them straight to
    // DirectWrite / a string formatter without further conversion.
    struct OverlaySnapshot {
        float fps_instant = 0;          // 1e9 / last frame_total_ns
        float fps_avg = 0;              // 1e9 / mean(frame_total_ns over the refresh window)
        float cpu_frame_ms = 0;         // mean(app_cpu_ns) / 1e6
        float gpu_frame_ms = 0;         // mean(gpu_time_ns) / 1e6
        float cpu_utilisation_pct = 0;  // 100 - mean(headroom_pct), clamped [0, 100]
        float gpu_utilisation_pct = 0;  // 100 - mean(gpu_headroom_pct), clamped [0, 100]
        float target_fps = 0;           // 1e9 / mean(period_ns), the runtime's predicted display rate
        bool  valid = false;            // false until the first refresh tick has finalised
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
        //                    refresh deadline immediately.
        explicit OverlayAggregator(int64_t refreshIntervalNs = 100'000'000LL,
                                   int64_t qpcFrequency = 1'000'000'000LL) noexcept
            : m_intervalNs(refreshIntervalNs > 0 ? refreshIntervalNs : 1),
              // Clamp anything < 1 kHz to the 1 GHz default. Real Windows
              // QPC is always at least 1 MHz, usually 10 MHz; a caller
              // somehow passing 1 / 2 / 100 is either a test bug or a
              // broken HAL clock falling through OpenXrLayer's own
              // fallback. Either way we substitute a value that won't
              // make every ns conversion explode.
              m_qpcFrequency(qpcFrequency >= 1000 ? qpcFrequency : 1'000'000'000LL) {}

        // Push one fully-resolved FrameRecord (post-GPU-patch — gpu_time_ns
        // is its final value, not the in-flight 0). The aggregator
        // accumulates internally; the snapshot is refreshed when the QPC
        // delta since the previous refresh exceeds refreshIntervalNs.
        void pushFrame(const FrameRecord& rec) noexcept {
            // Accumulate every per-frame field we'll need for the snapshot.
            m_sumCpuNs        += rec.app_cpu_ns;
            m_sumGpuNs        += rec.gpu_time_ns;
            m_sumFrameTotalNs += rec.frame_total_ns;
            m_sumPeriodNs     += rec.period_ns;
            m_sumHeadroomPct      += rec.headroom_pct;
            m_sumGpuHeadroomPct   += rec.gpu_headroom_pct;
            m_lastFrameTotalNs = rec.frame_total_ns;
            ++m_count;

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

            // utilisation = 100 - headroom. Clamp to [0, 100] so a single
            // CPU-bound frame's negative headroom doesn't spike the
            // displayed util above 100 (visually that would suggest the
            // GPU is somehow doing >100% of its budget — confusing).
            const float headroomMean    = static_cast<float>(m_sumHeadroomPct)    / countF;
            const float gpuHeadroomMean = static_cast<float>(m_sumGpuHeadroomPct) / countF;
            m_snapshot.cpu_utilisation_pct = std::clamp(100.0f - headroomMean,    0.0f, 100.0f);
            m_snapshot.gpu_utilisation_pct = std::clamp(100.0f - gpuHeadroomMean, 0.0f, 100.0f);

            m_snapshot.valid = true;

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
        OverlaySnapshot m_snapshot;
    };

} // namespace openxr_api_layer::detail
