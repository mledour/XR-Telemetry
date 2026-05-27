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
// overlay_bars_vs.hlsl — vertex shader for the instanced histogram bars.
// Compiled offline (vs_5_0) by the FxCompile build step into an embedded
// bytecode header; see openxr-api-layer.vcxproj.
// =============================================================================

#include "overlay_bars.hlsli"

VSOutput VSMain(VSInput v)
{
    const float histoT = histoTopLeft.y;
    const float histoB = histoBotRight.y;
    const float stripH = histoB - histoT;

    // Bar pixel rect. Empty slots (tier 3) draw a 2-px dash pinned to
    // the strip bottom regardless of `height`; everything else grows
    // upward from the bottom by heightFraction × stripH.
    const float barH = (v.tier == 3u) ? dashHeight
                                       : saturate(v.height) * stripH;
    const float left  = v.xLeft;
    const float right = v.xLeft + barWidth;
    const float bottom = histoB;
    const float top    = histoB - barH;

    // corner.x ∈ {0,1} → left/right; corner.y ∈ {0,1} → bottom/top.
    const float px = lerp(left,   right, v.corner.x);
    const float py = lerp(bottom, top,   v.corner.y);

    VSOutput o;
    // Pixel → NDC. X: [0,texW] → [-1,1]. Y: [0,texH] → [1,-1] (flip:
    // texture space is y-down, clip space is y-up).
    o.pos = float4(px / texSize.x * 2.0f - 1.0f,
                   1.0f - py / texSize.y * 2.0f,
                   0.0f, 1.0f);
    // Strip-relative Y for the gradient (0 = top of strip, 1 = bottom),
    // interpolated across the quad so the pixel shader samples the
    // gradient per-pixel — matching D2D's strip-spanning linear brush.
    o.stripY = saturate((py - histoT) / max(stripH, 1.0f));
    o.tier = v.tier;
    return o;
}
