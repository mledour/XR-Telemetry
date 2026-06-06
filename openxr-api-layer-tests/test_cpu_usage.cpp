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
// test_cpu_usage.cpp — tests on the CPUs ("busiest core") sampler.
//
// Two layers, mirroring test_gpu_telemetry.cpp:
//
//   * The POD sentinel contract (CpuUsageSample) is PORTABLE — the header
//     is <windows.h>-free — so it's exercised on every platform, including
//     the macOS / clang local build (see the test-suite-local-clang note).
//
//   * The CpuUsageReader behaviour needs NtQuerySystemInformation, so those
//     cases are guarded behind _WIN32. There we pin the DEFENSIVE contract:
//     a real Windows host always resolves the NT call, the baseline poll
//     primes a reading, and a second poll yields a sane [0,100] busiest-core
//     percentage. The exact number is load-dependent, so we assert the range
//     and finiteness, not a specific value — the in-headset HUD is the
//     acceptance signal for the actual reading.
// =============================================================================

#include <doctest/doctest.h>

#include <cmath>

#include "utils/cpu_usage.h"

using openxr_api_layer::detail::CpuUsageReader;
using openxr_api_layer::detail::CpuUsageSample;

TEST_CASE("CpuUsageSample: default-constructed sample signals 'no data'") {
    // This is what the aggregator / overlay see before the sampler has
    // produced a reading. NaN is the source-absent / no-baseline sentinel
    // that formatOverlayDisplayValues turns into "--".
    CpuUsageSample s;
    CHECK_FALSE(s.valid);
    CHECK(std::isnan(s.cpus_max_pct));
}

#if defined(_WIN32)

TEST_CASE("CpuUsageReader: init resolves the NT call and primes a baseline") {
    CpuUsageReader r;
    // Every Win32 process has ntdll mapped, so the resolve must succeed.
    CHECK(r.init());
    CHECK(r.isReady());
}

TEST_CASE("CpuUsageReader: post-baseline poll yields a sane busiest-core %") {
    CpuUsageReader r;
    REQUIRE(r.init());  // init() already took the baseline sample

    // First user-facing poll has a delta against the baseline. The exact
    // utilisation is load-dependent, but it must be a finite percentage in
    // [0, 100] (not NaN — that would mean "no reading").
    const CpuUsageSample s = r.poll();
    CHECK(s.valid);
    REQUIRE(std::isfinite(s.cpus_max_pct));
    CHECK(s.cpus_max_pct >= 0.0f);
    CHECK(s.cpus_max_pct <= 100.0f);
}

#endif  // _WIN32
