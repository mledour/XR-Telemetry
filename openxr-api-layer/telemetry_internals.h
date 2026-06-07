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
// telemetry_internals.h — pure helpers extracted from layer.cpp for testing.
//
// layer.cpp's anonymous namespace hides CsvWriter, GpuTimer, and the
// FrameRecord struct from the test binary, which is what we want for the
// production ABI but blocks unit-testing of the math + bookkeeping bits
// that don't actually need any OpenXR / D3D11 state to verify.
//
// This header exposes the parts that ARE pure (no side effects, no
// platform calls) under `openxr_api_layer::detail` so a test_*.cpp can
// `#include "telemetry_internals.h"` and assert on them directly.
//
// Stays self-contained — does NOT include pch.h, so the test binary can
// pick it up without pulling the full Windows / D3D / OpenXR header
// stack. Only <cstdint> and <deque> from the standard library.
// =============================================================================

#include <cassert>
#include <cstdint>
#include <deque>

namespace openxr_api_layer::detail {

    // XrBaseInStructure-style chain walker. Walks `head` looking for the
    // first entry whose `type` field equals `targetType`, returns nullptr
    // if none. Used by layer.cpp to locate XrGraphicsBindingD3D{11,12}KHR
    // (and any future binding type) inside the `createInfo->next` chain
    // passed to xrCreateSession.
    //
    // Exposed here — instead of staying private to layer.cpp — so the
    // test binary can verify the walk on a synthetic chain without
    // dragging in <openxr/openxr.h> or the D3D headers. The BaseLike
    // struct mirrors the XrBaseInStructure layout (an XrStructureType
    // enum is int32_t, followed by a pointer-to-self), so casting any
    // valid OpenXR `next` chain to BaseLike* is well-defined under the
    // common-initial-sequence rule used throughout the OpenXR API.
    struct BaseLike {
        int32_t type;
        const BaseLike* next;
    };
    inline const void* findInTypedChain(const void* head, int32_t targetType) noexcept {
        const auto* base = reinterpret_cast<const BaseLike*>(head);
        while (base) {
            if (base->type == targetType) return base;
            base = base->next;
        }
        return nullptr;
    }

    // One row of CSV. Pushed from the frame thread (xrEndFrame), drained by
    // a background writer thread.  POD-like: copyable, no resources owned.
    //
    // This used to live in layer.cpp's anonymous namespace; moved here so
    // tests on patchAndDrainPending() / flushPendingFramesUnresolved() can
    // construct one without faking an OpenXR session.
    struct FrameRecord {
        uint64_t frame_index;
        int64_t timestamp_qpc;     // raw QPC tick at xrEndFrame entry
        int64_t wait_block_ns;     // tWaitOut - tWaitIn   (compositor throttle)
        int64_t pre_begin_ns;      // tBegin - tWaitOut    (wait→begin housekeeping)
        int64_t app_cpu_ns;        // tEnd - tWaitOut      (wait→end window)
        int64_t end_frame_ns;      // OpenXrApi::xrEndFrame duration
        int64_t frame_total_ns;    // tEnd - tEndPrev      (full cycle)
        int64_t gpu_time_ns;       // D3D11 timestamp delta (begin→end on GPU)
        int64_t period_ns;
        float headroom_pct;        // CPU headroom
        float gpu_headroom_pct;    // GPU headroom
        bool should_render;
        // GPU telemetry — see gpu_telemetry.h for the per-source
        // semantics, including the 0-byte sentinel safety argument
        // for VRAM. Stored per-frame even though the underlying
        // sensors update ~1 Hz: the layer caches the latest poll and
        // every frame in the same window logs the same value, giving
        // the offline analyser a single column to filter on (no
        // "fill-forward" gymnastics).
        float gpu_temp_c;          // NaN ⇒ source unavailable
        uint64_t vram_used_bytes;  // 0   ⇒ source unavailable (no real GPU reports 0)
        uint64_t vram_budget_bytes;
        // CPU usage — busiest logical-processor utilisation %, the overlay's
        // "CPUs LOAD". Cached/latched like the GPU sensors above (the sampler
        // polls ~1 Hz; every frame in the window logs the same value). NaN ⇒
        // source unavailable. Appended LAST so the CSV column order stays
        // backward-compatible (see the CsvWriter header comment).
        float cpus_max_pct;        // NaN ⇒ source unavailable
    };

    // --- Time conversions ---------------------------------------------------

    // QueryPerformanceCounter delta ticks → nanoseconds. Splits into whole
    // and fractional parts so the (delta * 1e9) intermediate never overflows
    // int64 for any realistic interval. Concretely: the whole-part term
    // `whole * 1e9` stays within int64 as long as `whole < INT64_MAX / 1e9
    // ≈ 9.22e9`. With a 10 MHz QPC clock that bounds the input at
    // `ticks < 9.22e9 * 1e7 = 9.22e16 ticks ≈ 9.22e9 seconds ≈ 292 years`.
    // Process uptimes are not a concern.
    //
    // Returns 0 if freq <= 0 (defensive; never happens with a valid call to
    // QueryPerformanceFrequency).
    inline int64_t qpcToNs(int64_t ticks, int64_t freq) noexcept {
        if (freq <= 0) return 0;
        const int64_t whole = ticks / freq;
        const int64_t rem = ticks % freq;
        return whole * 1'000'000'000LL + (rem * 1'000'000'000LL) / freq;
    }

    // D3D11 GPU timestamp delta ticks → nanoseconds, with the same split-
    // and-recombine strategy as qpcToNs. Inputs come from
    // D3D11_QUERY_DATA_TIMESTAMP_DISJOINT::Frequency (UINT64) and the
    // delta of two D3D11_QUERY_TIMESTAMP results.
    //
    // Returns 0 if frequency == 0 (defensive; happens when the disjoint
    // query reports Disjoint == true).
    //
    // The final static_cast<int64_t> of a uint64_t value is well-defined
    // as long as the value fits in int64_t, i.e. `whole * 1e9 < 2^63`,
    // i.e. `whole < ~9.22e9`. With a 1 GHz GPU timestamp clock that bounds
    // `deltaTicks < 9.22e18 ticks ≈ 9.22e9 seconds ≈ 292 years` — well
    // beyond any frame interval. The assert catches the corner case where
    // a buggy driver reports a huge `deltaTicks` after marking the
    // disjoint range as non-disjoint (it should mark it disjoint instead,
    // in which case we never get here).
    inline int64_t gpuTimestampDeltaToNs(uint64_t deltaTicks,
                                         uint64_t frequency) noexcept {
        if (frequency == 0) return 0;
        const uint64_t whole = deltaTicks / frequency;
        const uint64_t rem = deltaTicks % frequency;
        constexpr uint64_t kMaxWhole =
            static_cast<uint64_t>(INT64_MAX) / 1'000'000'000ULL;
        assert(whole <= kMaxWhole &&
               "gpuTimestampDeltaToNs: deltaTicks span >292 years — "
               "suspect driver-reported timestamp / disjoint Frequency");
        return static_cast<int64_t>(
            whole * 1'000'000'000ULL +
            (rem * 1'000'000'000ULL) / frequency);
    }

    // Pair-of-timestamps wrapper around gpuTimestampDeltaToNs with two
    // defensive checks folded in:
    //
    //   1. freq == 0  — disjoint sentinel (D3D11) or uninitialized
    //                   GetTimestampFrequency() (D3D12). Returns 0 ns.
    //   2. tEnd <= tStart — driver bug. Seen on a handful of WMR drivers
    //                       where the resolved end-of-frame timestamp comes
    //                       back below the start. Treating the delta as
    //                       unsigned would yield a huge bogus number and
    //                       poison the headroom math downstream, so we
    //                       swallow it as 0 ns.
    //
    // Both D3D11GpuTimer::endFrameAndResolveOldest and D3D12GpuTimer::end
    // FrameAndResolveOldest funnel through here so the contract — and its
    // unit tests — stay in one place. The D3D11 path additionally checks
    // !disjointData.Disjoint at the call site (that flag lives on the
    // disjoint query, not on the pair of timestamps, so it doesn't belong
    // in this helper).
    inline int64_t gpuTimestampPairToNs(uint64_t tStart,
                                        uint64_t tEnd,
                                        uint64_t frequency) noexcept {
        if (frequency == 0 || tEnd <= tStart) return 0;
        return gpuTimestampDeltaToNs(tEnd - tStart, frequency);
    }

    // --- Headroom formulas --------------------------------------------------

    // CPU headroom % = (1 - appPerCycleNs / periodNs) * 100.
    //
    // Returns 100.0f when periodNs <= 0 — the "no measurement available"
    // sentinel for the rare transient case where the runtime hadn't
    // committed predictedDisplayPeriod yet on the very first frame. Same
    // convention as gpu_headroom_pct below so analyses can filter both
    // with `period_ns > 0`.
    //
    // Negative result ⇒ app exceeded the period budget this cycle (CPU-
    // bound this frame); positive ⇒ headroom available; 100 ⇒ no work.
    inline float computeCpuHeadroomPct(int64_t appPerCycleNs,
                                       int64_t periodNs) noexcept {
        if (periodNs <= 0) return 100.0f;
        const double ratio =
            static_cast<double>(appPerCycleNs) / static_cast<double>(periodNs);
        return static_cast<float>((1.0 - ratio) * 100.0);
    }

    // GPU headroom % = (1 - gpuTimeNs / periodNs) * 100.
    //
    // Same sentinel rule as computeCpuHeadroomPct. Reads 100% when
    // gpuTimeNs == 0 (no D3D11 binding, disjoint range invalid, or session-
    // end flush with no GPU result) — the natural value of the formula
    // when the numerator is 0, matching fpsVR / OpenXR Toolkit overlays.
    inline float computeGpuHeadroomPct(int64_t gpuTimeNs,
                                       int64_t periodNs) noexcept {
        if (periodNs <= 0) return 100.0f;
        const double ratio =
            static_cast<double>(gpuTimeNs) / static_cast<double>(periodNs);
        return static_cast<float>((1.0 - ratio) * 100.0);
    }

    // --- Pending-frame deque bookkeeping ------------------------------------

    // Drain pending FrameRecords up to (and including) the one matching
    // `resolvedFrameIndex`, patching its gpu_time_ns and gpu_headroom_pct
    // with the resolved values. Stale entries (frame_index <
    // resolvedFrameIndex — possible when the ring overran and lost results)
    // are pushed as-is, with gpu_time_ns staying at its placeholder 0.
    // Future entries (frame_index > resolvedFrameIndex) stop the drain so
    // they remain pending for their own resolution.
    //
    // `sink` is any callable accepting `const FrameRecord&`. In production
    // it's a thin lambda that forwards to CsvWriter::push; in tests it's a
    // capture-by-reference into a std::vector for inspection.
    //
    // PRECONDITION: `sink` must NOT mutate `pending` (e.g. by reentrantly
    // calling patchAndDrainPending or flushPendingFramesUnresolved). The
    // loop holds a reference into `pending.front()` across the sink call
    // and pop_front; mutating the deque from inside the sink would
    // invalidate that reference. C++17 has no `std::regular_invocable`
    // concept to enforce this at compile time — the constraint is on the
    // caller's discipline.
    template <typename Sink>
    void patchAndDrainPending(std::deque<FrameRecord>& pending,
                              uint64_t resolvedFrameIndex,
                              int64_t gpuTimeNs,
                              Sink&& sink) {
        while (!pending.empty()) {
            auto& front = pending.front();
            if (front.frame_index > resolvedFrameIndex) {
                break;  // future entry — stop the drain
            }
            if (front.frame_index == resolvedFrameIndex) {
                front.gpu_time_ns = gpuTimeNs;
                front.gpu_headroom_pct =
                    computeGpuHeadroomPct(gpuTimeNs, front.period_ns);
            }
            // Capture the match flag BEFORE pop_front invalidates `front`.
            const bool wasMatch = (front.frame_index == resolvedFrameIndex);
            sink(front);
            pending.pop_front();
            if (wasMatch) break;
        }
    }

    // Drain every pending FrameRecord through `sink` without patching, used
    // at xrDestroySession / ~OpenXrLayer when no more GPU results will land.
    // Records keep their placeholder gpu_time_ns (0) and gpu_headroom_pct
    // (100.0f). Analyses should filter `gpu_time_ns > 0` if they want to
    // exclude these last-of-session rows from GPU statistics.
    //
    // Same `sink` precondition as patchAndDrainPending: don't mutate
    // `pending` from inside the callback.
    template <typename Sink>
    void flushPendingFramesUnresolved(std::deque<FrameRecord>& pending,
                                      Sink&& sink) {
        while (!pending.empty()) {
            sink(pending.front());
            pending.pop_front();
        }
    }

} // namespace openxr_api_layer::detail
