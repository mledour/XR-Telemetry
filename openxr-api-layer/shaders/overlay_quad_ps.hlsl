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
// overlay_quad_ps.hlsl — pixel shader for the solid-colour quad pass.
// Compiled offline (ps_4_0) by FxCompile. Straight passthrough of the
// per-instance fill colour when radius == 0 and borderWidth == 0, else an
// anti-aliased rounded rectangle with an optional border ring; the blend
// state does the alpha compositing either way.
// =============================================================================

#include "overlay_quad.hlsli"

// Signed distance to a rounded axis-aligned box (Inigo Quilez): negative
// inside, 0 on the edge, positive outside. `p` is relative to the centre,
// `halfSize` is half the width/height, `r` the corner radius — all in px.
float sdRoundBox(float2 p, float2 halfSize, float r)
{
    float2 q = abs(p) - halfSize + r;
    return min(max(q.x, q.y), 0.0f) + length(max(q, 0.0f)) - r;
}

float4 PSMain(QuadVSOutput i) : SV_TARGET
{
    const float radius      = i.rb.x;
    const float borderWidth = i.rb.y;

    // No rounding AND no border → plain rectangle, byte-for-byte the old
    // passthrough. Every sharp quad (bars, grid, separators, budget line)
    // takes this branch unchanged; only rounded / stroked panels and the
    // outer frame fall through to the SDF path below.
    if (radius <= 0.0f && borderWidth <= 0.0f)
        return i.color;

    // Clamp the radius to half the shorter side so an over-large value
    // degrades to a stadium/pill instead of an inverted SDF.
    const float r  = min(radius, min(i.halfsz.x, i.halfsz.y));
    const float d  = sdRoundBox(i.local, i.halfsz, r);
    const float aa = max(fwidth(d), 1e-5f);        // ~1-px anti-alias band

    // Outer coverage: 1 inside the rounded outline, fading to 0 across the
    // ~1px AA band at the edge — this rounds (alpha-clips) the corners.
    const float outerCov = saturate(0.5f - d / aa);

    float4 c = i.color;                             // fill (may be translucent)
    if (borderWidth > 0.0f)
    {
        // Border ring: the outer `borderWidth` px take the border colour,
        // the interior takes the fill. ONE quad — the translucent fill has
        // nothing opaque behind it, so its alpha is preserved (the body
        // stays see-through; the border carries its own alpha).
        const float fillCov = saturate(0.5f - (d + borderWidth) / aa);
        c = lerp(i.borderColor, i.color, fillCov);
    }
    c.a *= outerCov;
    return c;
}
