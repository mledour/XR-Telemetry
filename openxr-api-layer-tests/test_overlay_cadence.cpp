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
// test_overlay_cadence.cpp — the overlay's two-tier paint cadence.
//
// Drives PaintCadence / needStaticPaint / commitPaint (overlay_cadence.h)
// directly, with no render target. These are the pure decision + state-
// update halves of the renderer's paint(): given the current cadence and
// the incoming snapshot version, decide whether to repaint the expensive
// static chrome (vs just the per-frame histogram), then fold the result
// back in.
//
// The cases mirror the corners that have actually bitten or could bite:
//   - the UINT64_MAX first-frame sentinel,
//   - the steady-state "same version → skip static" path,
//   - the watchdog forcing a static after K frames with a frozen version,
//   - the EndDraw-failure bookkeeping (a static failure must retry; a
//     dynamic failure must still advance the watchdog).
//
// A small driveOneFrame() helper threads the two functions together the
// same way paint() does, so the tests read as "what happens over N frames"
// rather than as isolated function calls.
// =============================================================================

#include <doctest/doctest.h>

#include "utils/overlay_cadence.h"

#include <cstdint>

using openxr_api_layer::detail::PaintCadence;
using openxr_api_layer::detail::needStaticPaint;
using openxr_api_layer::detail::commitPaint;

namespace {

    // The production constant (CoreRenderer::kMaxFramesBetweenPaints). Kept
    // local to the test so a deliberate change to the renderer's watchdog
    // also has to be reflected here — the test then documents the intended
    // cadence rather than blindly tracking the source.
    constexpr int kK = 30;

    // Run one frame through the same sequence paint() uses: decide, then
    // commit with the given EndDraw result. Returns whether the static
    // tier ran this frame (what the renderer would branch on).
    bool driveOneFrame(PaintCadence& c, uint64_t snapVersion,
                       bool endDrawOk, int maxFrames = kK) {
        const bool didStatic = needStaticPaint(c, snapVersion, maxFrames);
        commitPaint(c, didStatic, endDrawOk, snapVersion);
        return didStatic;
    }

} // namespace

TEST_CASE("PaintCadence: first frame always paints static (UINT64_MAX sentinel)") {
    PaintCadence c;  // lastPaintedVersion == UINT64_MAX, framesSincePaint == 0
    // Even with a version of 0 — the lowest an aggregator ever emits — the
    // sentinel differs, so the first frame must take the static branch
    // (there's no prior chrome in the shim to preserve).
    CHECK(needStaticPaint(c, /*snapVersion=*/0, kK) == true);
    CHECK(needStaticPaint(c, /*snapVersion=*/UINT64_MAX - 1, kK) == true);
}

TEST_CASE("PaintCadence: same version on the next frame skips the static tier") {
    PaintCadence c;
    REQUIRE(driveOneFrame(c, /*v=*/7, /*ok=*/true) == true);   // first paint: static
    // Version unchanged → the chrome is already current, so the next frame
    // is dynamic-only (just the scrolling histogram).
    CHECK(driveOneFrame(c, /*v=*/7, /*ok=*/true) == false);
    CHECK(driveOneFrame(c, /*v=*/7, /*ok=*/true) == false);
    CHECK(c.lastPaintedVersion == 7u);
    CHECK(c.framesSincePaint == 2);  // two dynamic frames since the static
}

TEST_CASE("PaintCadence: a version bump forces static and resets the counter") {
    PaintCadence c;
    driveOneFrame(c, /*v=*/1, true);                 // static
    driveOneFrame(c, /*v=*/1, true);                 // dynamic
    REQUIRE(c.framesSincePaint == 1);

    // Aggregator republished (new FPS / temps / etc.) → repaint chrome.
    CHECK(driveOneFrame(c, /*v=*/2, /*ok=*/true) == true);
    CHECK(c.lastPaintedVersion == 2u);
    CHECK(c.framesSincePaint == 0);                  // reset on static
}

TEST_CASE("PaintCadence: watchdog forces a static after K frames with a frozen version") {
    PaintCadence c;
    REQUIRE(driveOneFrame(c, /*v=*/5, true) == true);  // frame 0: static, counter -> 0

    // With the version frozen, frames 1..K-1 are dynamic-only. The Kth
    // dynamic frame is where framesSincePaint+1 reaches K and the watchdog
    // trips. Walk right up to (but not including) that point first.
    int dynamicFrames = 0;
    for (int i = 0; i < kK - 1; ++i) {
        const bool didStatic = driveOneFrame(c, /*v=*/5, /*ok=*/true);
        CHECK(didStatic == false);
        ++dynamicFrames;
    }
    REQUIRE(dynamicFrames == kK - 1);
    REQUIRE(c.framesSincePaint == kK - 1);

    // Next frame: framesSincePaint + 1 == K → watchdog fires even though
    // the version never moved.
    CHECK(driveOneFrame(c, /*v=*/5, /*ok=*/true) == true);
    CHECK(c.framesSincePaint == 0);                  // reset by the forced static
}

TEST_CASE("PaintCadence: counter advances on a dynamic frame even when EndDraw fails") {
    // Regression guard. The bookkeeping used to live inside an `if (ok)`
    // block, so a failed dynamic frame left framesSincePaint frozen — and
    // with a frozen version the watchdog could then never fire, stranding
    // the chrome on stale pixels until the aggregator next ticked.
    PaintCadence c;
    REQUIRE(driveOneFrame(c, /*v=*/3, /*ok=*/true) == true);  // static
    REQUIRE(c.framesSincePaint == 0);

    // Every subsequent dynamic frame fails EndDraw, yet the counter must
    // still climb so the watchdog can eventually force a recovery static.
    for (int i = 1; i <= kK; ++i) {
        const bool didStatic = driveOneFrame(c, /*v=*/3, /*ok=*/false);
        if (i < kK) {
            CHECK(didStatic == false);
            CHECK(c.framesSincePaint == i);
        } else {
            // Kth frame: watchdog fires. (didStatic is true, but EndDraw
            // failed, so the static didn't actually commit — see next case
            // for that distinction. The point here is the counter reached
            // the threshold despite the failures.)
            CHECK(didStatic == true);
        }
    }
}

TEST_CASE("PaintCadence: a failed static paint does not commit and retries next frame") {
    PaintCadence c;  // first frame is static by sentinel
    // Static branch chosen, but EndDraw fails: version must NOT advance,
    // so the next frame still sees needStatic and tries again.
    const bool didStatic1 = needStaticPaint(c, /*v=*/9, kK);
    REQUIRE(didStatic1 == true);
    commitPaint(c, /*didStatic=*/true, /*endDrawOk=*/false, /*v=*/9);
    CHECK(c.lastPaintedVersion == UINT64_MAX);        // unchanged — not committed
    CHECK(c.framesSincePaint == 0);                   // unchanged

    // Next frame retries the static and succeeds this time.
    const bool didStatic2 = needStaticPaint(c, /*v=*/9, kK);
    CHECK(didStatic2 == true);
    commitPaint(c, /*didStatic=*/true, /*endDrawOk=*/true, /*v=*/9);
    CHECK(c.lastPaintedVersion == 9u);                // now committed
    CHECK(c.framesSincePaint == 0);

    // And now that the version is committed, the following frame is
    // dynamic-only — confirming recovery is complete.
    CHECK(driveOneFrame(c, /*v=*/9, /*ok=*/true) == false);
}

TEST_CASE("PaintCadence: maxFrames <= 0 degenerates to every-frame static") {
    // Degenerate-but-valid config: a non-positive watchdog means the
    // watchdog term (framesSincePaint + 1 >= maxFrames) is always true,
    // so every frame is static regardless of version. Documents the edge
    // so a future tweak to the constant can't silently divide-by-nothing.
    PaintCadence c;
    CHECK(driveOneFrame(c, /*v=*/4, /*ok=*/true, /*maxFrames=*/0) == true);
    CHECK(driveOneFrame(c, /*v=*/4, /*ok=*/true, /*maxFrames=*/0) == true);
    CHECK(driveOneFrame(c, /*v=*/4, /*ok=*/true, /*maxFrames=*/-5) == true);
}
