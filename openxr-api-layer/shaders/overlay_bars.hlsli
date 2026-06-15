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
// overlay_bars.hlsli — shared declarations for the instanced histogram-bar
// shaders (overlay_bars_vs.hlsl + overlay_bars_ps.hlsl).
//
// Replaces the per-bar D2D FillRectangle loop (120 bars × 2 panels × host
// frame rate) with a single instanced draw. The CPU writes one BarInstance
// per ring sample into a dynamic vertex buffer; the GPU expands each into a
// quad and colours it (vertical gradient for healthy bars, solid orange/red
// for the warning/critical tiers, a 2-px dash for empty slots) — reproducing
// drawHistogramBars()'s look exactly, see overlay_layout.h::barVisualForSample.
//
// Tier encoding (BarInstance.tier), matching detail::BarTier:
//   0 = Green   → vertical gradient (gradTop → gradBottom)
//   1 = Orange  → solid orangeColor
//   2 = Red     → solid redColor
//   3 = Empty   → 2-px dashColor placeholder at the strip bottom
// =============================================================================

// Per-panel constants. All positions are in overlay-texture pixels; the vertex
// shader converts to NDC using texSize. Colours are straight-alpha RGBA (the
// bars are opaque, so straight == premultiplied on the BGRA8 target; only the
// dash carries alpha, and it lands on the already-opaque panel background).
// Packing note: members are ordered so each maps cleanly onto HLSL's
// 16-byte constant registers with no implicit padding — the two float2
// pairs fill reg0, (histoBotRight, barWidth, dashHeight) fills reg1, and
// each float4 colour gets its own register from offset 32 on. The C++
// BarConstants mirror struct must match byte-for-byte (see
// HistogramBarRenderer); do NOT insert an explicit pad float2 here — that
// would bump gradTop off its natural 16-byte boundary.
cbuffer BarConstants : register(b0)
{
    float2 texSize;        // (kTexW, kTexH)            reg0.xy
    float2 histoTopLeft;   // (histoL, histoT)          reg0.zw
    float2 histoBotRight;  // (histoR, histoB)          reg1.xy
    float  barWidth;       // barW in pixels            reg1.z
    float  dashHeight;     // kDashPlaceholderH (2.0)    reg1.w
    float4 gradTop;        // healthy gradient top      reg2
    float4 gradBottom;     // healthy gradient bottom   reg3
    float4 orangeColor;    // BarTier::Orange           reg4
    float4 redColor;       // BarTier::Red              reg5
    float4 dashColor;      // empty-slot placeholder    reg6
    // Overlay supersample factor (= renderer m_ss). The VS scales rectPx by
    // it so the PS's analytic edge coverage — measured against SV_Position,
    // which is in PHYSICAL render-target pixels under the supersampled
    // viewport — compares like-for-like. Appended at reg7 so the colour
    // registers above keep their natural 16-byte boundaries. ss==1 → no-op.
    float  supersample;    // overlay supersample factor reg7.x
    float3 _pad;           //                            reg7.yzw
};

// Per-vertex: the four corners of a unit quad (0,0)-(1,1), fed as a
// triangle strip. cx/cy ∈ {0,1}; cx selects left/right, cy selects
// bottom(0)/top(1) of the bar.
struct VSInput
{
    float2 corner : POSITION;     // unit-quad corner, per-vertex (slot 0)

    // Per-instance (slot 1): one entry per ring sample.
    float  xLeft  : BAR_XLEFT;    // bar left edge, pixels
    float  height : BAR_HEIGHT;   // heightFraction 0..1 (strip-relative)
    uint   tier   : BAR_TIER;     // 0..3, see header
};

struct VSOutput
{
    float4 pos    : SV_POSITION;
    float  stripY : STRIPY;       // 0 = strip top, 1 = strip bottom (for gradient)
    nointerpolation uint   tier  : BAR_TIER;
    // The bar's pixel-space rect (left, top, right, bottom), flat across
    // the quad. The pixel shader uses it with SV_Position to compute
    // analytic edge coverage — soft 1-px box-filter edges, the same look
    // D2D's anti-aliased FillRectangle gave. Without it, fractional bar
    // widths / positions snap hard to the pixel grid and the bars (or the
    // gaps between them) read as uneven.
    nointerpolation float4 rectPx : RECTPX;
};
