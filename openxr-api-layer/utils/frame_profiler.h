// MIT License
//
// Copyright(c) 2025 Michael Ledour
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

// =============================================================================
// frame_profiler.h — TEMPORARY per-section CPU profiler.
//
// Diagnostic only: breaks the layer's per-frame work into named sections,
// accumulates QueryPerformanceCounter deltas, and logs the mean/max per
// section every `logEvery` frames as a "[PROF <tag>]" line in the layer log.
// Used to pin down which call dominates the D3D12 overlay CPU cost (the CPU≈
// GPU coupling we couldn't explain from aggregate timings).
//
// REVERT before merge — delete this file and its call sites (grep "PROF").
// =============================================================================

#include <cstdint>
#include <string>

#include <Windows.h>

#include "framework/log.h"   // ::openxr_api_layer::log::Log + fmt (via pch.h)

namespace openxr_api_layer::utils::diag {

    inline int64_t profQpc() {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return c.QuadPart;
    }

    inline double profUsPerTick() {
        static const double f = [] {
            LARGE_INTEGER x;
            QueryPerformanceFrequency(&x);
            return 1.0e6 / static_cast<double>(x.QuadPart);
        }();
        return f;
    }

    // N named sections. add(i, ticks) accumulates a QPC delta; tick()
    // advances the frame counter and, every `logEvery` frames, logs the
    // per-section mean/max in microseconds and resets. Intended as a
    // function-static — single-threaded (xrEndFrame thread) so no locking.
    template <int N>
    struct SectionProfiler {
        const char* tag;
        const char* names[N];
        int         logEvery;
        int64_t     sum[N]{};
        int64_t     mx[N]{};
        int         frames = 0;

        void add(int i, int64_t ticks) {
            if (ticks < 0) ticks = 0;
            sum[i] += ticks;
            if (ticks > mx[i]) mx[i] = ticks;
        }

        void tick() {
            if (++frames < logEvery) return;
            const double u = profUsPerTick();
            std::string line =
                fmt::format("xr_telemetry: [PROF {}] {} frames (mean/max us):",
                            tag, frames);
            for (int i = 0; i < N; ++i) {
                line += fmt::format("  {}={:.1f}/{:.0f}",
                                    names[i],
                                    (static_cast<double>(sum[i]) * u) / frames,
                                    static_cast<double>(mx[i]) * u);
            }
            ::openxr_api_layer::log::Log(line + "\n");
            for (int i = 0; i < N; ++i) { sum[i] = 0; mx[i] = 0; }
            frames = 0;
        }
    };

}   // namespace openxr_api_layer::utils::diag
