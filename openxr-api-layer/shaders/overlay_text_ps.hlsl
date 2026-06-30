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
// a single .r sample gives the coverage.
//
// Gamma-correct coverage blending
// --------------------------------
// The overlay target is BGRA8_UNORM (not _SRGB) and the renderer's straight
// alpha-over blend is computed by the fixed-function blender directly on those
// stored bytes — i.e. in LINEAR light, since the runtime samples a _UNORM quad
// without a gamma decode. DirectWrite's ClearType coverage, however, is tuned
// to be blended in a ~gamma-2.2 (perceptual) space. Blending that gamma-tuned
// coverage linearly makes light-on-dark stems composite too THIN / washed —
// the classic gamma-incorrect text artifact, and part of why the HUD text
// reads soft.
//
// Re-map the coverage through a gamma curve so the blended edge lands at the
// perceptually-correct weight. With TEXT_GAMMA > 1, pow(c, 1/TEXT_GAMMA) lifts
// the mid-coverage edge pixels back toward where a perceptual blend would put
// them (fatter, better-defined stems). 2.2 matches the sRGB gamma the coverage
// was tuned for; it is the single tuning knob:
//   * lower toward 1.0  → lighter stems (1.0 == the old no-correction path)
//   * raise             → heavier stems
// DIRECTION CAVEAT: the correction direction depends on whether the runtime
// composites the quad in linear space (UNORM swapchain — Pimax/WMR/Oculus) or
// sRGB-decodes it (sRGB swapchain — SteamVR). The renderer passes which case
// applies via the `srgbComposite` cbuffer flag: linear → pow(c, 1/TEXT_GAMMA)
// (lifts stems); sRGB → pow(c, TEXT_GAMMA) (the inverse). Both are still worth
// one on-headset A/B pass to confirm weight + direction; purely cosmetic and
// trivially tuned via TEXT_GAMMA (or by forcing the branch).
// =============================================================================

#include "overlay_text.hlsli"

#define TEXT_GAMMA 2.2f

// Edge-contrast boost
// -------------------
// A Quad composition layer is resampled by the runtime with a bilinear tap
// through the lens-distortion / reprojection mesh, on a texel grid that never
// aligns to the HMD pixel grid — so every glyph edge is averaged with its
// neighbour and reads ~1px soft no matter the source resolution. Supersampling
// (kOverlaySupersample) gives that resample more texels to work with; this
// steepens the coverage ramp around the geometric edge (coverage == 0.5) so
// the edge stays crisper THROUGH that resample.
//
// Linear contrast about the 0.5 midpoint: EDGE_SHARPEN == 1.0 is the identity
// (off); > 1 narrows the anti-aliased ramp (crisper, but too high re-aliases —
// the supersample headroom is what keeps a moderate value clean). Symmetric
// about 0.5, so it sharpens without shifting stem weight; the gamma step below
// owns weight. Applied BEFORE the gamma so the weight correction acts on the
// sharpened edge.
//
// A resolution-independent variant (fwidth-based, ~1px edge regardless of
// magnification) would be the SDF-style upgrade if a fixed value proves too
// soft when the HUD is scaled up; a fixed contrast is more predictable on a
// bitmap-coverage atlas. Tune on-headset; purely cosmetic.
#define EDGE_SHARPEN 1.5f

float4 PSMain(TextVSOutput i) : SV_TARGET
{
    float coverage = atlasTexture.Sample(atlasSampler, i.uv);

    // The edge-contrast + gamma corrections run ONLY on the supersampled
    // (production) path. At supersample == 1 — the snapshot/golden test path —
    // this shader stays byte-identical to the pre-sharpening version, so the
    // 1x golden is untouched by this feature AND by any later tuning of the
    // constants above. Keeps the text pass on the overlay-wide invariant
    // "supersample == 1 reproduces the legacy pixels".
    if (supersample > 1.0f)
    {
        // Edge contrast first: steepen the coverage ramp about 0.5 so glyph
        // edges survive the compositor's bilinear resample crisper.
        coverage = saturate((coverage - 0.5f) * EDGE_SHARPEN + 0.5f);
        // Then gamma-correct the coverage. The exponent direction depends on
        // how the runtime composites the quad (see DIRECTION CAVEAT): linear
        // (UNORM swapchain) lifts with 1/TEXT_GAMMA; sRGB (sRGB swapchain, the
        // runtime sRGB-decodes the quad) needs the inverse, TEXT_GAMMA.
        // Guard pow(0): pow(0, x) is spec-undefined in SM4.0 (a driver
        // returning NaN would poison the blend) and 0 coverage must stay fully
        // transparent, so branch it rather than lifting it with an epsilon.
        float gammaExp = (srgbComposite > 0.5f) ? TEXT_GAMMA
                                                : (1.0f / TEXT_GAMMA);
        coverage = (coverage > 0.0f) ? pow(coverage, gammaExp)
                                     : 0.0f;
    }

    return float4(i.color.rgb, i.color.a * coverage);
}
