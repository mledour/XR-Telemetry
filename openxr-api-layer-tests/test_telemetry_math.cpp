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
// test_telemetry_math.cpp — pure-function unit tests on the bits of layer.cpp
// that don't need an OpenXR runtime or a D3D11 device to verify.
//
// The integration tests in test_telemetry.cpp exercise the whole layer
// black-box (xrCreateInstance → … → xrDestroyInstance), but the
// patchAndDrainPending / flushPendingFramesUnresolved / disjoint-conversion
// paths that V2 of the layer added are skipped there because the mock
// runtime has no D3D11 device — the GpuTimer stays inactive and the
// pending-frame deque stays empty.
//
// This file targets the math + bookkeeping helpers extracted into
// telemetry_internals.h. No pch.h, no mock_runtime, no fixture — just
// pure-function asserts on synthetic inputs.
// =============================================================================

#include <doctest/doctest.h>

#include "telemetry_internals.h"

#include <deque>
#include <vector>

using openxr_api_layer::detail::FrameRecord;
using openxr_api_layer::detail::qpcToNs;
using openxr_api_layer::detail::qpcDeltaNsOrZero;
using openxr_api_layer::detail::gpuTimestampDeltaToNs;
using openxr_api_layer::detail::gpuTimestampPairToNs;
using openxr_api_layer::detail::computeCpuHeadroomPct;
using openxr_api_layer::detail::computeGpuHeadroomPct;
using openxr_api_layer::detail::patchAndDrainPending;
using openxr_api_layer::detail::flushPendingFramesUnresolved;

namespace {

    // A throwaway FrameRecord with sensible defaults; tests tweak only the
    // fields they assert against. period_ns is set to 90 Hz @ 11.111 ms,
    // matching the mock runtime used in the integration tests so the
    // numbers in these tests stay recognisable.
    FrameRecord makeRecord(uint64_t frameIndex) {
        FrameRecord r{};
        r.frame_index = frameIndex;
        r.period_ns = 11'111'111;
        // Placeholders the production code uses on the pending-deque path:
        r.gpu_time_ns = 0;
        r.gpu_headroom_pct = 100.0f;
        return r;
    }

    // Sink that captures records pushed by patch / flush, so tests can
    // inspect what got drained and in what order.
    struct RecordingSink {
        std::vector<FrameRecord> log;
        void operator()(const FrameRecord& r) {
            log.push_back(r);
        }
    };

} // namespace

// =============================================================================
// qpcToNs — QueryPerformanceCounter ticks → nanoseconds.
// =============================================================================

TEST_CASE("qpcToNs: zero ticks → zero ns") {
    CHECK(qpcToNs(0, 10'000'000) == 0);
}

TEST_CASE("qpcToNs: exactly freq ticks → exactly 1 second (1e9 ns)") {
    CHECK(qpcToNs(10'000'000, 10'000'000) == 1'000'000'000);
}

TEST_CASE("qpcToNs: realistic 11.11 ms frame at 10 MHz QPC") {
    // Modern Windows QPC frequency is typically 10 MHz. One 90 Hz frame
    // (11.111 ms) at that rate is 111_111 ticks → 11_111_100 ns.
    const auto ns = qpcToNs(111'111, 10'000'000);
    CHECK(ns == 11'111'100);
}

TEST_CASE("qpcToNs: freq <= 0 returns 0 (defensive guard)") {
    CHECK(qpcToNs(12'345, 0) == 0);
    CHECK(qpcToNs(12'345, -1) == 0);
}

// qpcDeltaNsOrZero — guarded delta for the per-frame windows in xrEndFrame
// (wait_block / app_cpu / pre_begin / render). Returns 0 for an invalid or
// out-of-order anchor so a skipped wait/begin can't yield a bogus over-a-
// frame span (the staleness that would otherwise break Render ≤ App).

TEST_CASE("qpcDeltaNsOrZero: forward-ordered delta equals qpcToNs of the difference") {
    CHECK(qpcDeltaNsOrZero(1'000, 1'000 + 111'111, 10'000'000)
          == qpcToNs(111'111, 10'000'000));
}

TEST_CASE("qpcDeltaNsOrZero: from <= 0 returns 0 (uninitialised / cleared anchor)") {
    CHECK(qpcDeltaNsOrZero(0, 12'345, 10'000'000) == 0);
    CHECK(qpcDeltaNsOrZero(-5, 12'345, 10'000'000) == 0);
}

TEST_CASE("qpcDeltaNsOrZero: to <= from returns 0 (stale / out-of-order anchor)") {
    CHECK(qpcDeltaNsOrZero(20'000, 10'000, 10'000'000) == 0);  // stale > fresh
    CHECK(qpcDeltaNsOrZero(10'000, 10'000, 10'000'000) == 0);  // equal endpoints
}

TEST_CASE("qpcToNs: no overflow on long intervals (10 minutes at 10 MHz)") {
    // 10 minutes × 60 s × 10 MHz = 6e9 ticks → 600 s = 6e11 ns.
    // If the implementation naively did (delta * 1e9) it would overflow
    // int64 (max ~9.2e18 vs delta_max = 6e9 × 1e9 = 6e18 — actually
    // within int64 range but already past int32 max). The whole+rem split
    // keeps the intermediates well-bounded for centuries.
    const int64_t ns = qpcToNs(6'000'000'000LL, 10'000'000);
    CHECK(ns == 600'000'000'000LL);
}

TEST_CASE("qpcToNs: fractional remainder is preserved (not rounded to seconds)") {
    // 10_500_001 ticks at 10 MHz = 1.0500001 s = 1_050_000_100 ns
    // (with one tick = 100 ns).
    const int64_t ns = qpcToNs(10'500'001, 10'000'000);
    CHECK(ns == 1'050'000'100);
}

// =============================================================================
// gpuTimestampDeltaToNs — D3D11 timestamp delta → nanoseconds.
//
// Same split-and-recombine math as qpcToNs but for unsigned inputs (the
// raw values come from D3D11_QUERY_DATA_TIMESTAMP_DISJOINT::Frequency
// and successive D3D11_QUERY_TIMESTAMP reads, all uint64_t).
// =============================================================================

TEST_CASE("gpuTimestampDeltaToNs: zero delta → zero ns") {
    CHECK(gpuTimestampDeltaToNs(0, 1'000'000'000ULL) == 0);
}

TEST_CASE("gpuTimestampDeltaToNs: freq == 0 returns 0 (disjoint sentinel)") {
    // When the GPU's disjoint query reports Disjoint == true, the driver
    // typically zeros out Frequency. Our caller checks Disjoint first, but
    // the helper itself must be defensive too.
    CHECK(gpuTimestampDeltaToNs(12'345, 0) == 0);
}

TEST_CASE("gpuTimestampDeltaToNs: 1 GHz GPU, 1 ms of work") {
    // 1 GHz GPU → 1 tick = 1 ns. 1 ms = 1_000_000 ticks → 1_000_000 ns.
    CHECK(gpuTimestampDeltaToNs(1'000'000ULL, 1'000'000'000ULL) == 1'000'000);
}

TEST_CASE("gpuTimestampDeltaToNs: realistic 4 ms frame at typical GPU freq") {
    // Common reported GPU frequency is 25 MHz on Intel iGPUs, or in the
    // low GHz on discrete cards. Pick 1.2 GHz as a stand-in for an
    // RTX-class chip: 4 ms × 1.2e9 = 4_800_000 ticks.
    constexpr uint64_t freq = 1'200'000'000ULL;
    constexpr uint64_t delta = 4'800'000ULL;
    CHECK(gpuTimestampDeltaToNs(delta, freq) == 4'000'000);
}

TEST_CASE("gpuTimestampDeltaToNs: handles large deltas without overflow") {
    // 1 second of work at 1 GHz = 1e9 ticks. delta * 1e9 = 1e18, within
    // int64 range but close to its limit (~9.2e18). The split-and-
    // recombine path makes this safe.
    constexpr uint64_t freq = 1'000'000'000ULL;
    CHECK(gpuTimestampDeltaToNs(freq, freq) == 1'000'000'000);
}

// =============================================================================
// gpuTimestampPairToNs — pair-of-timestamps wrapper used by BOTH the D3D11
// and D3D12 timer resolve loops. Concentrates the freq==0 + tEnd<=tStart
// defensive checks so they live in exactly one place.
// =============================================================================

TEST_CASE("gpuTimestampPairToNs: nominal pair → delta in nanoseconds") {
    // 1 GHz GPU clock, 4 ms of work between two timestamps. Resolves to
    // 4_000_000 ns (4 ms), same as gpuTimestampDeltaToNs(4_000_000, 1e9).
    constexpr uint64_t freq = 1'000'000'000ULL;
    constexpr uint64_t tStart = 1'234'567'890ULL;
    constexpr uint64_t tEnd = tStart + 4'000'000ULL;
    CHECK(gpuTimestampPairToNs(tStart, tEnd, freq) == 4'000'000);
}

TEST_CASE("gpuTimestampPairToNs: tEnd == tStart → 0 (no work spans 0 ticks)") {
    // Equal timestamps imply zero-duration work. Don't divide-by-anything,
    // don't return a negative — the helper short-circuits at the guard.
    CHECK(gpuTimestampPairToNs(1'000ULL, 1'000ULL, 1'000'000'000ULL) == 0);
}

TEST_CASE("gpuTimestampPairToNs: tEnd < tStart → 0 (WMR driver bug)") {
    // Some WMR drivers occasionally return an end-of-frame timestamp below
    // the start. Without this guard the unsigned subtraction would wrap to
    // ~2^64 and produce a giant bogus duration; the helper returns 0
    // instead so the per-frame headroom math stays sane.
    CHECK(gpuTimestampPairToNs(2'000ULL, 1'000ULL, 1'000'000'000ULL) == 0);
}

TEST_CASE("gpuTimestampPairToNs: freq == 0 → 0 (disjoint / uninitialized)") {
    // Disjoint query reports Frequency=0 on D3D11; GetTimestampFrequency
    // returning 0 would be an init failure on D3D12. Either way we return
    // 0 ns rather than risk a divide-by-zero in the underlying helper.
    CHECK(gpuTimestampPairToNs(1'000ULL, 5'000ULL, 0) == 0);
}

TEST_CASE("gpuTimestampPairToNs: realistic 11.1 ms frame at 1.2 GHz") {
    // RTX-class card, an 11.1 ms frame ≈ 13_320_000 GPU ticks.
    constexpr uint64_t freq = 1'200'000'000ULL;
    constexpr uint64_t tStart = 999'999'999'000ULL;
    constexpr uint64_t tEnd = tStart + 13'320'000ULL;
    CHECK(gpuTimestampPairToNs(tStart, tEnd, freq) == 11'100'000);
}

// =============================================================================
// computeCpuHeadroomPct / computeGpuHeadroomPct — % of period not spent.
// =============================================================================

TEST_CASE("computeCpuHeadroomPct: half the period → 50%") {
    CHECK(computeCpuHeadroomPct(5'555'555, 11'111'111) == doctest::Approx(50.0f).epsilon(0.001));
}

TEST_CASE("computeCpuHeadroomPct: zero work → 100% headroom") {
    CHECK(computeCpuHeadroomPct(0, 11'111'111) == doctest::Approx(100.0f).epsilon(0.001));
}

TEST_CASE("computeCpuHeadroomPct: work equals period → 0% headroom (on budget)") {
    CHECK(computeCpuHeadroomPct(11'111'111, 11'111'111) == doctest::Approx(0.0f).epsilon(0.001));
}

TEST_CASE("computeCpuHeadroomPct: app exceeds period → negative headroom (CPU-bound)") {
    // 1.5 × period of work → headroom = -50%.
    CHECK(computeCpuHeadroomPct(16'666'666, 11'111'111) == doctest::Approx(-50.0f).epsilon(0.001));
}

TEST_CASE("computeCpuHeadroomPct: period <= 0 → 100% (no-measurement sentinel)") {
    CHECK(computeCpuHeadroomPct(5'555'555, 0) == 100.0f);
    CHECK(computeCpuHeadroomPct(5'555'555, -1) == 100.0f);
}

TEST_CASE("computeGpuHeadroomPct: same formula as CPU") {
    // The two helpers are intentionally identical in shape; one test per
    // distinct interesting case is enough.
    CHECK(computeGpuHeadroomPct(0, 11'111'111) == doctest::Approx(100.0f).epsilon(0.001));
    CHECK(computeGpuHeadroomPct(5'555'555, 11'111'111) == doctest::Approx(50.0f).epsilon(0.001));
    CHECK(computeGpuHeadroomPct(1'000, 0) == 100.0f);  // sentinel
}

// =============================================================================
// patchAndDrainPending — match / stale / future ordering across the deque.
// =============================================================================

TEST_CASE("patchAndDrainPending: empty deque is a no-op") {
    std::deque<FrameRecord> pending;
    RecordingSink sink;
    patchAndDrainPending(pending, /*resolvedFrameIndex=*/42, /*gpuTimeNs=*/1000, sink);
    CHECK(sink.log.empty());
    CHECK(pending.empty());
}

TEST_CASE("patchAndDrainPending: single matching entry → patched + drained") {
    std::deque<FrameRecord> pending;
    pending.push_back(makeRecord(/*frame_index=*/5));

    RecordingSink sink;
    patchAndDrainPending(pending, /*resolvedFrameIndex=*/5, /*gpuTimeNs=*/5'555'555, sink);

    REQUIRE(sink.log.size() == 1);
    CHECK(sink.log[0].frame_index == 5);
    CHECK(sink.log[0].gpu_time_ns == 5'555'555);
    // 5_555_555 / 11_111_111 ≈ 50% used → ~50% headroom.
    CHECK(sink.log[0].gpu_headroom_pct == doctest::Approx(50.0f).epsilon(0.001));
    CHECK(pending.empty());
}

TEST_CASE("patchAndDrainPending: future-only entries stay in the deque") {
    // resolved=5 but the only pending entry is frame_index=10 — that's
    // for a later cycle. Drain should not touch it.
    std::deque<FrameRecord> pending;
    pending.push_back(makeRecord(10));

    RecordingSink sink;
    patchAndDrainPending(pending, /*resolvedFrameIndex=*/5, /*gpuTimeNs=*/9'999, sink);

    CHECK(sink.log.empty());
    REQUIRE(pending.size() == 1);
    CHECK(pending.front().frame_index == 10);
    CHECK(pending.front().gpu_time_ns == 0);          // unchanged
    CHECK(pending.front().gpu_headroom_pct == 100.0f);// unchanged
}

TEST_CASE("patchAndDrainPending: stale entries are pushed unpatched, then match is patched") {
    // The ring overran and lost results for frames 1 and 2; frame 3 just
    // resolved. The drain should:
    //   - push frame 1 with gpu_time_ns = 0 (sentinel)
    //   - push frame 2 with gpu_time_ns = 0 (sentinel)
    //   - patch frame 3 with the resolved gpu_time_ns and push it
    //   - leave frame 4 untouched (future)
    std::deque<FrameRecord> pending;
    pending.push_back(makeRecord(1));
    pending.push_back(makeRecord(2));
    pending.push_back(makeRecord(3));
    pending.push_back(makeRecord(4));

    RecordingSink sink;
    patchAndDrainPending(pending, /*resolvedFrameIndex=*/3, /*gpuTimeNs=*/2'222'222, sink);

    REQUIRE(sink.log.size() == 3);
    CHECK(sink.log[0].frame_index == 1);
    CHECK(sink.log[0].gpu_time_ns == 0);          // stale, untouched
    CHECK(sink.log[0].gpu_headroom_pct == 100.0f);
    CHECK(sink.log[1].frame_index == 2);
    CHECK(sink.log[1].gpu_time_ns == 0);          // stale, untouched
    CHECK(sink.log[1].gpu_headroom_pct == 100.0f);
    CHECK(sink.log[2].frame_index == 3);
    CHECK(sink.log[2].gpu_time_ns == 2'222'222);  // patched
    // 2_222_222 / 11_111_111 = 20% used → 80% headroom.
    CHECK(sink.log[2].gpu_headroom_pct == doctest::Approx(80.0f).epsilon(0.001));

    REQUIRE(pending.size() == 1);
    CHECK(pending.front().frame_index == 4);      // future, left alone
}

TEST_CASE("patchAndDrainPending: all stale (resolved past every entry) drains everything") {
    // Edge case: resolved=10 but every pending entry is <= 10. The loop
    // pushes them all as stale and pending ends empty.
    std::deque<FrameRecord> pending;
    pending.push_back(makeRecord(1));
    pending.push_back(makeRecord(2));
    pending.push_back(makeRecord(3));

    RecordingSink sink;
    patchAndDrainPending(pending, /*resolvedFrameIndex=*/10, /*gpuTimeNs=*/12'345, sink);

    REQUIRE(sink.log.size() == 3);
    CHECK(sink.log[0].frame_index == 1);
    CHECK(sink.log[1].frame_index == 2);
    CHECK(sink.log[2].frame_index == 3);
    for (const auto& r : sink.log) {
        CHECK(r.gpu_time_ns == 0);  // no match, no patch
    }
    CHECK(pending.empty());
}

TEST_CASE("patchAndDrainPending: match patches gpu_headroom_pct against the entry's period") {
    // Two entries with DIFFERENT period_ns (the runtime briefly reported 0
    // on the first frame, then settled at 90 Hz). The match should use
    // each entry's own period_ns, not a global setting.
    std::deque<FrameRecord> pending;
    auto r1 = makeRecord(1);
    r1.period_ns = 0;             // sentinel branch
    pending.push_back(r1);
    auto r2 = makeRecord(2);
    r2.period_ns = 22'222'222;    // 45 Hz (motion reprojection)
    pending.push_back(r2);

    RecordingSink sink;
    patchAndDrainPending(pending, /*resolvedFrameIndex=*/2, /*gpuTimeNs=*/5'555'555, sink);

    REQUIRE(sink.log.size() == 2);
    // r1 was stale; gpu_time stays 0, gpu_headroom stays at the
    // placeholder 100% from makeRecord().
    CHECK(sink.log[0].frame_index == 1);
    CHECK(sink.log[0].gpu_time_ns == 0);
    CHECK(sink.log[0].gpu_headroom_pct == 100.0f);
    // r2 matched; gpu_headroom against its own 22.22 ms period:
    // 5_555_555 / 22_222_222 = 25% used → 75% headroom.
    CHECK(sink.log[1].frame_index == 2);
    CHECK(sink.log[1].gpu_time_ns == 5'555'555);
    CHECK(sink.log[1].gpu_headroom_pct == doctest::Approx(75.0f).epsilon(0.001));
}

TEST_CASE("patchAndDrainPending: stops at the match (doesn't drain future entries)") {
    // resolved=2, pending=[1, 2, 3]. After patching frame 2 the loop must
    // break — frame 3 stays for its own resolution.
    std::deque<FrameRecord> pending;
    pending.push_back(makeRecord(1));
    pending.push_back(makeRecord(2));
    pending.push_back(makeRecord(3));

    RecordingSink sink;
    patchAndDrainPending(pending, /*resolvedFrameIndex=*/2, /*gpuTimeNs=*/1'234, sink);

    REQUIRE(sink.log.size() == 2);
    CHECK(sink.log[0].frame_index == 1);
    CHECK(sink.log[1].frame_index == 2);
    REQUIRE(pending.size() == 1);
    CHECK(pending.front().frame_index == 3);
}

// =============================================================================
// flushPendingFramesUnresolved — drain everything, no patching.
// =============================================================================

TEST_CASE("flushPendingFramesUnresolved: drains everything in FIFO order") {
    std::deque<FrameRecord> pending;
    pending.push_back(makeRecord(1));
    pending.push_back(makeRecord(2));
    pending.push_back(makeRecord(3));

    RecordingSink sink;
    flushPendingFramesUnresolved(pending, sink);

    REQUIRE(sink.log.size() == 3);
    CHECK(sink.log[0].frame_index == 1);
    CHECK(sink.log[1].frame_index == 2);
    CHECK(sink.log[2].frame_index == 3);
    for (const auto& r : sink.log) {
        CHECK(r.gpu_time_ns == 0);            // never patched
        CHECK(r.gpu_headroom_pct == 100.0f);  // never patched
    }
    CHECK(pending.empty());
}

TEST_CASE("flushPendingFramesUnresolved: empty deque is a no-op") {
    std::deque<FrameRecord> pending;
    RecordingSink sink;
    flushPendingFramesUnresolved(pending, sink);
    CHECK(sink.log.empty());
    CHECK(pending.empty());
}

// =============================================================================
// findInTypedChain — generic XrBaseInStructure-style chain walker used by
// the D3D11 / D3D12 graphics-binding finders in layer.cpp. Tested here on a
// synthetic chain of BaseLike nodes (the same layout XrBaseInStructure has)
// so we never need to drag in <openxr/openxr.h> for this unit.
// =============================================================================

namespace {
    // A chain node that extends BaseLike with a payload so we can verify
    // the returned pointer actually addresses the matching entry, not a
    // bystander with the same type tag.
    struct ChainNode : openxr_api_layer::detail::BaseLike {
        int payload;
    };

    // Tag values picked to mirror real OpenXR enum integers without
    // requiring the header; they're arbitrary so long as they're distinct.
    constexpr int32_t kTagA = 1001;
    constexpr int32_t kTagB = 1002;
    constexpr int32_t kTagC = 1003;
}

TEST_CASE("findInTypedChain: null head returns nullptr") {
    using openxr_api_layer::detail::findInTypedChain;
    CHECK(findInTypedChain(nullptr, kTagA) == nullptr);
}

TEST_CASE("findInTypedChain: single-node chain, type matches") {
    using openxr_api_layer::detail::findInTypedChain;
    ChainNode a{{kTagA, nullptr}, 42};
    const auto* hit = static_cast<const ChainNode*>(findInTypedChain(&a, kTagA));
    REQUIRE(hit != nullptr);
    CHECK(hit->payload == 42);
}

TEST_CASE("findInTypedChain: single-node chain, type does not match") {
    using openxr_api_layer::detail::findInTypedChain;
    ChainNode a{{kTagA, nullptr}, 42};
    CHECK(findInTypedChain(&a, kTagB) == nullptr);
}

TEST_CASE("findInTypedChain: walks past non-matching prefix to find target") {
    using openxr_api_layer::detail::findInTypedChain;
    ChainNode c{{kTagC, nullptr}, 30};
    ChainNode b{{kTagB, &c}, 20};
    ChainNode a{{kTagA, &b}, 10};
    // Find the middle node — exercise that we don't stop at A and don't
    // overshoot to C.
    const auto* hit = static_cast<const ChainNode*>(findInTypedChain(&a, kTagB));
    REQUIRE(hit != nullptr);
    CHECK(hit->payload == 20);
}

TEST_CASE("findInTypedChain: returns the FIRST match when duplicates exist") {
    using openxr_api_layer::detail::findInTypedChain;
    ChainNode second{{kTagB, nullptr}, 200};
    ChainNode first{{kTagB, &second}, 100};
    ChainNode head{{kTagA, &first}, 1};
    const auto* hit = static_cast<const ChainNode*>(findInTypedChain(&head, kTagB));
    REQUIRE(hit != nullptr);
    CHECK(hit->payload == 100);  // not 200 — early-out on first hit
}

TEST_CASE("findInTypedChain: target not present in any chain entry") {
    using openxr_api_layer::detail::findInTypedChain;
    ChainNode c{{kTagC, nullptr}, 30};
    ChainNode b{{kTagB, &c}, 20};
    ChainNode a{{kTagA, &b}, 10};
    // Look for a tag value nothing in the chain carries.
    CHECK(findInTypedChain(&a, 9999) == nullptr);
}
