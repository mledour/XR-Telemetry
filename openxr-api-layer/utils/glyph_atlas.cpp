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

#include "pch.h"

#include "glyph_atlas.h"

#include <algorithm>
#include <array>

namespace openxr_api_layer::utils::glyph_atlas {

    namespace {

        using Microsoft::WRL::ComPtr;

        // -------- Resolved face: face + size + scale -------------------
        //
        // Cached per (faceId, sizePx) so we resolve the family / get the
        // font face / read design metrics only once even when several
        // BuildRequests share a face. The scale factor folds the
        // design-units-per-em into a single multiplier — apply it to any
        // design-unit metric to get pixels.
        struct ResolvedFace {
            ComPtr<IDWriteFontFace> face;
            float                   scale = 0.0f;     // sizePx / designUnitsPerEm
            float                   ascent = 0.0f;    // pixels
            float                   descent = 0.0f;   // pixels (positive)
            float                   lineGap = 0.0f;   // pixels
            float                   capHeight = 0.0f; // pixels
        };

        // -------- Family-name resolution -------------------------------
        //
        // Walks the custom collection first (Rajdhani / Rajdhani live
        // there) and falls back to the system collection. Returns null
        // ComPtr on miss — caller logs and continues.
        //
        // Lookup strategy: exact match first, then case-sensitive
        // prefix match. The prefix fallback handles the case where
        // DirectWrite enumerates a bundled font under its WWS / legacy
        // family name (e.g. "Rajdhani SemiBold") rather than the
        // typographic family ("Rajdhani") declared by the TTF's `name`
        // table. The D2D CreateTextFormat path is more forgiving and
        // tolerates the mismatch silently, but FindFamilyName is exact;
        // without the prefix fallback the atlas build silently fails
        // for any weight/style-suffixed bundled font.
        ComPtr<IDWriteFontFamily> resolveFamilyIn(
            IDWriteFontCollection* coll,
            const wchar_t*         familyName)
        {
            if (!coll || !familyName) return {};
            // Exact match.
            {
                UINT32 idx = 0;
                BOOL   exists = FALSE;
                if (SUCCEEDED(coll->FindFamilyName(familyName, &idx, &exists)) && exists) {
                    ComPtr<IDWriteFontFamily> family;
                    if (SUCCEEDED(coll->GetFontFamily(idx, family.GetAddressOf()))) {
                        return family;
                    }
                }
            }
            // Prefix match. Walks the family list and picks the first
            // entry whose locale-0 name starts with `familyName` + ' '.
            // The trailing space avoids spurious matches (e.g. "Bar"
            // accidentally matching "Rajdhani SemiBold"). Cheap — both the
            // custom collection and our requested names stay tiny.
            const std::size_t reqLen = std::wcslen(familyName);
            const UINT32 famCount = coll->GetFontFamilyCount();
            for (UINT32 fi = 0; fi < famCount; ++fi) {
                ComPtr<IDWriteFontFamily> fam;
                if (FAILED(coll->GetFontFamily(fi, fam.GetAddressOf()))) continue;
                ComPtr<IDWriteLocalizedStrings> names;
                if (FAILED(fam->GetFamilyNames(names.GetAddressOf()))) continue;
                UINT32 nameLen = 0;
                if (FAILED(names->GetStringLength(0, &nameLen))) continue;
                if (nameLen < reqLen + 1) continue;   // need at least " <suffix>"
                std::wstring wname(nameLen + 1, L'\0');
                if (FAILED(names->GetString(0, wname.data(), nameLen + 1))) continue;
                wname.resize(nameLen);
                if (std::wcsncmp(wname.c_str(), familyName, reqLen) == 0 &&
                    wname[reqLen] == L' ') {
                    return fam;
                }
            }
            return {};
        }

        ComPtr<IDWriteFontFamily> resolveFamily(
            IDWriteFactory*          factory,
            IDWriteFontCollection*   customCollection,
            const wchar_t*           familyName)
        {
            // Try the custom collection first (exact + prefix).
            if (auto fam = resolveFamilyIn(customCollection, familyName)) {
                return fam;
            }
            // Fallback to system fonts. Bahnschrift is the documented
            // fallback when bundled-font loading fails (see
            // CoreRenderer::init() in overlay_renderer.cpp).
            ComPtr<IDWriteFontCollection> systemColl;
            if (FAILED(factory->GetSystemFontCollection(systemColl.GetAddressOf(), FALSE))) {
                return {};
            }
            return resolveFamilyIn(systemColl.Get(), familyName);
        }

        // -------- Build a ResolvedFace -------------------------------------
        //
        // Picks the closest font in the family matching the (weight,
        // style) the format uses, then opens a font face and reads
        // design metrics. Both faces resolve to Rajdhani SemiBold upright
        // — the value chiffres and the labels share one font:
        //   Chiffres        → weight SEMI_BOLD, style NORMAL
        //   Rajdhani Upright → weight SEMI_BOLD, style NORMAL
        bool resolveFace(
            IDWriteFactory*          factory,
            IDWriteFontCollection*   customCollection,
            const wchar_t*           familyName,
            DWRITE_FONT_WEIGHT       weight,
            DWRITE_FONT_STYLE        style,
            float                    sizePx,
            ResolvedFace&            out)
        {
            ComPtr<IDWriteFontFamily> family =
                resolveFamily(factory, customCollection, familyName);
            if (!family) return false;

            ComPtr<IDWriteFont> font;
            if (FAILED(family->GetFirstMatchingFont(
                    weight, DWRITE_FONT_STRETCH_NORMAL, style,
                    font.GetAddressOf()))) {
                return false;
            }

            if (FAILED(font->CreateFontFace(out.face.GetAddressOf()))) {
                return false;
            }

            DWRITE_FONT_METRICS m{};
            out.face->GetMetrics(&m);
            if (m.designUnitsPerEm == 0) return false;

            out.scale     = sizePx / static_cast<float>(m.designUnitsPerEm);
            out.ascent    = static_cast<float>(m.ascent)    * out.scale;
            out.descent   = static_cast<float>(m.descent)   * out.scale;
            out.lineGap   = static_cast<float>(m.lineGap)   * out.scale;
            out.capHeight = static_cast<float>(m.capHeight) * out.scale;
            return true;
        }

        // -------- Pick (weight, style) for a face ----------------------
        //
        // Mirrors the format split in CoreRenderer::init() — keep these
        // in lockstep with the kChiffresWeight / kLabelWeight constants
        // over there.
        struct FaceStyle {
            DWRITE_FONT_WEIGHT weight;
            DWRITE_FONT_STYLE  style;
        };
        FaceStyle styleFor(GlyphFace face) {
            switch (face) {
            case GlyphFace::Chiffres:
                return { DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL };
            case GlyphFace::RajdhaniUpright:
            default:
                return { DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL };
            }
        }

        // -------- Pick the right family for a face --------------------
        const wchar_t* familyFor(GlyphFace face, const BuildSpec& spec) {
            switch (face) {
            case GlyphFace::Chiffres: return spec.familyChiffres;
            case GlyphFace::RajdhaniUpright:
            default:                          return spec.familyLabels;
            }
        }

        // -------- Single-glyph rasterization ---------------------------
        //
        // Rasterizes one glyph to a CPU buffer in grayscale. Strategy:
        //
        //   1. Build a glyph run containing exactly this glyph at the
        //      origin (0, 0).
        //   2. Call CreateGlyphRunAnalysis (CLEARTYPE_3x1, NATURAL).
        //   3. GetAlphaTextureBounds gives us the pixel rect the glyph
        //      will occupy when rendered.
        //   4. Allocate (3 × width × height) bytes for the CLEARTYPE_3x1
        //      RGB subpixel buffer.
        //   5. CreateAlphaTexture fills the buffer.
        //   6. Collapse RGB → single grayscale byte per pixel (mean of
        //      the three subpixels). DirectWrite returns subpixels in
        //      red-green-blue order; the human eye perceives a 3-tap
        //      box filter as smooth grayscale.
        //
        // The grayscale collapse is the standard pattern when sampling
        // ClearType output as a single-channel atlas — see e.g. Chrome's
        // Skia DirectWrite glyph cache and the dwrite_text_renderer
        // reference samples in the Windows SDK.
        struct GlyphRaster {
            std::vector<uint8_t> pixels;     // R8 grayscale, row-major
            int32_t              widthPx  = 0;
            int32_t              heightPx = 0;
            int32_t              originX  = 0;   // bbox left, pixel-space
            int32_t              originY  = 0;   // bbox top,  pixel-space
            float                advanceX = 0.0f;
        };

        bool rasterizeGlyph(
            IDWriteFactory*  factory,
            ResolvedFace&    face,
            wchar_t          codepoint,
            float            sizePx,
            GlyphRaster&     out)
        {
            // Map codepoint → glyph index. Index 0 means "missing glyph"
            // in DirectWrite — treat as soft failure (out.missingGlyphs).
            const UINT32 cp = static_cast<UINT32>(codepoint);
            UINT16 glyphIndex = 0;
            if (FAILED(face.face->GetGlyphIndices(&cp, 1, &glyphIndex))
                || glyphIndex == 0) {
                return false;
            }

            // Advance comes from design glyph metrics, scaled to px.
            DWRITE_GLYPH_METRICS gm{};
            if (FAILED(face.face->GetDesignGlyphMetrics(&glyphIndex, 1, &gm, FALSE))) {
                return false;
            }
            out.advanceX = static_cast<float>(gm.advanceWidth) * face.scale;

            // Build a single-glyph run at the origin. Anchor on the
            // baseline (0, 0) so getAlphaTextureBounds returns pixel
            // coords relative to that baseline.
            FLOAT glyphAdvance = out.advanceX;
            DWRITE_GLYPH_OFFSET glyphOffset{0.0f, 0.0f};
            DWRITE_GLYPH_RUN run{};
            run.fontFace      = face.face.Get();
            run.fontEmSize    = sizePx;
            run.glyphCount    = 1;
            run.glyphIndices  = &glyphIndex;
            run.glyphAdvances = &glyphAdvance;
            run.glyphOffsets  = &glyphOffset;
            run.isSideways    = FALSE;
            run.bidiLevel     = 0;

            // CLEARTYPE_3x1 + NATURAL → 3-byte-per-pixel subpixel buffer.
            // Pixels per DIP = 1.0 (we work in pixel-space; the overlay
            // layout is already pixel-space). Transform is null = identity.
            ComPtr<IDWriteGlyphRunAnalysis> analysis;
            if (FAILED(factory->CreateGlyphRunAnalysis(
                    &run,
                    1.0f /*pixelsPerDip*/,
                    nullptr /*transform*/,
                    DWRITE_RENDERING_MODE_NATURAL,
                    DWRITE_MEASURING_MODE_NATURAL,
                    0.0f /*baselineOriginX*/,
                    0.0f /*baselineOriginY*/,
                    analysis.GetAddressOf()))) {
                return false;
            }

            RECT bounds{};
            if (FAILED(analysis->GetAlphaTextureBounds(
                    DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds))) {
                return false;
            }
            const int32_t w = bounds.right  - bounds.left;
            const int32_t h = bounds.bottom - bounds.top;
            if (w <= 0 || h <= 0) {
                // Zero-area glyph (space, etc.). Still valid — record
                // the advance, leave the bitmap empty so the packer
                // skips it. Caller stores a GlyphInfo with w=h=0 so the
                // renderer can still advance the pen without sampling
                // the atlas.
                out.widthPx  = 0;
                out.heightPx = 0;
                out.originX  = bounds.left;
                out.originY  = bounds.top;
                out.pixels.clear();
                return true;
            }

            // CLEARTYPE_3x1: 3 bytes per pixel, row-major, no padding.
            std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
            if (FAILED(analysis->CreateAlphaTexture(
                    DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds,
                    rgb.data(), static_cast<UINT32>(rgb.size())))) {
                return false;
            }

            // Collapse RGB subpixels → single grayscale byte. The mean
            // is what a non-subpixel sampler perceives; rounding via
            // +1 keeps integer division from biasing toward darker.
            out.pixels.assign(static_cast<size_t>(w) * h, 0);
            for (int32_t y = 0; y < h; ++y) {
                const uint8_t* src = rgb.data() + static_cast<size_t>(y) * w * 3;
                uint8_t*       dst = out.pixels.data() + static_cast<size_t>(y) * w;
                for (int32_t x = 0; x < w; ++x) {
                    const uint32_t r = src[x * 3 + 0];
                    const uint32_t g = src[x * 3 + 1];
                    const uint32_t b = src[x * 3 + 2];
                    dst[x] = static_cast<uint8_t>((r + g + b + 1) / 3);
                }
            }
            out.widthPx  = w;
            out.heightPx = h;
            out.originX  = bounds.left;
            out.originY  = bounds.top;
            return true;
        }

        // -------- Shelf packer ----------------------------------------
        //
        // Trivial top-down shelf algorithm: sort glyphs by descending
        // height, walk them, place left-to-right on the current shelf
        // until it overflows the atlas width, then start a new shelf
        // below using the height of the first glyph placed on it.
        //
        // For our working set (~600 glyphs, 1024×1024 atlas, max glyph
        // ~30×52) shelf packing leaves only a few percent of slack —
        // worth swapping for a smarter packer only if we ever push into
        // a smaller atlas. Today we trade ~5% area for ~30 lines of
        // packer code.
        struct PendingGlyph {
            GlyphFace face;
            uint16_t  sizePx;
            wchar_t   codepoint;
            GlyphRaster raster;
        };

        struct Shelf {
            uint16_t cursorX = 0;   // next free pixel on the shelf
            uint16_t baseY   = 0;   // top of the shelf
            uint16_t height  = 0;   // tallest glyph on this shelf so far
        };

        // Try to place `gw × gh` into the current shelf or open a new
        // one. Returns false if the atlas is full.
        bool placeOnShelf(
            uint16_t  atlasW,
            uint16_t  atlasH,
            uint8_t   padding,
            uint16_t  gw,
            uint16_t  gh,
            Shelf&    shelf,
            uint16_t& outX,
            uint16_t& outY)
        {
            // Reserve one pixel of padding on the right + below each
            // glyph; left and top come from the previous neighbour's
            // padding (or the atlas edge). This keeps bilinear taps from
            // bleeding between glyphs.
            const uint16_t cellW = static_cast<uint16_t>(gw + padding);
            const uint16_t cellH = static_cast<uint16_t>(gh + padding);

            // Hard rejection up front: a single glyph that doesn't fit
            // either atlas axis can never be placed. Without this gate
            // the "current shelf" branch silently misplaces a too-wide
            // glyph (outX=0 + memcpy with gw > atlasStride → row-by-row
            // overflow), and the "new shelf" branch never checks the
            // height of the very first glyph on a baseY=0 shelf — a
            // glyph taller than the atlas writes past the buffer end.
            // Unreachable today (max glyph ~52px on a 1024×2048 atlas),
            // but the system-font fallback (corrupted / atypical face)
            // could turn this latent into host-process heap corruption,
            // which the "never crash the host" rule forbids.
            if (cellW > atlasW || cellH > atlasH) return false;

            if (shelf.cursorX + cellW <= atlasW) {
                // Fits on current shelf.
                outX = shelf.cursorX;
                outY = shelf.baseY;
                shelf.cursorX = static_cast<uint16_t>(shelf.cursorX + cellW);
                if (cellH > shelf.height) shelf.height = cellH;
                return true;
            }

            // Need to open a new shelf below. Verify it fits the atlas.
            const uint16_t newBaseY = static_cast<uint16_t>(shelf.baseY + shelf.height);
            if (newBaseY + cellH > atlasH) return false;

            shelf.baseY   = newBaseY;
            shelf.cursorX = cellW;
            shelf.height  = cellH;
            outX = 0;
            outY = newBaseY;
            return true;
        }

        // Blit a glyph raster into the atlas at (x, y). Row-major, R8.
        void blit(
            uint8_t*  atlas,
            uint16_t  atlasStride,
            uint16_t  dstX,
            uint16_t  dstY,
            const GlyphRaster& g)
        {
            for (int32_t row = 0; row < g.heightPx; ++row) {
                const uint8_t* src = g.pixels.data() + static_cast<size_t>(row) * g.widthPx;
                uint8_t*       dst = atlas
                    + static_cast<size_t>(dstY + row) * atlasStride
                    + dstX;
                std::memcpy(dst, src, static_cast<size_t>(g.widthPx));
            }
        }

    }   // namespace

    bool build(const BuildSpec& spec, BuildResult& out) {
        if (!spec.dwriteFactory) return false;
        if (spec.atlasWidthPx == 0 || spec.atlasHeightPx == 0) return false;

        IDWriteFactory* factory = spec.dwriteFactory.Get();
        IDWriteFontCollection* collection = spec.fontCollection.Get();

        // -------- Phase 1: resolve faces + read metrics ---------------
        //
        // One entry per (face, sizePx) — same key the renderer will use
        // at draw time. We resolve up front so a missing face fails fast
        // and we don't waste time rasterizing glyphs we couldn't place.
        std::unordered_map<FaceMetricsKey, ResolvedFace> faces;
        faces.reserve(spec.requests.size());

        for (const BuildRequest& req : spec.requests) {
            const FaceMetricsKey key = makeFaceMetricsKey(req.face, req.sizePx);
            if (faces.find(key) != faces.end()) continue;

            const FaceStyle style = styleFor(req.face);
            ResolvedFace resolved{};
            if (!resolveFace(factory, collection, familyFor(req.face, spec),
                             style.weight, style.style,
                             static_cast<float>(req.sizePx), resolved)) {
                // Hard failure: a requested face couldn't be resolved
                // even via system fallback. The renderer cannot draw
                // anything, so we abort the build.
                return false;
            }
            FaceMetrics fm{};
            fm.ascent    = resolved.ascent;
            fm.descent   = resolved.descent;
            fm.lineGap   = resolved.lineGap;
            fm.capHeight = resolved.capHeight;
            out.faceMetrics.emplace(key, fm);
            faces.emplace(key, std::move(resolved));
        }

        // -------- Phase 2: rasterize all glyphs to CPU buffers --------
        //
        // We rasterize before packing because packing needs the glyph
        // bbox, which only emerges from GetAlphaTextureBounds. Buffer
        // each glyph in a PendingGlyph; once everything is rasterized
        // we sort by height descending and shelf-pack.
        //
        // Duplicate (face, sizePx, codepoint) tuples across requests
        // are dropped silently — the map insert below collapses them.
        std::vector<PendingGlyph> pending;
        // Rough upper bound: sum of (charset sizes). Worth reserving;
        // resizing during raster blows the working set 20-50 MB at
        // peak.
        size_t roughCount = 0;
        for (const BuildRequest& req : spec.requests) roughCount += req.charset.size();
        pending.reserve(roughCount);

        // Track inserted (face, sizePx, cp) tuples for de-dup.
        std::unordered_map<GlyphKey, bool> seen;
        seen.reserve(roughCount);

        for (const BuildRequest& req : spec.requests) {
            const FaceMetricsKey faceKey = makeFaceMetricsKey(req.face, req.sizePx);
            auto faceIt = faces.find(faceKey);
            if (faceIt == faces.end()) continue;   // unreachable: phase 1 inserted it
            ResolvedFace& resolved = faceIt->second;

            for (wchar_t cp : req.charset) {
                const GlyphKey gk = makeGlyphKey(req.face, req.sizePx, static_cast<uint16_t>(cp));
                if (!seen.emplace(gk, true).second) continue;   // duplicate

                GlyphRaster raster{};
                if (!rasterizeGlyph(factory, resolved, cp,
                                    static_cast<float>(req.sizePx), raster)) {
                    out.missingGlyphs++;
                    continue;
                }
                PendingGlyph pg{};
                pg.face      = req.face;
                pg.sizePx    = req.sizePx;
                pg.codepoint = cp;
                pg.raster    = std::move(raster);
                pending.push_back(std::move(pg));
            }
        }

        // -------- Phase 3: shelf pack ---------------------------------
        //
        // Sort by descending height first — that's the standard trick
        // that makes shelf packing nearly as good as more elaborate
        // bin packers for typeset glyphs (which cluster around a small
        // set of heights per size).
        std::sort(pending.begin(), pending.end(),
                  [](const PendingGlyph& a, const PendingGlyph& b) {
                      return a.raster.heightPx > b.raster.heightPx;
                  });

        out.atlasWidth  = spec.atlasWidthPx;
        out.atlasHeight = spec.atlasHeightPx;
        out.bitmap.assign(static_cast<size_t>(spec.atlasWidthPx) * spec.atlasHeightPx, 0);
        out.glyphs.reserve(pending.size());

        Shelf shelf{};
        for (const PendingGlyph& pg : pending) {
            const uint16_t gw = static_cast<uint16_t>(pg.raster.widthPx);
            const uint16_t gh = static_cast<uint16_t>(pg.raster.heightPx);

            uint16_t dstX = 0;
            uint16_t dstY = 0;
            if (gw == 0 || gh == 0) {
                // Whitespace glyph (e.g. ' '). Record advance only,
                // no pixels to blit. Keep u/v at 0 so a buggy renderer
                // still samples a sane pixel.
                dstX = 0;
                dstY = 0;
            } else {
                if (!placeOnShelf(spec.atlasWidthPx, spec.atlasHeightPx,
                                  spec.padding, gw, gh, shelf, dstX, dstY)) {
                    // Atlas full. This is a hard failure — atlas was
                    // sized for the working set; running out means the
                    // working set grew without the atlas growing with
                    // it. Bail and let the caller log + degrade.
                    return false;
                }
                blit(out.bitmap.data(), spec.atlasWidthPx, dstX, dstY, pg.raster);
            }

            GlyphInfo gi{};
            gi.u        = dstX;
            gi.v        = dstY;
            gi.w        = gw;
            gi.h        = gh;
            gi.bearingX = static_cast<int16_t>(pg.raster.originX);
            // DirectWrite returns originY measured from the baseline
            // going DOWN (i.e. originY = -ascent of the glyph for
            // typical letterforms). Convert to "positive = up from
            // baseline" so the renderer can use the standard
            // (penX + bearingX, baselineY - bearingY) placement.
            gi.bearingY = static_cast<int16_t>(-pg.raster.originY);
            gi.advanceX = pg.raster.advanceX;
            out.glyphs.emplace(
                makeGlyphKey(pg.face, pg.sizePx,
                             static_cast<uint16_t>(pg.codepoint)),
                gi);
        }

        return true;
    }

}   // namespace openxr_api_layer::utils::glyph_atlas
