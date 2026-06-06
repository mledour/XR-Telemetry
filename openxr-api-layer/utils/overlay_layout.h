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
// overlay_layout.h — pure helpers shared by overlay_renderer.cpp:
//   * formatOverlayDisplayValues: snapshot → POD of pre-formatted strings +
//     numeric values, one entry per cell of the new fpsVR-redesign HUD
//     (header bar with FPS/AVG/P95/P99, two frametime panels with current-
//     value labels, bottom row with TEMP / UTIL cells per side).
//   * geometryForPosition: settings.position string → quad pose constants
//     for the 4:3 720×540 texture layout.
//   * barVisualForSample / budgetLineFraction: per-bar colour + height
//     decisions for the histogram panels (unchanged contract).
//
// Stays self-contained — no OpenXR / D3D / DirectWrite headers. Lets the
// test binary exercise the formatting and geometry math on macOS / Linux
// without the Windows SDK stack.
// =============================================================================

#include "overlay_aggregator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace openxr_api_layer::detail {

    // Snapshot → pre-formatted strings for the new fpsVR-redesign HUD.
    // The renderer never calls snprintf at draw time — it only paints
    // pre-formatted cells. Numeric fields (the *_fraction members) are
    // exposed alongside the string versions so the circular gauges can
    // draw the arc geometry directly without re-parsing the percentage
    // text.
    //
    // Empty / default-constructed when the snapshot is still invalid
    // (caller skips text drawing in that case).
    //
    // Field naming mirrors the visible cell positions:
    //
    //   ── header bar (single row, 5 cells) ─────────────────────────
    //   FPS 142  FPS AVG 138  P95 124  P99 108  P99.9 98
    //   fps_instant fps_avg   fps_p95  fps_p99  fps_p99_9
    //
    //   ── GPU frametime panel ───────────────────────────────────────
    //   GPU FRAMETIME MS                          6.7 ms
    //                                     gpu_frametime_ms
    //   <histogram >
    //
    //   ── CPU frametime panel ───────────────────────────────────────
    //   CPU FRAMETIME MS        App ms 4.3 ms / Render ms 7.4 ms
    //                            cpu_app_ms_str   cpu_frametime_ms
    //   <histogram>
    //
    //   ── bottom row (2 panels, 60/40 split, 5 cells total) ────────
    //   ┌──────────────────────────────────────┬───────────────────┐
    //   │ GPU TEMP │ GPU LOAD │ VRAM           │ CPUs     │ CPU LOAD│
    //   │ 67 °C    │ 92 %     │ 76 %           │ 98 %     │ 78 %    │
    //   └──────────────────────────────────────┴───────────────────┘
    //   GPU panel (3 cells)                    CPU panel (2 cells)
    //   gpu_temp_c   gpu_util_pct    vram_pct  cpus_max_pct cpu_util_pct
    //                gpu_util_fraction         cpus_max_fraction
    //                                                       cpu_util_fraction
    //
    // The CPU panel's first cell is "CPUs": the utilisation of the
    // BUSIEST logical processor (fpsVR's "CPUs"), sourced from
    // CpuUsageReader via the snapshot's cpus_max_pct. It REPLACES the
    // old "CPU TEMP" placeholder — CPU temperature needs ring-0 access
    // (handled separately by the out-of-process helper design), whereas
    // per-core usage comes from a documented user-mode NT call we can
    // make in-process. cpus_max_pct renders "--" only when the source
    // is unavailable (NaN) — same isfinite() guard as the GPU cells.
    struct OverlayDisplayValues {
        // Header bar
        std::string fps_instant      = "--";  // big white number
        std::string fps_avg          = "--";  // cyan accent
        std::string fps_p95          = "--";  // cyan accent
        std::string fps_p99          = "--";  // cyan accent
        std::string fps_p99_9        = "--";  // cyan accent — single
                                                // worst ~0.1 % of frames
                                                // over a 30 s sliding
                                                // window (see aggregator
                                                // for the window math)

        // Frametime panel current-value labels (top-right of each
        // panel — the histogram itself takes the rest of the space).
        // GPU panel only shows the per-cycle GPU time. CPU panel
        // shows BOTH the per-cycle CPU (= "Render ms") AND the
        // app-only window (= "App ms"); the difference is the OpenXR
        // overhead, which is the diagnostic value the design surfaces.
        std::string gpu_frametime_ms = "--.-";
        std::string cpu_frametime_ms = "--.-";  // Render ms
        std::string cpu_app_ms       = "--.-";  // App ms (wait→end)

        // Bottom row — GPU temperature (integer °C, "--" sentinel when
        // source absent). The CPU panel shows "CPUs" (busiest-core
        // utilisation, cpus_max_pct below) in this slot instead of a
        // temperature.
        std::string gpu_temp_c       = "--";

        // Bottom row — "CPUs": busiest-core utilisation %, "--" when the
        // CPU sampler has no reading (NaN). Replaces the old CPU TEMP
        // placeholder.
        std::string cpus_max_pct     = "--";

        // Bottom row — utilisation percentages (drawn both as text
        // inside the gauge AND geometrically as an arc filling 0..N%
        // of a 270° dial).
        std::string gpu_util_pct     = "--";
        std::string cpu_util_pct     = "--";

        // VRAM percentage of budget (= vram_used / vram_budget). Lives
        // in the GPU panel of the bottom row as a third cell next to
        // GPU TEMP / GPU LOAD. "--" when DXGI didn't answer (no
        // adapter, Win10 pre-RS1) — distinct from "0 %" which would
        // be impossible in practice (any live D3D session holds at
        // least a few MB of compositor swapchain).
        std::string vram_pct         = "--";
        // Tier fractions (0..1) drive the warning-tier text colour
        // for LOAD and VRAM in the bottom panels — same cyan / orange
        // / red palette as the histogram bars, same thresholds via
        // gaugeTierForUtilisation (0.80 / 0.90).
        float       gpu_util_fraction = 0.0f;
        float       cpu_util_fraction = 0.0f;
        float       vram_fraction     = 0.0f;
        // Tier fraction (0..1) for the "CPUs" cell — same warning-tier
        // colour ramp (cyan / orange / red) as LOAD / VRAM, since "CPUs"
        // is also a utilisation percentage. 0 when the source is absent.
        float       cpus_max_fraction = 0.0f;

        // True when at least the FPS field carries a real value. The
        // renderer can skip the text/gauge passes (but still paint the
        // frame/background) on the very first ~100 ms of a session
        // before the aggregator publishes the first snapshot.
        bool        valid = false;
    };

    inline OverlayDisplayValues formatOverlayDisplayValues(const OverlaySnapshot& snap) {
        OverlayDisplayValues v;
        if (!snap.valid) return v;
        v.valid = true;

        // The aggregator guards against div-by-zero already, but a
        // momentarily-negative `frame_total - wait_block` can
        // accumulate into NaN / ±Inf. Every formatter here checks
        // std::isfinite() and substitutes "--" / "--.-" so the cells
        // stay at their fixed widths even when the input is briefly
        // garbage. The renderer relies on these fixed widths for
        // pixel-alignment of the right-side accent numbers in the
        // header bar — letting "nan" / "inf" leak through would
        // shift the right column rightward and break the grid.
        auto fmtFpsInt = [](float fps) {
            char buf[16];
            if (std::isfinite(fps) && fps >= 0.0f) {
                std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(fps)));
            } else {
                std::snprintf(buf, sizeof(buf), "--");
            }
            return std::string(buf);
        };
        auto fmtMsOneDecimal = [](float ms) {
            char buf[16];
            if (std::isfinite(ms) && ms >= 0.0f) {
                std::snprintf(buf, sizeof(buf), "%.1f", ms);
            } else {
                std::snprintf(buf, sizeof(buf), "--.-");
            }
            return std::string(buf);
        };
        auto fmtTempInt = [](float c) {
            char buf[16];
            if (std::isfinite(c) && c >= -50.0f && c <= 200.0f) {
                std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(c)));
            } else {
                std::snprintf(buf, sizeof(buf), "--");
            }
            return std::string(buf);
        };
        auto fmtPctInt = [](float p) {
            char buf[16];
            if (std::isfinite(p) && p >= 0.0f) {
                std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(p)));
            } else {
                std::snprintf(buf, sizeof(buf), "--");
            }
            return std::string(buf);
        };

        v.fps_instant      = fmtFpsInt(snap.fps_instant);
        v.fps_avg          = fmtFpsInt(snap.fps_avg);
        v.fps_p95          = fmtFpsInt(snap.fps_p95);
        v.fps_p99          = fmtFpsInt(snap.fps_p99);
        v.fps_p99_9        = fmtFpsInt(snap.fps_p99_9);

        v.gpu_frametime_ms = fmtMsOneDecimal(snap.gpu_frame_ms);
        v.cpu_frametime_ms = fmtMsOneDecimal(snap.cpu_frame_ms);
        v.cpu_app_ms       = fmtMsOneDecimal(snap.cpu_app_ms);

        v.gpu_temp_c       = fmtTempInt(snap.gpu_temp_c);
        // "CPUs" — busiest-core utilisation. NaN (no sampler reading /
        // source absent) flows through fmtPctInt as "--", same as any
        // other percentage cell.
        v.cpus_max_pct     = fmtPctInt(snap.cpus_max_pct);

        v.gpu_util_pct     = fmtPctInt(snap.gpu_utilisation_pct);
        v.cpu_util_pct     = fmtPctInt(snap.cpu_utilisation_pct);

        v.gpu_util_fraction = std::isfinite(snap.gpu_utilisation_pct)
            ? std::clamp(snap.gpu_utilisation_pct / 100.0f, 0.0f, 1.0f)
            : 0.0f;
        v.cpu_util_fraction = std::isfinite(snap.cpu_utilisation_pct)
            ? std::clamp(snap.cpu_utilisation_pct / 100.0f, 0.0f, 1.0f)
            : 0.0f;
        v.cpus_max_fraction = std::isfinite(snap.cpus_max_pct)
            ? std::clamp(snap.cpus_max_pct / 100.0f, 0.0f, 1.0f)
            : 0.0f;

        // VRAM percentage of budget. Both bytes values come from
        // DXGI_QUERY_VIDEO_MEMORY_INFO. Guard: both must be non-zero
        // (a 0 budget would div-by-zero and a 0 used means DXGI
        // returned nothing — see gpu_telemetry.h for the sentinel
        // contract).
        if (snap.vram_used_bytes > 0 && snap.vram_budget_bytes > 0) {
            const double frac =
                static_cast<double>(snap.vram_used_bytes) /
                static_cast<double>(snap.vram_budget_bytes);
            v.vram_fraction = static_cast<float>(std::clamp(frac, 0.0, 1.0));
            v.vram_pct = fmtPctInt(v.vram_fraction * 100.0f);
        } else {
            v.vram_fraction = 0.0f;
            v.vram_pct = "--";
        }
        return v;
    }


    // The geometry of the head-locked quad. Coordinates are in OpenXR
    // view space (XR_REFERENCE_SPACE_TYPE_VIEW): +X right, +Y up,
    // -Z forward.
    //
    // The renderer turns these into an XrPosef + XrExtent2Df at draw
    // time — no OpenXR types here so this header stays portable.
    struct OverlayGeometry {
        // Quad CENTER position relative to the view origin (metres).
        float pos_x;
        float pos_y;
        float pos_z;  // negative = in front of the user
        // Quad dimensions in metres at the chosen distance. The
        // default width / height ratio matches the kTexW / kTexH
        // texture aspect so pixels stay square in the HMD with no
        // anisotropic stretching; see kBaseWidth / kBaseHeight in
        // geometryForPosition below for the concrete values.
        float width_m;
        float height_m;
    };

    // Resolve a settings.overlay.position string into a concrete geometry.
    // Unknown strings fall back to head_top_right — same robustness
    // contract as parseHotkey's unknown-key fallback.
    //
    // `scale` multiplies the default quad dimensions (already clamped to
    // [0.5, 2.0] by parseSettings). `offset_x` / `offset_y` are an extra
    // view-space nudge in metres applied to the quad CENTRE on top of the
    // preset corner (+X right, +Y up; already clamped to [-1, 1] by the
    // parser). They let a user push the HUD further into a corner — or back
    // off it — without a rebuild; the corner presets below are just the
    // starting point.
    inline OverlayGeometry geometryForPosition(const std::string& position,
                                                float scale,
                                                float offset_x = 0.0f,
                                                float offset_y = 0.0f) noexcept {
        // Aspect matches the renderer's kTexW × kTexH texture. Quad
        // dimensions chosen so pixel density stays square in the HMD:
        // width_m / height_m == kTexW / kTexH. At 1 m view-space
        // distance the quad covers a fixed horizontal FOV (kBaseWidth);
        // the vertical FOV tracks the texture aspect each time the
        // layout's vertical budget changes. Corner offsets are tuned
        // so the quad's CORNER (not centre) lands well off-axis, close
        // to the edge of a comfortable reading zone — they're a starting
        // point the user can fine-tune via offset_x / offset_y.
        constexpr float kBaseWidth  = 0.28f;
        constexpr float kBaseHeight = 0.162f;  // tracks kTexH/kTexW (see above) so
                                               // HMD pixels stay square; recompute
                                               // whenever the texture height changes.
        constexpr float kZ          = -1.0f;          // 1 m forward
        // Pushed further toward the corner than the original 0.22 / 0.14 so
        // the HUD hugs the edge of the FOV out of the box and gets less in
        // the way mid-game; the user dials it the rest of the way with
        // offset_x / offset_y. By DEFAULT (offset 0) the quad's outer edge
        // sits at ~0.30 + 0.14 = 0.44 m (~24° off-axis at 1 m), well inside
        // a typical ~±50° HMD FOV. NOTE this is the default only — a user
        // offset_x/_y (clamped to ±1.0 m by the parser) can deliberately
        // push the centre out to ~1.30 m (~52°), past that comfortable
        // bound; the clamp is the only guard, and dialing the HUD partly
        // off-screen is the user's call.
        constexpr float kCornerOffX = 0.30f;
        constexpr float kCornerOffY = 0.18f;

        OverlayGeometry g;
        g.width_m  = kBaseWidth  * scale;
        g.height_m = kBaseHeight * scale;
        g.pos_z    = kZ;
        g.pos_x    = +kCornerOffX;
        g.pos_y    = +kCornerOffY;

        // String compare without iequalsAscii dependency: positions are
        // normalised in the parser to lowercase snake_case. Keep this
        // table small — every entry adds to the per-frame branching
        // budget of the renderer.
        if (position == "head_top_left") {
            g.pos_x = -kCornerOffX;
        } else if (position == "head_top_center") {
            g.pos_x = 0.0f;
        } else if (position == "head_center") {
            g.pos_x = 0.0f;
            g.pos_y = 0.0f;
        }
        // anything else → head_top_right (already set above)

        // User nudge, applied last so it composes with any preset.
        g.pos_x += offset_x;
        g.pos_y += offset_y;
        return g;
    }

    // --- World-anchor pose math -------------------------------------------
    //
    // POD mirrors of XrVector3f / XrQuaternionf / XrPosef, field-for-field
    // (XrPosef is { XrQuaternionf orientation; XrVector3f position; },
    // XrQuaternionf is {x,y,z,w}, XrVector3f is {x,y,z}). Declared here
    // — instead of using the OpenXR types — so this header stays buildable
    // on macOS / Linux for the unit tests (see the file banner). The
    // renderer copies field-by-field between these and the real Xr types.
    struct OverlayVec3 { float x, y, z; };
    struct OverlayQuat { float x, y, z, w; };
    struct OverlayPose { OverlayQuat orientation; OverlayVec3 position; };

    // Rotate a vector by a unit quaternion (the q·v·q⁻¹ sandwich, in the
    // branch-free t = 2·(q.xyz × v) form). Used to express the head-local
    // quad offset in world coordinates when freezing a world anchor.
    inline OverlayVec3 rotateByQuat(const OverlayQuat& q,
                                    const OverlayVec3& v) noexcept {
        const float tx = 2.0f * (q.y * v.z - q.z * v.y);
        const float ty = 2.0f * (q.z * v.x - q.x * v.z);
        const float tz = 2.0f * (q.x * v.y - q.y * v.x);
        return {
            v.x + q.w * tx + (q.y * tz - q.z * ty),
            v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx),
        };
    }

    // Strip a quaternion down to its yaw (rotation about world +Y) only,
    // discarding pitch and roll, and return it normalised. This is the
    // swing–twist "twist about Y" component: for a unit quaternion the
    // twist about Y is (0, qy, 0, qw) renormalised. Used so a world-locked
    // panel always hangs upright and level, no matter how the user's head
    // was tilted at the instant it was summoned (looking down, head cocked).
    //
    // Renormalising here also means we never hand a non-unit orientation to
    // XrCompositionLayerQuad.pose, even if the runtime returned a slightly
    // drifted (non-unit) head orientation from xrLocateSpace.
    //
    // Degenerate case — qy and qw both ~0 (head rolled ~180° onto its side,
    // so the yaw is undefined): fall back to identity (face +Z / world
    // forward) rather than dividing by zero.
    inline OverlayQuat yawOnlyQuat(const OverlayQuat& q) noexcept {
        const float n = std::sqrt(q.y * q.y + q.w * q.w);
        if (n < 1e-6f) {
            return {0.0f, 0.0f, 0.0f, 1.0f};
        }
        const float inv = 1.0f / n;
        return {0.0f, q.y * inv, 0.0f, q.w * inv};
    }

    // Freeze the head-locked quad into a world-space pose at activation.
    //   `head`   — the headset pose in the world (LOCAL) reference space,
    //              from xrLocateSpace(viewSpace, localSpace, …).
    //   `offset` — the quad CENTRE offset in the head's local frame, i.e.
    //              the geometryForPosition() {pos_x, pos_y, pos_z}.
    //
    // ORIENTATION takes the head's YAW only, so the panel always hangs
    // upright — a tilted head (roll) or a glance up/down at the summon
    // instant never leaves the panel permanently pitched or rolled.
    //
    // POSITION, however, is placed along the head's FULL gaze (pitch
    // included): the offset is rotated by the complete head orientation, so
    // looking up when you summon the HUD anchors it higher and looking down
    // anchors it lower. You aim the panel's height with your gaze, but it
    // still ends up vertical and readable. The result is handed straight to
    // XrCompositionLayerQuad.pose and stays fixed in the play space until
    // the next anchor.
    inline OverlayPose composeAnchorPose(const OverlayPose& head,
                                         const OverlayVec3& offset) noexcept {
        const OverlayVec3 r = rotateByQuat(head.orientation, offset);
        return {
            yawOnlyQuat(head.orientation),
            { head.position.x + r.x,
              head.position.y + r.y,
              head.position.z + r.z },
        };
    }

    // Convert one histogram sample (nanoseconds) into a 0..1 normalised
    // bar height, given the highest sample currently in the ring. Used
    // by older fall-back paths; the current renderer uses
    // barVisualForSample below for budget-anchored bars + colour tiers.
    //
    // Returns 0 if the ring is empty or sample is non-positive. Capped
    // at 1.0 so a single spike doesn't overflow the bar area visually.
    inline float normaliseBar(int64_t sampleNs, int64_t maxNs) noexcept {
        if (maxNs <= 0 || sampleNs <= 0) return 0.0f;
        const float ratio = static_cast<float>(sampleNs) / static_cast<float>(maxNs);
        return std::clamp(ratio, 0.0f, 1.0f);
    }

    // Colour tier for a histogram bar — fpsVR-style "headroom
    // warning" tiering: the bar stays at the panel's accent colour
    // when there's healthy headroom (>20 % below the budget), turns
    // ORANGE as a warning when headroom falls under 20 %, and RED
    // when headroom is under 10 % (i.e. the frame is on the verge of
    // overruns / reprojection).
    //
    //   ratio = frametime / budget
    //   ratio < 0.80 → Green   (≥ 20 % headroom — fine)
    //   ratio < 0.90 → Orange  (10–20 % headroom — warning)
    //   ratio ≥ 0.90 → Red     ( < 10 % headroom — critical)
    //
    // The same tiering drives the circular utilisation gauge's fill
    // colour in the bottom panels — see drawCircularGauge in the
    // renderer.
    enum class BarTier { Green, Orange, Red };

    struct BarVisual {
        // Fraction of the strip height the bar should take (0..1).
        // The renderer multiplies this by the strip's pixel height to
        // get the actual rectangle to fill.
        float   heightFraction;
        BarTier tier;
    };

    // Anchored normalisation: the Y axis spans `0..1.2 × budgetNs`,
    // so a frame at 100 % of budget fills ~83 % of the strip height
    // (= 1/1.2) — the budget reference line sits at that same 83 %.
    // Bars that exceed 120 % of budget saturate at the strip's top.
    //
    // Why 1.2× and not 2×: the previous 0..2× range reserved the
    // entire UPPER half of the strip for overruns, which meant
    // typical light-load frames (40–70 % of budget) only filled
    // 20–35 % of the strip — leaving the top half empty under normal
    // operation. fpsVR uses 0..2× too, but with much taller strips,
    // so the absolute pixel space below "budget" stays visually
    // satisfying. At our 54-px strip height the trade-off flips —
    // fuller bars under normal load + clipped overruns reads
    // better than half-empty bars + visible overruns.
    //
    // Trade-off: a frame at 130 % budget (= -30 % headroom)
    // saturates visually at the top, indistinguishable from 200 %.
    // The TIER colour still tells the user "this frame is in red
    // (≥ 90 % budget)" — the SHAPE of the bar tells "how often",
    // the COLOUR tells "how bad". Most users care about the
    // first-derivative (am I getting overruns?) more than the
    // exact magnitude.
    //
    // Budget <= 0 (no display-period info yet from the runtime) yields
    // a green zero-height bar — the renderer skips the strip entirely
    // in that case, but the helper stays defensive.
    inline BarVisual barVisualForSample(int64_t sampleNs, int64_t budgetNs) noexcept {
        if (budgetNs <= 0 || sampleNs <= 0) return {0.0f, BarTier::Green};
        const float ratio = static_cast<float>(sampleNs) / static_cast<float>(budgetNs);
        // ratio / 1.2 == ratio * (5/6). Direct multiply chosen for
        // numerical stability (1.2f isn't exactly representable in
        // IEEE-754 but the division is well-conditioned for the
        // [0, 4] input range we'll see in practice).
        const float height = std::clamp(ratio / 1.2f, 0.0f, 1.0f);
        BarTier tier = BarTier::Green;
        if (ratio >= 0.9f) {
            tier = BarTier::Red;
        } else if (ratio >= 0.8f) {
            tier = BarTier::Orange;
        }
        return {height, tier};
    }

    // Reuse the same tiering for the circular utilisation gauge in
    // the bottom panels — keeps the visual language coherent (a bar
    // turning orange next to a gauge turning red reads as "GPU was
    // borderline last second, now it's pinned"). The input here is
    // a 0..1 utilisation FRACTION (not a frametime ratio), but the
    // semantics are the same: 0.80–0.89 = orange, 0.90+ = red.
    inline BarTier gaugeTierForUtilisation(float fraction) noexcept {
        if (!(fraction >= 0.0f)) return BarTier::Green;  // NaN → green default
        if (fraction >= 0.90f) return BarTier::Red;
        if (fraction >= 0.80f) return BarTier::Orange;
        return BarTier::Green;
    }

    // The Y position of the budget reference line, expressed as a
    // fraction of the strip height (0 = top, 1 = bottom). With the
    // 0..1.2× budget scale above, the line sits at 1/6 of the strip
    // (≈ 16.7 % from the top) — that's where the BAR TIP lands when
    // ratio = 1.0 (since heightFraction = 1/1.2 = 5/6, and the bar
    // grows upward from the bottom, so the tip is 5/6 from the
    // bottom = 1/6 from the top).
    inline constexpr float budgetLineFraction() noexcept {
        return 1.0f / 6.0f;
    }

    // Height, in overlay-texture pixels, of the placeholder dash drawn at
    // the strip bottom for an empty (no-sample-yet) histogram slot. 2 px is
    // enough to read as a "this slot exists" marker without crowding the
    // bar grid.
    inline constexpr float kDashPlaceholderH = 2.0f;

} // namespace openxr_api_layer::detail
