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
// test_cpu_telemetry.cpp — exhaustive tests on the PURE seqlock/staleness
// decision (evaluateCpuTelemetryRead) plus the CpuTelemetrySample sentinel
// contract. These run on every platform (no Windows / no real mapping).
//
// The CpuTelemetryReader's OS plumbing (OpenFileMapping / MapViewOfFile) is
// the same kind of thin shell as GpuTelemetryReader's NvAPI/DXGI side and is
// validated the same way: a defensive contract test compiled only on Windows
// (it links cpu_telemetry.cpp), plus in-game testing once a helper exists.
// The pure decision function below is where the real logic — and every torn /
// stale / malformed edge — is pinned.
// =============================================================================

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "utils/cpu_telemetry.h"

using openxr_api_layer::detail::CpuTelemetrySample;
using openxr_api_layer::detail::evaluateCpuTelemetryRead;
using openxr_api_layer::detail::kCpuTelemetryMagic;
using openxr_api_layer::detail::kCpuTelemetrySchemaVersion;
using openxr_api_layer::detail::kCpuTelemetryStalenessNs;

namespace {
    // 1 tick == 1 ns, the same identity convention the overlay aggregator
    // tests use — lets us pass nowQpc / timestamp_qpc as plain nanosecond
    // counts without computing a fake QPC frequency.
    constexpr int64_t kFreq = 1'000'000'000LL;

    // A "good" read: correct magic + schema, even/matching seq, fresh
    // timestamp, finite temp. Each test perturbs exactly one input.
    CpuTelemetrySample good(float tempC = 55.0f) {
        return evaluateCpuTelemetryRead(
            kCpuTelemetryMagic, kCpuTelemetrySchemaVersion,
            /*seq_before=*/4u, /*seq_after=*/4u,
            /*timestamp_qpc=*/0, tempC,
            /*nowQpc=*/100'000'000LL /* 100 ms old */, kFreq,
            kCpuTelemetryStalenessNs);
    }
} // namespace

TEST_CASE("CpuTelemetrySample: default-constructed signals 'no data'") {
    CpuTelemetrySample s;
    CHECK_FALSE(std::isfinite(s.cpu_temp_c));  // NaN sentinel
    CHECK_FALSE(s.temp_valid);
}

TEST_CASE("evaluateCpuTelemetryRead: well-formed fresh read yields the temp") {
    const auto s = good(61.5f);
    REQUIRE(s.temp_valid);
    CHECK(s.cpu_temp_c == doctest::Approx(61.5f));
}

TEST_CASE("evaluateCpuTelemetryRead: wrong magic → no data") {
    const auto s = evaluateCpuTelemetryRead(
        0xDEADBEEFu, kCpuTelemetrySchemaVersion, 4u, 4u, 0, 55.0f,
        100'000'000LL, kFreq, kCpuTelemetryStalenessNs);
    CHECK_FALSE(s.temp_valid);
    CHECK_FALSE(std::isfinite(s.cpu_temp_c));
}

TEST_CASE("evaluateCpuTelemetryRead: unknown schema version → no data") {
    const auto s = evaluateCpuTelemetryRead(
        kCpuTelemetryMagic, kCpuTelemetrySchemaVersion + 1u, 4u, 4u, 0, 55.0f,
        100'000'000LL, kFreq, kCpuTelemetryStalenessNs);
    CHECK_FALSE(s.temp_valid);
}

TEST_CASE("evaluateCpuTelemetryRead: odd seq (writer mid-update) → torn → no data") {
    const auto s = evaluateCpuTelemetryRead(
        kCpuTelemetryMagic, kCpuTelemetrySchemaVersion,
        /*seq_before=*/5u, /*seq_after=*/5u, 0, 55.0f,
        100'000'000LL, kFreq, kCpuTelemetryStalenessNs);
    CHECK_FALSE(s.temp_valid);
}

TEST_CASE("evaluateCpuTelemetryRead: seq changed between loads → torn → no data") {
    const auto s = evaluateCpuTelemetryRead(
        kCpuTelemetryMagic, kCpuTelemetrySchemaVersion,
        /*seq_before=*/4u, /*seq_after=*/6u, 0, 55.0f,
        100'000'000LL, kFreq, kCpuTelemetryStalenessNs);
    CHECK_FALSE(s.temp_valid);
}

TEST_CASE("evaluateCpuTelemetryRead: stale timestamp (dead helper) → no data") {
    // Age = 3 s with a 2 s staleness budget.
    const auto s = evaluateCpuTelemetryRead(
        kCpuTelemetryMagic, kCpuTelemetrySchemaVersion, 4u, 4u,
        /*timestamp_qpc=*/0, 55.0f,
        /*nowQpc=*/3'000'000'000LL, kFreq, kCpuTelemetryStalenessNs);
    CHECK_FALSE(s.temp_valid);
}

TEST_CASE("evaluateCpuTelemetryRead: exactly at the staleness boundary is accepted") {
    // Age == budget (not strictly greater) → still fresh.
    const auto s = evaluateCpuTelemetryRead(
        kCpuTelemetryMagic, kCpuTelemetrySchemaVersion, 4u, 4u,
        /*timestamp_qpc=*/0, 55.0f,
        /*nowQpc=*/kCpuTelemetryStalenessNs, kFreq, kCpuTelemetryStalenessNs);
    CHECK(s.temp_valid);
}

TEST_CASE("evaluateCpuTelemetryRead: timestamp in the future → bogus → no data") {
    // nowQpc < timestamp_qpc: clock glitch or garbage write.
    const auto s = evaluateCpuTelemetryRead(
        kCpuTelemetryMagic, kCpuTelemetrySchemaVersion, 4u, 4u,
        /*timestamp_qpc=*/500'000'000LL, 55.0f,
        /*nowQpc=*/0, kFreq, kCpuTelemetryStalenessNs);
    CHECK_FALSE(s.temp_valid);
}

TEST_CASE("evaluateCpuTelemetryRead: helper published NaN → no data") {
    const auto s = good(std::numeric_limits<float>::quiet_NaN());
    CHECK_FALSE(s.temp_valid);
    CHECK_FALSE(std::isfinite(s.cpu_temp_c));
}

TEST_CASE("evaluateCpuTelemetryRead: zero frequency disables the staleness gate") {
    // qpcFrequency <= 0 → we can't time-check, so we trust the (untorn,
    // finite) reading rather than blanking it forever. Even an ancient
    // timestamp passes.
    const auto s = evaluateCpuTelemetryRead(
        kCpuTelemetryMagic, kCpuTelemetrySchemaVersion, 4u, 4u,
        /*timestamp_qpc=*/0, 55.0f,
        /*nowQpc=*/999'000'000'000LL, /*qpcFrequency=*/0,
        kCpuTelemetryStalenessNs);
    CHECK(s.temp_valid);
    CHECK(s.cpu_temp_c == doctest::Approx(55.0f));
}

TEST_CASE("evaluateCpuTelemetryRead: a realistic CPU temperature round-trips") {
    // Sanity band — the helper hands us °C, and we don't mangle it.
    for (float t : {28.0f, 45.0f, 72.0f, 95.0f}) {
        const auto s = good(t);
        REQUIRE(s.temp_valid);
        CHECK(s.cpu_temp_c == doctest::Approx(t));
    }
}

#if defined(_WIN32)
// Reader plumbing only links on Windows (cpu_telemetry.cpp pulls <windows.h>).
// CI runners have no helper publishing to Local\XrTelemetryCpu, so this pins
// the DEFENSIVE contract: construction, poll-before-init, and init-with-no-
// helper must all be crash-free and report "no data" cleanly.
TEST_CASE("CpuTelemetryReader: poll() before init() is safe and empty") {
    openxr_api_layer::detail::CpuTelemetryReader reader;
    const auto s = reader.poll();
    CHECK_FALSE(s.temp_valid);
    CHECK_FALSE(std::isfinite(s.cpu_temp_c));
}

TEST_CASE("CpuTelemetryReader: init() with no helper returns false but stays usable") {
    openxr_api_layer::detail::CpuTelemetryReader reader;
    // No helper on CI → mapping absent → not ready. Must not crash, and a
    // subsequent poll() must still be safe (it lazily retries the connect).
    reader.init();
    CHECK_FALSE(reader.isReady());
    const auto s = reader.poll();
    CHECK_FALSE(s.temp_valid);
}
#endif  // _WIN32
