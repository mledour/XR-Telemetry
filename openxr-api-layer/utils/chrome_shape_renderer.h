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
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "instanced_batch.h"

namespace openxr_api_layer::utils::chrome_shapes {

    // ===================================================================
    // Chrome-shape Renderer — D3D11 pipeline for the overlay's
    // axis-aligned filled rectangles (panel backgrounds, outer frame,
    // column separators). Replaces D2D's FillRectangle / FillGeometry /
    // DrawLine inside CoreRenderer's drawChrome path with one
    // DrawInstanced call against the existing overlay_quad shaders.
    //
    // After the sharp-corners decision (frame chamfer + panel rounded
    // corners both dropped to plain rectangles), every chrome shape
    // becomes a sequence of filled axis-aligned quads:
    //   * panel BG:        1 fill rect + 4 thin outline rects
    //   * outer frame:     1 fill rect + 4 thin outline rects
    //   * column separator: 1 thin rect
    //
    // Lifecycle mirrors glyph_atlas::Renderer:
    //   * init(device, ctx, target) primes shaders / input layout /
    //     buffers / RTV against the passed target texture.
    //   * One paint is:
    //         beginBatch();
    //         addRect / addOutline (any number);
    //         flush();
    //     beginBatch clears the scratch vector; addRect appends one
    //     QuadInstance; flush DISCARD-maps the dynamic instance buffer,
    //     uploads the whole vector, sets the full pipeline state, and
    //     emits a single DrawInstanced.
    //
    // Re-uses the offline-compiled `overlay_quad_vs` / `overlay_quad_ps`
    // bytecode the HistogramBarRenderer already pulls in — same shader,
    // same per-instance layout (rect + color), so an HLSL tweak lands
    // for both call sites at once.
    //
    // Failure semantics: init returns false on any D3D11 creation
    // failure; the caller logs and disables the overlay entirely (fail-
    // closed — there is no D2D fallback). The layer never crashes the
    // host for a chrome-rendering miss.
    // ===================================================================
    class Renderer {
      public:
        // RTV is supplied per-flush by the caller, not stored — so one
        // pipeline (state + buffers) can serve different targets (the
        // in-engine paint texture / shim, and the snapshot test's own
        // texture). dstWidth/dstHeight are the fixed pixel dimensions of
        // those targets; they pre-populate the cbuffer's texSize and
        // stay constant for the renderer's lifetime (always kTexW × kTexH).
        bool init(Microsoft::WRL::ComPtr<ID3D11Device>        device,
                  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx,
                  UINT                                        dstWidth,
                  UINT                                        dstHeight);

        bool isReady() const noexcept { return m_ready; }

        // -------- Batched draw -----------------------------------------
        void beginBatch() noexcept;

        // Filled axis-aligned rect. `color` is straight-alpha RGBA.
        // The pointer is read immediately into the scratch vector, so
        // the caller can let `color` point at a temporary.
        void addRect(float x, float y, float w, float h,
                     const float color[4]);

        // 1-px (or thicker, via `strokeWidth`) outline around an
        // axis-aligned rect, rendered as 4 thin filled rects. Top
        // and bottom run full width; the two side strokes are inset
        // by strokeWidth so they don't double-paint the corner
        // pixels (alpha-correct on a single layer; cosmetic on
        // straight-alpha against an opaque BG).
        void addOutline(float x, float y, float w, float h,
                        float strokeWidth, const float color[4]);

        // Flush: sets full pipeline state on the immediate context
        // (targeting `rtv`) and emits one DrawInstanced for all queued
        // rects. Returns true on success (incl. an empty no-op), false
        // only on a real GPU failure (buffer-grow / Map) — the caller
        // uses that to suppress a frame that would otherwise composite
        // over a freshly-cleared target.
        bool flush(ID3D11RenderTargetView* rtv);

      private:
        // Must match overlay_quad.hlsli's QuadVSInput per-instance
        // layout byte-for-byte (16 + 16 = 32 bytes).
        struct QuadInstance {
            float rect[4];
            float color[4];
        };
        static_assert(sizeof(QuadInstance) == 32,
                       "QuadInstance must match HLSL per-instance "
                       "layout in overlay_quad.hlsli");

        // overlay_quad's cbuffer carries only texSize. It's set once
        // at init from the target texture's dimensions; never refreshed
        // since the shim is fixed-size for the renderer's lifetime.
        struct QuadConstants {
            float texSize[2];
            float pad[2];
        };
        static_assert(sizeof(QuadConstants) == 16,
                       "QuadConstants must match HLSL cbuffer "
                       "packing in overlay_quad.hlsli");

        static constexpr UINT kInitialInstances = 64;     // ~2 KB
        static constexpr UINT kMaxInstances     = 4096;   // ~128 KB

        bool createPipeline();
        bool createBuffers();

        bool m_ready = false;

        Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_ctx;
        // Target dimensions snapshotted at init for the cbuffer.
        UINT                                            m_dstW = 0;
        UINT                                            m_dstH = 0;

        Microsoft::WRL::ComPtr<ID3D11VertexShader>     m_vs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>      m_ps;
        Microsoft::WRL::ComPtr<ID3D11InputLayout>      m_layout;
        Microsoft::WRL::ComPtr<ID3D11Buffer>           m_quadVB;
        // Dynamic instance VB + its growth + DISCARD-map upload, shared
        // with glyph_atlas::Renderer (see instanced_batch.h).
        InstancedBatchBuffer                           m_batch;
        Microsoft::WRL::ComPtr<ID3D11Buffer>           m_cb;
        Microsoft::WRL::ComPtr<ID3D11BlendState>       m_blend;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState>  m_raster;

        std::vector<QuadInstance> m_scratch;
    };

}   // namespace openxr_api_layer::utils::chrome_shapes
