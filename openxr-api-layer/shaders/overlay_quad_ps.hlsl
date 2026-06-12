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
// per-instance colour (radius == 0), or an anti-aliased rounded-rectangle
// when radius > 0; the blend state does the alpha compositing either way.
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
    // radius <= 0 → plain rectangle, identical to the old passthrough.
    // Every sharp quad (bars, grid, separators, budget line) takes this
    // branch and is byte-for-byte unchanged; only the rounded panels and
    // outer frame fall through to the SDF path below.
    if (i.radius <= 0.0f)
        return i.color;

    // Clamp the radius to half the shorter side so an over-large value
    // degrades to a stadium/pill instead of an inverted SDF.
    float r   = min(i.radius, min(i.halfsz.x, i.halfsz.y));
    float d   = sdRoundBox(i.local, i.halfsz, r);
    float aa  = max(fwidth(d), 1e-5f);   // ~1-px anti-alias band
    float cov = saturate(0.5f - d / aa); // 1 inside → 0 outside the rounded edge
    float4 c  = i.color;
    c.a *= cov;
    return c;
}
