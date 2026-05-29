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
// overlay_text_vs.hlsl — vertex shader for the sprite-text pass.
// Compiled offline (vs_4_0) by FxCompile. Expands a unit-quad triangle strip
// into the per-instance destination rect AND the matching per-instance atlas
// UV rect — one instance per glyph in the run. Y-flips destination coords
// (texture space y-down → clip space y-up); atlas UVs stay y-down because the
// atlas bitmap is laid out top-down by the builder.
// =============================================================================

#include "overlay_text.hlsli"

TextVSOutput VSMain(TextVSInput v)
{
    // Destination rect → pixel coords of the four corners.
    const float dstLeft   = v.rect.x;
    const float dstTop    = v.rect.y;
    const float dstRight  = v.rect.x + v.rect.z;
    const float dstBottom = v.rect.y + v.rect.w;

    // Atlas rect → pixel coords inside the atlas, then normalised to UV.
    const float uLeft   =  v.uvRect.x                     / atlasSize.x;
    const float uTop    =  v.uvRect.y                     / atlasSize.y;
    const float uRight  = (v.uvRect.x + v.uvRect.z)       / atlasSize.x;
    const float uBottom = (v.uvRect.y + v.uvRect.w)       / atlasSize.y;

    // corner.x ∈ {0,1} → left/right, corner.y ∈ {0,1} → top/bottom.
    const float px = lerp(dstLeft, dstRight,  v.corner.x);
    const float py = lerp(dstTop,  dstBottom, v.corner.y);
    const float u  = lerp(uLeft,   uRight,    v.corner.x);
    const float vv = lerp(uTop,    uBottom,   v.corner.y);

    TextVSOutput o;
    o.pos = float4(px / texSize.x * 2.0f - 1.0f,
                   1.0f - py / texSize.y * 2.0f,
                   0.0f, 1.0f);
    o.uv    = float2(u, vv);
    o.color = v.color;
    return o;
}
