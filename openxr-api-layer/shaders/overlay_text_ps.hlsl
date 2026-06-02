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
// overlay_text_ps.hlsl — pixel shader for the sprite-text pass.
// Compiled offline (ps_4_0) by FxCompile. Samples the R8 glyph atlas as an
// alpha mask, multiplies by the per-instance tint colour, and emits straight-
// alpha RGBA. The blend state (straight alpha-over, set by the renderer) does
// the compositing against whatever is already in the render target — the
// already-painted chrome / panel background / bars.
//
// The atlas is grayscale (mean of CLEARTYPE_3x1 subpixels in the builder), so
// a single .r sample gives the coverage. No gamma correction here: the overlay
// target is BGRA8 and we treat it as linear — perceptually fine for HUD text
// at the sizes we ship (the swapchain image is created BGRA8_UNORM, not
// _SRGB, so the runtime samples our bytes without a gamma decode).
// =============================================================================

#include "overlay_text.hlsli"

float4 PSMain(TextVSOutput i) : SV_TARGET
{
    const float coverage = atlasTexture.Sample(atlasSampler, i.uv);
    return float4(i.color.rgb, i.color.a * coverage);
}
