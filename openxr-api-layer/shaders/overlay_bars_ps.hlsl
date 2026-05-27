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
// overlay_bars_ps.hlsl — pixel shader for the instanced histogram bars.
// Compiled offline (ps_5_0) by the FxCompile build step. Picks the bar's
// fill from its tier: a vertical gradient for healthy bars (sampled per-
// pixel via stripY so a short bar shows only the gradient's lower span,
// matching the D2D strip-spanning linear brush), solid orange/red for the
// warning/critical tiers, and the grid-dash colour for empty placeholders.
// =============================================================================

#include "overlay_bars.hlsli"

float4 PSMain(VSOutput i) : SV_TARGET
{
    float4 col;
    switch (i.tier)
    {
        case 0u:  // Green → vertical gradient (top → bottom of strip)
            col = lerp(gradTop, gradBottom, i.stripY);
            break;
        case 1u:  // Orange
            col = orangeColor;
            break;
        case 2u:  // Red
            col = redColor;
            break;
        default:  // 3u Empty → dash placeholder
            col = dashColor;
            break;
    }

    // Analytic 1-px box-filter coverage on the LEFT/RIGHT edges only.
    // i.pos.x is the pixel centre (SV_Position); rectPx.x/.z are the
    // bar's left/right in pixels. A pixel fully inside gets coverage 1,
    // an edge pixel its fractional overlap — softening the vertical
    // edges so fractional bar widths/positions read as uniform width.
    //
    // The top/bottom are deliberately left CRISP (no covY). Anti-
    // aliasing them too multiplied the corner pixels by both factors
    // (covX·covY ≈ 0.25), dimming the top corners so short bars looked
    // "thinner at the top". A flat pixel-snapped top reads cleaner for
    // a bar chart and removes that artifact; the shared baseline at the
    // strip floor stays crisp for the same reason.
    const float covX = saturate(min(i.pos.x - i.rectPx.x,
                                     i.rectPx.z - i.pos.x) + 0.5f);
    col.a *= covX;
    return col;
}
