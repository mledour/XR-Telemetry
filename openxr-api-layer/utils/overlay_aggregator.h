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
//   - The renderer calls snapshot() in its own refresh tick and gets
//     whatever the most recent finalised aggregate was. No coupling
//     between renderer cadence and aggregation cadence.
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

    // What the renderer draws into the head-locked quad. All fields are
    // floats so the rendering code can pass them straight to a string
    // formatter without further conversion.
    struct OverlaySnapshot {
        float fps_instant = 0;          // 1e9 / last frame_total_ns
        float fps_avg = 0;              // 1e9 / mean(frame_total_ns over the refresh window)
        // CPU time the app spends PER CYCLE (frame_total - wait_block).
        // Matches the metric used by `cpu_utilisation_pct` and by the
        // CSV's `headroom_pct` column, so the displayed time and util%
        // are coherent. Falls back to app_cpu_ns on the first frame
        // (no previous cycle yet), same as xrEndFrame does for
        // headroom computation. Surfaced in the HUD as "Render ms" —
        // it's the full per-cycle CPU cost (app work + endframe call +
        // pre-begin housekeeping).
        float cpu_frame_ms = 0;
        // App-only CPU time — the wait→end window (rec.app_cpu_ns).
        // Surfaced as "App ms" alongside Render ms in the CPU
        // frametime panel. The difference (Render − App) tells the
        // user how much of the per-cycle CPU is OpenXR overhead vs
        // their own game logic / submission code, which is the
        // diagnostic value the design asks for.
        float cpu_app_ms = 0;
        float gpu_frame_ms = 0;         // mean(gpu_time_ns) / 1e6
        float cpu_utilisation_pct = 0;  // 100 - mean(headroom_pct), clamped [0, 100]
        float gpu_utilisation_pct = 0;  // 100 - mean(gpu_headroom_pct), clamped [0, 100]
        // System-wide utilisation of the BUSIEST logical processor over the
        // last poll interval — the fpsVR "CPUs" reading. Distinct from
        // cpu_utilisation_pct (which is frame-timing-derived, per-cycle CPU
        // vs budget): a high cpus_max_pct with a tame cpu_utilisation_pct is
        // the single-thread-bound signature. Latched from CpuUsageReader via
        // pushCpuTelemetry (NOT averaged over the window — it's already an
        // interval delta). NaN ⇒ source unavailable / no baseline yet; the
        // format helper renders that as "--".
        float cpus_max_pct = std::numeric_limits<float>::quiet_NaN();
        float target_fps = 0;           // 1e9 / mean(period_ns), the runtime's predicted display rate
        // FPS percentiles over a sliding window (kPercentileWindowSize
        // samples — 30 s @ 144 Hz). Computed by sorting the recent
        // frame_total_ns ring and indexing at the 95th / 99th /
        // 99.9th percentile of the ASCENDING-sorted frametimes —
        // the FPS value reported is `1e9 / frametime_at_that_index`,
        // so smaller percentile-FPS means "worse N% of frames".
        //
        //   fps_p95   = 95 % of frames hit at least this FPS
        //               (the slow 5 % drop below)
        //   fps_p99   = 99 % of frames hit at least this FPS
        //               (the worst 1 % drop below)
        //   fps_p99_9 = 99.9 % of frames hit at least this FPS
        //               (the single worst ~0.1 % — typically a few
        //               specific spike frames in a 30 s window)
        //
        // 30 s window vs the previous 5 s was specifically chosen so
        // P99.9 has statistical meaning: at 144 Hz, 0.1 % of 4320 ≈
        // 4 frames, enough samples to be stable against a single
        // random background-process hiccup. At 90 Hz the window
        // covers ~48 s = ~4 frames in P99.9 too.
        //
        // Same convention as fpsVR / SteamVR "Frame Timing".
        // 0 until the percentile window has at least one sample.
        float fps_p95   = 0;
        float fps_p99   = 0;
        float fps_p99_9 = 0;
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
        // percentileIntervalNs: minimum spacing between P95/P99/P99.9
        //                    re-sorts. 0 (the default) means "recompute on
        //                    every publish" — the historical behaviour, kept
        //                    as the default so the unit tests stay
        //                    deterministic without threading a clock through
        //                    them. Production passes ~1 s (see OpenXrLayer):
        //                    the percentile window is 30-48 s, so re-sorting
        //                    10×/s only churned CPU. The sort was the
        //                    dominant cost of publishAndReset (~150 µs for a
        //                    full 4320-entry ring, measured via the
        //                    xr_telemetry_publish ETW span); throttling it to
        //                    ~1 Hz removes it from 9 of every 10 publishes.
        explicit OverlayAggregator(int64_t refreshIntervalNs = 100'000'000LL,
                                   int64_t qpcFrequency = 1'000'000'000LL,
                                   int64_t percentileIntervalNs = 0) noexcept
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
              m_qpcFrequency(qpcFrequency >= 1000 ? qpcFrequency : 10'000'000LL),
              // Stored as-given; the publish path treats any value <= 0 as
              // "recompute every publish", so no normalisation is needed here.
              m_percentileIntervalNs(percentileIntervalNs) {}

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

        // Push the latest "CPUs" reading (busiest-core utilisation %) from
        // CpuUsageReader::poll(). Same latch-not-average rationale as
        // pushGpuTelemetry: the value is already an interval delta, so the
        // snapshot carries the most recent poll verbatim. NaN is accepted
        // (and re-published as NaN); the format helper substitutes "--".
        // Decoupled from pushFrame() because the CPU sampler polls at the
        // aggregator cadence (~1 Hz), not per frame.
        void pushCpuTelemetry(float cpus_max_pct) noexcept {
            m_latestCpusMaxPct = cpus_max_pct;
            m_gotCpuTelemetry  = true;
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
            m_sumAppCpuNs     += rec.app_cpu_ns;
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

            m_snapshot.cpu_frame_ms = (static_cast<float>(m_sumCpuNs)    / countF) / 1.0e6f;
            m_snapshot.cpu_app_ms   = (static_cast<float>(m_sumAppCpuNs) / countF) / 1.0e6f;
            m_snapshot.gpu_frame_ms = (static_cast<float>(m_sumGpuNs)    / countF) / 1.0e6f;

            const float avgPeriod = static_cast<float>(m_sumPeriodNs) / countF;
            m_snapshot.target_fps = avgPeriod > 0.0f ? 1.0e9f / avgPeriod : 0.0f;

            // Percentiles (P95 / P99 / P99.9). Two cost controls here, both
            // motivated by the xr_telemetry_publish ETW span showing the old
            // std::sort of the full 4320-entry ring at ~150 µs — the single
            // dominant term in publishAndReset's frame-thread cost:
            //
            //   1) std::nth_element instead of std::sort. We need only three
            //      order statistics, not a total order. Selection is O(n) per
            //      pick vs sort's O(n log n); nesting the three picks from the
            //      highest index down keeps each successive selection inside
            //      the previous left partition, so the total stays well under
            //      one full sort.
            //   2) Recompute at most every m_percentileIntervalNs. The
            //      percentiles describe a 30-48 s window, so refreshing them
            //      faster than ~1 Hz adds churn with no perceptible signal.
            //      Between recomputes the snapshot carries the previous values
            //      (same latch idea as the GPU telemetry below). With the
            //      default interval of 0 this gates to "every publish", which
            //      is what the unit tests assume.
            //
            // m_percentilesPrimed forces the very first computation
            // immediately so the HUD isn't blank for up to one interval at
            // session start.
            const bool refreshPercentiles =
                m_percentileCount > 0 &&
                (m_percentileIntervalNs <= 0 || !m_percentilesPrimed ||
                 nowNs - m_lastPercentileNs >= m_percentileIntervalNs);
            if (refreshPercentiles) {
                // Copy the live ring into the pre-allocated scratch (no
                // per-publish heap allocation). Done only when we actually
                // recompute, so the ~34 KB copy is also skipped on the
                // throttled publishes.
                std::copy(m_percentileRing.begin(),
                          m_percentileRing.begin() + m_percentileCount,
                          m_percentileSortScratch.begin());
                const auto begin = m_percentileSortScratch.begin();
                const std::size_t n = m_percentileCount;

                // INTEGER permille (per-thousand) indexing, NOT floor(p*N)
                // with a float p — `0.95 * 100` is 94.99999… in IEEE-754 and
                // floor() yields 94, dropping P95 into the fast tail, the
                // OPPOSITE of intent. Frametimes are ascending, so the slow
                // tail sits at the high indices and "P95 FPS" is the FPS at
                // the frametime 95 % of frames stay at-or-below. The valid
                // range is [begin, begin+n); the rest of the 4320-entry
                // scratch is stale, so every index is bounded by n.
                auto idxOf = [n](int permille) -> std::size_t {
                    std::size_t idx = (static_cast<std::size_t>(permille) * n) / 1000;
                    return idx >= n ? n - 1 : idx;
                };
                const std::size_t i95  = idxOf(950);
                const std::size_t i99  = idxOf(990);
                const std::size_t i999 = idxOf(999);

                // i95 <= i99 <= i999 always. After nth_element positions
                // i999 over [begin, begin+n), the i999 smallest elements
                // occupy [begin, begin+i999) (in arbitrary order) and the
                // global i99-th statistic is among them — so confine the next
                // selection to that subrange, and i95 within [begin,
                // begin+i99). The guards keep `nth` strictly < `last` when
                // indices coincide at small n (nth must be dereferenceable);
                // a coinciding index is already correctly placed by the prior
                // wider selection.
                std::nth_element(begin, begin + i999, begin + n);
                if (i99 < i999) std::nth_element(begin, begin + i99, begin + i999);
                if (i95 < i99)  std::nth_element(begin, begin + i95, begin + i99);

                const int64_t ft_p95   = m_percentileSortScratch[i95];
                const int64_t ft_p99   = m_percentileSortScratch[i99];
                const int64_t ft_p99_9 = m_percentileSortScratch[i999];
                m_snapshot.fps_p95   = ft_p95   > 0 ? 1.0e9f / static_cast<float>(ft_p95)   : 0.0f;
                m_snapshot.fps_p99   = ft_p99   > 0 ? 1.0e9f / static_cast<float>(ft_p99)   : 0.0f;
                m_snapshot.fps_p99_9 = ft_p99_9 > 0 ? 1.0e9f / static_cast<float>(ft_p99_9) : 0.0f;

                m_lastPercentileNs  = nowNs;
                m_percentilesPrimed = true;
            }
            // else: leave m_snapshot.fps_p95 / p99 / p99_9 at the last
            // computed values (0 until the first prime).

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

            // Latch the latest "CPUs" reading too — same reasoning as the
            // GPU telemetry above (it's an interval delta, not something to
            // average across the refresh window). Stays NaN until the CPU
            // sampler reports, so the renderer falls back to "--" cleanly.
            m_snapshot.cpus_max_pct      = m_latestCpusMaxPct;

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
            m_sumCpuNs = m_sumAppCpuNs = m_sumGpuNs = m_sumFrameTotalNs = m_sumPeriodNs = 0;
            m_sumHeadroomPct = m_sumGpuHeadroomPct = 0.0;
            m_count = 0;
            m_lastRefreshNs = nowNs;
        }

        int64_t m_intervalNs;
        int64_t m_qpcFrequency;
        int64_t m_lastRefreshNs = 0;
        bool    m_armed = false;
        // Percentile re-sort throttle (see the constructor doc). The interval
        // is set from the ctor arg; m_lastPercentileNs is the QPC-ns time of
        // the last P95/P99/P99.9 recompute, and m_percentilesPrimed makes the
        // first publish with samples compute immediately.
        int64_t m_percentileIntervalNs = 0;
        int64_t m_lastPercentileNs = 0;
        bool    m_percentilesPrimed = false;
        int64_t m_sumCpuNs = 0;     // per-cycle (frame_total - wait_block) — Render ms
        int64_t m_sumAppCpuNs = 0;  // wait→end window (rec.app_cpu_ns) — App ms
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
        // Latest "CPUs" (busiest-core %) reading from pushCpuTelemetry.
        // Latched, not averaged — see pushCpuTelemetry. NaN until the CPU
        // sampler first reports (or forever, on the unlikely host where
        // NtQuerySystemInformation didn't resolve).
        float    m_latestCpusMaxPct = std::numeric_limits<float>::quiet_NaN();
        bool     m_gotCpuTelemetry = false;

        // Sliding window of frame_total_ns samples for P95 / P99 /
        // P99.9 computation. 4320 samples = 30 s @ 144 Hz / 48 s @
        // 90 Hz. Width chosen so P99.9 has statistical meaning
        // (0.1 % × 4320 ≈ 4 samples — robust against a single
        // background hiccup). The percentiles still respond to
        // sustained changes within ~3 s because P95 ignores the
        // newest spike until it accumulates a fair share of the
        // window. Storage: 4320 × 8 B ≈ 34 KB per aggregator —
        // trivial vs the OpenXR-side per-frame surface.
        //
        // Sort cost at publish: 4320 ints in well under 100 µs on
        // a modern CPU (we sort ~10×/s, totalling ~1 ms/s of CPU —
        // imperceptible against the frame thread's 11 ms budget).
        static constexpr std::size_t kPercentileWindowSize = 4320;
        std::array<int64_t, kPercentileWindowSize> m_percentileRing{};
        // Scratch buffer for the per-publish sort. Reused across
        // publishes so we don't churn 34 KB / 10 Hz = 340 KB/s through
        // the host's allocator. Layers run inside third-party games and
        // the project policy is "zero alloc in the hot path where
        // possible" — see CLAUDE.md's notes on anti-cheat / I/O
        // surprise risk. Same fixed-size array shape as
        // m_percentileRing so we can copy + sort in place.
        std::array<int64_t, kPercentileWindowSize> m_percentileSortScratch{};
        std::size_t m_percentileHead = 0;
        std::size_t m_percentileCount = 0;

        OverlaySnapshot m_snapshot;
    };

} // namespace openxr_api_layer::detail
