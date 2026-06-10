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
// overlay_cadence.h — the two-tier paint cadence state machine, factored out
// of the overlay renderer's paint() so it can be unit-tested without a live
// ID2D1RenderTarget.
//
// The overlay paints in two tiers:
//   - Static tier: outer frame, header, panel titles + current numeric
//     values, bottom row. Snapshot-driven — only needs rebuilding when the
//     aggregator republishes (~10 Hz), so it is cadence-gated rather than
//     redrawn per frame.
//   - Dynamic tier: the two histogram regions. The ring buffers are fed every
//     frame (pushFrameSample at the host's 90-144 Hz), so the bars must
//     repaint every frame to scroll smoothly.
//
// paint() owns a PaintCadence, asks needStaticPaint() whether this frame
// needs the static tier, draws accordingly, then calls commitPaint() to
// fold the result back into the cadence. Keeping that decision/update logic
// as free functions on a plain struct means the corner cases (first-frame
// sentinel, watchdog timeout, EndDraw-failure bookkeeping) are covered by
// fast unit tests in test_overlay_cadence.cpp rather than by eyeballing a
// rendered PNG.
// =============================================================================

#include <cstdint>

namespace openxr_api_layer::detail {

    // Cadence state for the two-tier paint. Owned by the renderer; one
    // instance per CoreRenderer, persisted across paint() calls.
    struct PaintCadence {
        // snap.version of the last *successful* static paint. UINT64_MAX
        // is the initial sentinel: it differs from every real aggregator
        // version (those start at 0 and only climb), so the very first
        // paint always takes the static branch — there's no prior chrome
        // to preserve.
        uint64_t lastPaintedVersion = UINT64_MAX;

        // Frames elapsed since the last successful static paint. Drives
        // the watchdog (see needStaticPaint): once it would reach
        // maxFramesBetweenStatic, a static paint is forced even with an
        // unchanged snap.version, so a stalled aggregator can't leave the
        // chrome frozen forever.
        int framesSincePaint = 0;
    };

    // Decide whether this frame needs the static (full chrome) tier, or
    // just the dynamic (histogram) tier. Pure function of the current
    // cadence and this frame's snapshot version.
    //
    //   - versionChanged: aggregator published new data since our last
    //     static paint → repaint the chrome so the new values show.
    //   - watchdog: maxFramesBetweenStatic frames elapsed with no version
    //     change → force a static anyway (defensive against a stalled
    //     aggregator).
    //
    // The "+ 1" mirrors counting THIS frame: with framesSincePaint == K-1
    // entering the call, this is the Kth dynamic frame and the watchdog
    // trips. maxFramesBetweenStatic <= 0 means "every frame is static"
    // (the watchdog is always true), which is a valid degenerate config.
    inline bool needStaticPaint(const PaintCadence& c,
                                 uint64_t snapVersion,
                                 int maxFramesBetweenStatic) noexcept {
        const bool versionChanged = snapVersion != c.lastPaintedVersion;
        const bool watchdog = c.framesSincePaint + 1 >= maxFramesBetweenStatic;
        return versionChanged || watchdog;
    }

    // Fold the result of a paint back into the cadence, given which tier
    // ran (didStatic) and whether EndDraw succeeded (endDrawOk).
    //
    //   - Static + ok:  commit the version, reset the counter.
    //   - Static + !ok: leave everything untouched — lastPaintedVersion
    //                   stays behind snapVersion so the next frame retries
    //                   the static branch until it lands.
    //   - Dynamic:      always advance the counter, even on !ok. A dynamic
    //                   frame that fails EndDraw must still progress the
    //                   watchdog; otherwise a sustained dynamic-tier
    //                   failure with a frozen snap.version would freeze the
    //                   counter and the watchdog could never fire to force
    //                   a recovery static paint.
    inline void commitPaint(PaintCadence& c,
                             bool didStatic,
                             bool endDrawOk,
                             uint64_t snapVersion) noexcept {
        if (didStatic) {
            if (endDrawOk) {
                c.lastPaintedVersion = snapVersion;
                c.framesSincePaint = 0;
            }
        } else {
            ++c.framesSincePaint;
        }
    }

} // namespace openxr_api_layer::detail
