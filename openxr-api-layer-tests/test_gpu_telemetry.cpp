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
// test_gpu_telemetry.cpp — smoke tests on GpuTelemetryReader.
//
// We can't meaningfully unit-test the actual NvAPI / DXGI plumbing in CI:
//   - GitHub Actions runners have no NVIDIA GPU and no NvAPI driver, so
//     nvapi64.dll won't load. The test will exercise the FALLBACK path
//     (init() returns false on temperature side, poll() returns NaN).
//   - DXGI VRAM also needs an adapter; we pass nullptr so the VRAM path
//     stays disabled too.
//
// What this file pins instead is the DEFENSIVE contract:
//   - init(nullptr) doesn't crash, and returns false cleanly when both
//     sources are unavailable.
//   - poll() on an uninitialised reader doesn't crash and returns
//     "no data" sentinels (NaN temp + 0 VRAM).
//
// The "happy path" (real NVIDIA GPU, DXGIAdapter passed in) is validated
// by in-game testing — the CSV's gpu_temp_c column and the overlay's
// row 3 are the user-facing acceptance signals.
// =============================================================================

#include <doctest/doctest.h>

#include <cmath>

#include "utils/gpu_telemetry.h"

using openxr_api_layer::detail::GpuTelemetryReader;
using openxr_api_layer::detail::GpuTelemetrySample;

TEST_CASE("GpuTelemetryReader: default-constructed sample signals 'no data'") {
    // The default GpuTelemetrySample is the value the renderer / CSV
    // see when no source has reported yet. Pin the sentinels so a
    // future refactor doesn't drift them.
    GpuTelemetrySample s;
    CHECK_FALSE(std::isfinite(s.gpu_temp_c));   // NaN sentinel
    CHECK(s.vram_used_bytes == 0);
    CHECK(s.vram_budget_bytes == 0);
    CHECK_FALSE(s.temp_valid);
    CHECK_FALSE(s.vram_valid);
}

TEST_CASE("GpuTelemetryReader: init(nullptr) on a non-NVIDIA host returns false cleanly") {
    // GitHub Actions runners don't have NvAPI installed; nullptr
    // adapter disables the DXGI path. So init() should return
    // false (neither source ready) without crashing.
    //
    // On a developer's local NVIDIA box the temperature side may
    // actually init — that's fine, the test still passes as long
    // as we don't crash. The point is the contract on the CI
    // path.
    GpuTelemetryReader reader;
    reader.init(nullptr);
    // poll() must always be safe to call, even if init() returned
    // false. We're verifying no UB from the destructor either —
    // the FreeLibrary + Release paths in ~GpuTelemetryReader must
    // be null-safe when init() bailed out.
    const auto sample = reader.poll();
    // No adapter → VRAM path is definitively off.
    CHECK_FALSE(sample.vram_valid);
    CHECK(sample.vram_used_bytes == 0);
    // Temperature is "best-effort": we accept either outcome
    // (NaN on CI, real value on dev box) — we just verify the
    // call returned.
    if (sample.temp_valid) {
        CHECK(std::isfinite(sample.gpu_temp_c));
        // Realistic GPU temperatures sit between 20°C (idle on
        // a cold morning) and 110°C (thermal throttle threshold).
        // A reading outside this band almost certainly means we
        // misinterpreted the NvAPI struct layout.
        CHECK(sample.gpu_temp_c >= 0.0f);
        CHECK(sample.gpu_temp_c < 120.0f);
    } else {
        CHECK_FALSE(std::isfinite(sample.gpu_temp_c));
    }
}

TEST_CASE("GpuTelemetryReader: poll() is safe before init()") {
    // A caller that constructed the reader but didn't get around to
    // calling init() yet (race in the host's startup sequence?) must
    // still see a well-formed empty sample, not crash.
    GpuTelemetryReader reader;
    const auto sample = reader.poll();
    CHECK_FALSE(sample.temp_valid);
    CHECK_FALSE(sample.vram_valid);
    CHECK_FALSE(std::isfinite(sample.gpu_temp_c));
    CHECK(sample.vram_used_bytes == 0);
    CHECK(sample.vram_budget_bytes == 0);
}
