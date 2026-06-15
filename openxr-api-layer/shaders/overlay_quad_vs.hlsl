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
// overlay_quad_vs.hlsl — vertex shader for the solid-colour quad pass.
// Compiled offline (vs_4_0) by FxCompile. Expands a unit-quad triangle strip
// into the per-instance pixel rect and converts to NDC (Y-flip: texture space
// is y-down, clip space y-up).
// =============================================================================

#include "overlay_quad.hlsli"

QuadVSOutput VSMain(QuadVSInput v)
{
    const float left   = v.rect.x;
    const float top    = v.rect.y;
    const float right  = v.rect.x + v.rect.z;
    const float bottom = v.rect.y + v.rect.w;

    // corner.x ∈ {0,1} → left/right; corner.y ∈ {0,1} → top/bottom.
    const float px = lerp(left, right,  v.corner.x);
    const float py = lerp(top,  bottom, v.corner.y);

    QuadVSOutput o;
    o.pos = float4(px / texSize.x * 2.0f - 1.0f,
                   1.0f - py / texSize.y * 2.0f,
                   0.0f, 1.0f);
    o.color       = v.color;
    o.borderColor = v.borderColor;
    // Rounded-rect SDF inputs (texture pixels). `local` is this vertex's
    // offset from the rect centre and interpolates per-pixel; halfsz and
    // rb (corner radius, border width) are per-instance constants the PS
    // reads to evaluate the signed distance and the border ring. `dash`
    // (period, on-length px) is the per-instance dash pattern the PS uses to
    // break the grid + left-axis lines into dashes; 0 = solid (every other
    // quad).
    o.halfsz = v.rect.zw * 0.5f;
    o.local  = float2(px, py) - (v.rect.xy + o.halfsz);
    o.rb     = v.params.xy;
    o.dash   = v.params.zw;
    return o;
}
