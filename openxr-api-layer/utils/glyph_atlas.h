// MIT License
//
// Copyright(c) 2025 Michael Ledour
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <dwrite.h>
#include <wrl/client.h>

namespace openxr_api_layer::utils::glyph_atlas {

    // ===================================================================
    // Glyph atlas — CPU-side rasterizer + packer.
    //
    // Purpose: replace per-frame D2D + DirectWrite text drawing with a
    // one-shot DirectWrite rasterization at init, packed into a single
    // R8_UNORM bitmap. At runtime the renderer samples that texture
    // through a textured-quad shader; no DirectWrite or D2D call happens
    // on the hot path.
    //
    // Scope:
    //   * Pure CPU work: takes IDWriteFactory + IDWriteFontCollection,
    //     produces a BuildResult containing the bitmap + glyph table.
    //   * Caller wraps the BuildResult bitmap into an ID3D11Texture2D
    //     (R8_UNORM) + SRV inside the renderer that owns it. The atlas
    //     module never touches D3D — that keeps it testable without a
    //     device and avoids leaking graphics-API choice into the API.
    //
    // Faces:
    //   Only two cuts are baked. Match the format split in
    //   CoreRenderer::init() (overlay_renderer.cpp): Quantico Italic
    //   for the chiffres, Rajdhani SemiBold upright for labels / titles /
    //   units. Adding a face means rasterizing a new working set at every
    //   used size — keep this small.
    //
    // Working set:
    //   * QuanticoItalic: digits 0-9 plus '.' — see findValueRuns() in
    //     overlay_renderer.cpp. The leading '-' stays upright Rajdhani.
    //   * RajdhaniUpright: ASCII 0x20..0x7E plus the small Unicode set
    //     used in unit suffixes (°, µ, ×). Conservatively wide — keeps
    //     us from re-baking every time a new string sneaks into the
    //     overlay.
    //
    // Sizes:
    //   The atlas bakes the union of font sizes used by the overlay
    //   formats. Today that's six unique pixel sizes (17, 18, 19, 32, 43,
    //   52) — see the kFont* constants in CoreRenderer::init(). The
    //   builder accepts the size list at runtime so layout tweaks don't
    //   require regenerating the atlas module.
    //
    // Failure mode:
    //   build() returns false on missing resources (font face not in the
    //   collection, glyph index 0). The caller must degrade to bypass —
    //   layers do not crash the host process for a cosmetic miss.
    // ===================================================================

    // -------- Face identification --------------------------------------
    //
    // 4 bits in the packed GlyphKey, so this enum can grow to 16 faces if
    // we ever ship a third cut (heavier weight for an "alert" tier, e.g.).
    enum class GlyphFace : uint8_t {
        QuanticoItalic    = 0,   // digits + '.' on the value chiffres
        RajdhaniUpright = 1,   // everything else (labels, titles, units)
    };

    // -------- Per-glyph table entry ------------------------------------
    //
    // Coordinates in the atlas are pixel-space (u, v, w, h), normalised
    // to UV at sample time inside the shader. Pixel-space stays a power
    // of one — easier to debug in a viewer and avoids floating-point
    // drift in the packer.
    //
    // bearingX/Y and advanceX are pixel-space metrics from the glyph
    // run analysis. We follow the standard text-rendering convention:
    //
    //     pen advances by advanceX horizontally;
    //     glyph quad is positioned at (penX + bearingX, baselineY -
    //     bearingY) with size (w, h).
    //
    // bearingY is the distance from baseline to the top of the glyph
    // bitmap, positive upward — i.e. the typographic ascent of this
    // specific glyph at this specific size.
    struct GlyphInfo {
        uint16_t u;          // atlas pixel x of glyph top-left
        uint16_t v;          // atlas pixel y of glyph top-left
        uint16_t w;          // glyph bitmap width in pixels
        uint16_t h;          // glyph bitmap height in pixels
        int16_t  bearingX;   // horizontal offset from pen
        int16_t  bearingY;   // vertical offset from baseline (positive = up)
        float    advanceX;   // pen advance after rendering this glyph
    };

    // -------- Per-(face, size) baseline metrics ------------------------
    //
    // Needed at layout time to position whole runs vertically (we don't
    // store glyph baselines individually because the run baseline is
    // shared). All values in pixels at the requested em size.
    struct FaceMetrics {
        float ascent;        // distance from baseline up to typographic top
        float descent;       // distance from baseline down to typographic bottom (positive)
        float lineGap;       // recommended gap between baselines
        float capHeight;     // height of capital letters (used by text alignment)
    };

    // -------- Packed glyph key -----------------------------------------
    //
    // Layout (high → low bits):
    //   [31..28] face (4 bits, 16 values)
    //   [27..16] sizePx (12 bits, up to 4095 px)
    //   [15..0 ] codepoint (16 bits, BMP only — we don't bake astral)
    //
    // Packed uint32_t lets us use a plain std::unordered_map without a
    // custom hasher. Lookups are on the hot path (every character of
    // every value drawn), so keeping the hash cheap matters.
    using GlyphKey = uint32_t;

    constexpr GlyphKey makeGlyphKey(GlyphFace face, uint16_t sizePx, uint16_t codepoint) {
        return (static_cast<GlyphKey>(face)     << 28)
             | (static_cast<GlyphKey>(sizePx)   << 16)
             |  static_cast<GlyphKey>(codepoint);
    }

    // -------- Face-metrics key -----------------------------------------
    //
    // Same packing as GlyphKey but without the codepoint field — the
    // metrics are shared across all glyphs of a given (face, sizePx).
    using FaceMetricsKey = uint32_t;

    constexpr FaceMetricsKey makeFaceMetricsKey(GlyphFace face, uint16_t sizePx) {
        return (static_cast<FaceMetricsKey>(face)   << 16)
             |  static_cast<FaceMetricsKey>(sizePx);
    }

    // -------- Build request --------------------------------------------
    //
    // One entry per (face, sizePx, charset) tuple to bake. The builder
    // unions overlapping requests internally — pass the working set as
    // it is, the result table is keyed on (face, sizePx, codepoint) so
    // duplicates collapse.
    //
    // `charset` is UTF-32 to keep the API independent of source encoding;
    // the bundled fonts cover BMP characters only and astral code points
    // would never resolve to a real glyph anyway.
    struct BuildRequest {
        GlyphFace            face;
        uint16_t             sizePx;
        std::vector<wchar_t> charset;   // UTF-16; surrogates not supported
    };

    // -------- Build inputs ---------------------------------------------
    //
    // The builder needs the same DirectWrite factory + custom font
    // collection that the existing CoreRenderer builds at init. Pass
    // them in — we don't want this module to own font loading or to
    // duplicate the in-memory loader plumbing.
    //
    // `familyChiffres` / `familyLabels` decouple us from the actual
    // family names: today they're L"Quantico" / L"Rajdhani"; on fallback
    // (bundled load failed) both flip to L"Bahnschrift" and the atlas
    // still builds, just with synthesised oblique on the italic face.
    struct BuildSpec {
        Microsoft::WRL::ComPtr<IDWriteFactory>         dwriteFactory;
        Microsoft::WRL::ComPtr<IDWriteFontCollection>  fontCollection;   // may be null → use system
        const wchar_t*                                  familyChiffres = L"Quantico";
        const wchar_t*                                  familyLabels   = L"Rajdhani";

        // Atlas dimensions. Square power-of-two. 1024 fits today's
        // working set with comfortable slack (~10-15% of area used).
        uint16_t atlasWidthPx  = 1024;
        uint16_t atlasHeightPx = 1024;

        // 1-pixel border around each glyph to keep bilinear sampling
        // from bleeding neighbour glyphs into the quad. The shader uses
        // POINT sampling today but the border is cheap and survives a
        // future switch to LINEAR for free.
        uint8_t padding = 1;

        std::vector<BuildRequest> requests;
    };

    // -------- Build result ---------------------------------------------
    //
    // Returned on success. The caller wraps `bitmap` into a 2D texture
    // (R8_UNORM, atlasWidth × atlasHeight, single mip, no MSAA) with
    // initial data pointing into the vector. After upload the vector
    // can be dropped — the atlas table is what the renderer holds onto.
    struct BuildResult {
        uint16_t                                       atlasWidth  = 0;
        uint16_t                                       atlasHeight = 0;
        std::vector<uint8_t>                           bitmap;       // R8, row-major, no padding between rows
        std::unordered_map<GlyphKey, GlyphInfo>        glyphs;
        std::unordered_map<FaceMetricsKey, FaceMetrics> faceMetrics;

        // Number of glyphs that failed to resolve (missing in font face)
        // or failed to rasterize. Logged by the caller; a non-zero value
        // does not abort the build — we want to ship the atlas with
        // whatever did resolve and let the renderer fall back to a
        // placeholder for the rest.
        uint32_t missingGlyphs = 0;
    };

    // -------- Builder entry point --------------------------------------
    //
    // Returns true on success (atlas packed, table populated), false on
    // hard failure (atlas too small, DirectWrite QI fails, no font face
    // resolvable). On hard failure `out` is left in an unspecified state
    // — the caller should discard it and degrade to bypass.
    //
    // Soft failures (individual glyphs missing) are reported through
    // `out.missingGlyphs` and do not return false. The renderer can
    // detect missing glyphs at draw time by checking `glyphs.find()`.
    //
    // Cost: ~30-50 µs per glyph for DirectWrite rasterization at typical
    // sizes — full atlas build is in the 20-50 ms range. Runs once at
    // session init; not on the frame path.
    bool build(const BuildSpec& spec, BuildResult& out);

}   // namespace openxr_api_layer::utils::glyph_atlas
