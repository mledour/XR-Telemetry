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

#include <cstddef>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "glyph_atlas.h"

namespace openxr_api_layer::utils::glyph_atlas {

    // ===================================================================
    // GlyphAtlas Renderer — D3D11 pipeline that paints text by sampling a
    // pre-baked glyph atlas. Replaces the per-character D2D drawText /
    // DrawTextLayout path inside the overlay's hot frame.
    //
    // Lifecycle:
    //   * init(device, ctx, renderTarget, atlas) — reads the atlas
    //     BuildResult by const-ref: creates a GPU texture from the R8
    //     bitmap, copies the glyph + metrics tables, and primes the
    //     pipeline state (VS/PS, input layout, sampler, blend, RTV
    //     from the supplied target texture). The atlas can be shared
    //     across multiple renderers on different D3D11 devices.
    //   * One frame is:
    //         beginBatch();
    //         drawRun(...); drawRun(...); ...      // any number
    //         flush();
    //     beginBatch resets the scratch instance vector; drawRun appends
    //     one TextInstance per glyph; flush uploads the whole vector in
    //     one DISCARD-mapped write and emits a single DrawInstanced. The
    //     pipeline state is set inside flush() — the renderer assumes the
    //     caller (D2D / bars / external paint) has clobbered everything.
    //
    // Coordinate convention:
    //   * Destination space matches the rest of the overlay: pixel-space
    //     on a (kTexW × kTexH) target, top-left origin, Y down. The VS
    //     handles the Y-flip into clip space.
    //   * `penX` is the absolute pixel X of the FIRST glyph's left side;
    //     `baselineY` is the absolute pixel Y of the baseline. drawRun
    //     advances internally by each glyph's `advanceX` from the atlas
    //     table and returns the final pen position so the caller can
    //     chain runs (e.g. mixed Rajdhani + Barlow Italic on one line).
    //   * Alignment isn't done inside drawRun. The caller picks the
    //     anchor: LEADING is penX directly; TRAILING is penX = anchorX -
    //     measure(...); CENTER is penX = anchorX - 0.5 * measure(...).
    //     This mirrors how D2D's TEXT_ALIGNMENT_TRAILING / _CENTER are
    //     resolved at the layout level, kept explicit here so layout
    //     code can mix faces inside a single anchored block.
    //
    // Thread model:
    //   * All public methods touch the immediate D3D11 context captured
    //     at init() time. Same constraint as the rest of the overlay —
    //     paint happens on the xrEndFrame thread, no cross-thread use.
    // ===================================================================
    class Renderer {
      public:
        // -------- Lifecycle --------------------------------------------
        //
        // `renderTarget` is the D3D11 texture the renderer paints into —
        // today the cross-device shim, eventually the D3D11-wrapped
        // swapchain image directly (Task 15 in the migration plan).
        //
        // `atlas` is taken by const-ref: each renderer creates its own
        // ID3D11Texture2D from the bitmap (the bitmap copy happens via
        // CreateTexture2D's SUBRESOURCE_DATA — D3D11 reads it once and
        // owns the GPU copy), and copies the glyph + metrics tables.
        // This lets a single BuildResult (typically built once on
        // CoreRenderer's DirectWrite factory) feed multiple renderers
        // sitting on different ID3D11Devices — today the app-D3D11
        // device and the D3D11-on-12 shim device, tomorrow potentially
        // additional debug viewers.
        //
        // Returns false on any pipeline-creation failure. Caller logs +
        // degrades to bypass — never crashes the host.
        bool init(Microsoft::WRL::ComPtr<ID3D11Device>          device,
                  Microsoft::WRL::ComPtr<ID3D11DeviceContext>   ctx,
                  Microsoft::WRL::ComPtr<ID3D11Texture2D>       renderTarget,
                  const BuildResult&                            atlas);

        bool isReady() const noexcept { return m_ready; }

        // -------- Batched draw ----------------------------------------
        void beginBatch() noexcept;

        // Emit one glyph quad per character. Missing glyphs (no entry in
        // the atlas table) advance the pen by a fallback width but emit
        // no instance — keeps a stray Unicode codepoint from blocking
        // the whole frame. Returns the pen position after the run.
        float drawRun(GlyphFace            face,
                      uint16_t             sizePx,
                      float                penX,
                      float                baselineY,
                      const wchar_t*       s,
                      std::size_t          n,
                      const float          color[4]);

        // std::wstring convenience overload — most callers already have
        // a wstring on hand.
        float drawRun(GlyphFace            face,
                      uint16_t             sizePx,
                      float                penX,
                      float                baselineY,
                      const std::wstring&  s,
                      const float          color[4]) {
            return drawRun(face, sizePx, penX, baselineY,
                            s.c_str(), s.size(), color);
        }

        // -------- Layout helpers --------------------------------------
        //
        // Measure the total advance of a string at (face, sizePx) — the
        // sum of each glyph's advanceX. Used by the caller to compute
        // TRAILING / CENTER anchors before calling drawRun.
        float measure(GlyphFace      face,
                      uint16_t       sizePx,
                      const wchar_t* s,
                      std::size_t    n) const;

        // Per-(face, sizePx) baseline metrics — ascent / descent / line
        // gap / cap height, in pixels. Returns nullptr if the (face,
        // sizePx) wasn't baked into the atlas. Callers use ascent to
        // turn a "top-of-text Y" into a baseline Y.
        const FaceMetrics* metrics(GlyphFace face, uint16_t sizePx) const;

        // -------- Flush -----------------------------------------------
        //
        // Sets the full pipeline state on the immediate context (RT,
        // viewport, blend, sampler, shaders, layout, vertex / instance
        // buffers, constant buffer) and emits one DrawInstanced for all
        // queued instances. No-op if the scratch vector is empty.
        void flush();

      private:
        // CPU-side mirror of the per-instance vertex data — must match
        // the HLSL `TextVSInput` semantics byte-for-byte (see
        // overlay_text.hlsli).
        struct TextInstance {
            float rect[4];     // dest x, y, w, h (pixels)
            float uvRect[4];   // atlas u, v, w, h (pixels)
            float color[4];    // straight-alpha RGBA
        };

        // Cbuffer mirror — 16-byte register, 2× float2.
        struct TextConstants {
            float texSize[2];     // dest tex (kTexW, kTexH)
            float atlasSize[2];   // atlas (atlasWidth, atlasHeight)
        };

        // Per-flush growth: if a batch exceeds the current instance-buffer
        // capacity, we recreate the buffer at the next power of two. Cap
        // at kMaxInstances to keep a bug from runaway-growing the buffer.
        static constexpr UINT kInitialInstances = 1024;   // ~48 KB
        static constexpr UINT kMaxInstances     = 65536;  // ~3 MB

        bool createPipeline();              // shaders + input layout + states
        bool createBuffers();               // VB + IB + CB
        bool createAtlasTexture(const BuildResult& atlas);
        bool growInstanceBuffer(UINT desired);

        // Map a fallback advance for a missing glyph: use ' ' at the same
        // (face, sizePx). If even that's missing, return 0 — the caller's
        // string just collapses on that glyph.
        float fallbackAdvance(GlyphFace face, uint16_t sizePx) const;

        bool m_ready = false;

        Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_ctx;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_target;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;

        Microsoft::WRL::ComPtr<ID3D11VertexShader>     m_vs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>      m_ps;
        Microsoft::WRL::ComPtr<ID3D11InputLayout>      m_layout;

        Microsoft::WRL::ComPtr<ID3D11Buffer>           m_quadVB;
        Microsoft::WRL::ComPtr<ID3D11Buffer>           m_instanceVB;
        UINT                                            m_instanceCapacity = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer>           m_cb;

        Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_atlasTex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_atlasSRV;
        Microsoft::WRL::ComPtr<ID3D11SamplerState>       m_sampler;
        Microsoft::WRL::ComPtr<ID3D11BlendState>         m_blend;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState>    m_raster;

        // Atlas tables — populated by init from BuildResult, queried
        // every drawRun / measure / metrics call. unordered_map is hot;
        // the GlyphKey is a packed uint32_t (cheap hash).
        std::unordered_map<GlyphKey, GlyphInfo>         m_glyphs;
        std::unordered_map<FaceMetricsKey, FaceMetrics> m_faceMetrics;

        // Atlas pixel dimensions, snapshotted for the cbuffer.
        uint16_t m_atlasW = 0;
        uint16_t m_atlasH = 0;

        // Scratch storage for queued instances — reused across batches.
        std::vector<TextInstance> m_scratch;
    };

}   // namespace openxr_api_layer::utils::glyph_atlas
