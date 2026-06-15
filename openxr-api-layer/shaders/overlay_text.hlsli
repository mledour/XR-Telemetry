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
// overlay_text.hlsli — shared declarations for the sprite-text shaders
// (overlay_text_vs.hlsl + overlay_text_ps.hlsl).
//
// Replaces the D2D + DirectWrite per-frame text path: at session init the
// CPU-side glyph atlas builder (utils/glyph_atlas.{h,cpp}) pre-rasterizes the
// overlay's working set into a single R8_UNORM texture; at draw time the
// renderer issues one instanced draw per text run, each instance being a
// glyph quad (one corner of the unit quad per vertex, the destination pixel
// rect + atlas UV rect + RGBA tint as per-instance data).
//
// The atlas is single-channel grayscale (mean of the three CLEARTYPE_3x1
// subpixels DirectWrite produces). The PS uses the sampled value as an
// alpha mask and multiplies by the per-instance tint colour — same straight-
// alpha blend state as the quad pass, so chrome + bars + text all composite
// through one pipeline state.
//
// Shader model 4.0 (feature level 10_0) — matches the rest of the overlay
// shaders. No instancing features beyond input-layout per-instance data;
// every recent OpenXR-capable host supports this.
// =============================================================================

cbuffer TextConstants : register(b0)
{
    float2 texSize;     // (kTexW, kTexH) for pixel → NDC on the destination
    float2 atlasSize;   // (atlasW, atlasH) for atlas-pixel → atlas-UV
    // Overlay supersample factor (= renderer m_ss). The VS ignores it; the PS
    // gates its edge-contrast + gamma corrections on supersample > 1 so the
    // 1x (snapshot/golden) path stays byte-identical to the legacy shader.
    float  supersample; // reg1.x
    float3 _pad;        // reg1.yzw
};

Texture2D<float> atlasTexture : register(t0);   // R8_UNORM glyph atlas
SamplerState     atlasSampler : register(s0);   // POINT clamp, set by renderer

struct TextVSInput
{
    float2 corner  : POSITION;     // unit-quad corner (0,0)-(1,1), per-vertex

    // Per-instance:
    float4 rect    : TEXT_RECT;    // destination (x, y, width, height) in pixels
    float4 uvRect  : TEXT_UV_RECT; // atlas (x, y, width, height) in atlas pixels
    float4 color   : TEXT_COLOR;   // straight-alpha RGBA tint
};

struct TextVSOutput
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};
