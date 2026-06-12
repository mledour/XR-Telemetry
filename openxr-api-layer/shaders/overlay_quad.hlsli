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
// overlay_quad.hlsli — shared declarations for the solid-colour quad shaders
// (overlay_quad_vs.hlsl + overlay_quad_ps.hlsl).
//
// Generic instanced axis-aligned rectangles in straight-alpha colour, used
// to paint the parts of the histogram region that AREN'T the bars: the
// opaque panel-background fill, the 4 dashed-style grid lines (drawn as thin
// translucent rects), and the budget reference line — one DrawInstanced per
// group. Colours carry their own alpha; the blend state is straight alpha-
// over, and every rect lands on the already-opaque panel background so the
// composited result stays opaque (premultiplied == straight on the BGRA8
// target).
// =============================================================================

cbuffer QuadConstants : register(b0)
{
    float2 texSize;   // (kTexW, kTexH) for pixel → NDC
    float2 _pad;      // 16-byte alignment
};

struct QuadVSInput
{
    float2 corner : POSITION;   // unit-quad corner (0,0)-(1,1), per-vertex

    // Per-instance:
    float4 rect   : QUAD_RECT;   // (x, y, width, height) in pixels (top-left origin)
    float4 color  : QUAD_COLOR;  // straight-alpha RGBA
    float  radius : QUAD_RADIUS; // corner radius in pixels; 0 = sharp (passthrough)
};

struct QuadVSOutput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
    // Rounded-rect SDF inputs — only consulted by the PS when radius > 0.
    float2 local                  : TEXCOORD0;  // pixel offset from the rect centre
    nointerpolation float2 halfsz : TEXCOORD1;  // rect half-size in pixels
    nointerpolation float  radius : TEXCOORD2;  // corner radius in pixels
};
