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
    // Every chrome shape is a sequence of filled axis-aligned quads, each
    // optionally corner-rounded by the quad shader's SDF (radius 0 = sharp,
    // byte-for-byte the old plain rect):
    //   * panel BG:        2 nested rounded rects (1-px border + inset fill)
    //   * outer frame:     2 nested rounded rects (border + inset fill)
    //   * column separator: 1 thin sharp rect
    //
    // Lifecycle mirrors glyph_atlas::Renderer:
    //   * init(device, ctx, target) primes shaders / input layout /
    //     buffers / RTV against the passed target texture.
    //   * One paint is:
    //         beginBatch();
    //         addRect (any number);
    //         flush();
    //     beginBatch clears the scratch vector; addRect appends one
    //     QuadInstance; flush DISCARD-maps the dynamic instance buffer,
    //     uploads the whole vector, sets the full pipeline state, and
    //     emits a single DrawInstanced.
    //
    // Re-uses the offline-compiled `overlay_quad_vs` / `overlay_quad_ps`
    // bytecode the HistogramBarRenderer already pulls in — same shader,
    // same per-instance layout (rect + color + corner radius), so an HLSL
    // tweak lands for both call sites at once.
    //
    // Failure semantics: init returns false on any D3D11 creation
    // failure; the caller logs and disables the overlay entirely (fail-
    // closed — there is no D2D fallback). The layer never crashes the
    // host for a chrome-rendering miss.
    // ===================================================================
    // ---- Shared overlay_quad per-instance layout --------------------
    //
    // ONE source of truth for the quad instance + its D3D11 input layout,
    // shared by this Renderer AND HistogramBarRenderer (overlay_renderer.cpp)
    // so the two pipelines that feed the same overlay_quad shader can't
    // drift. Mirrors overlay_quad.hlsli's QuadVSInput byte-for-byte:
    //   rect(16) + color(16) + borderColor(16) + radius(4) + borderWidth(4)
    //   + pad(8) = 64 B. QUAD_PARAMS (offset 48) packs {radius, borderWidth}
    //   into its .xy; .zw (pad) is unread.
    struct QuadInstance {
        float rect[4];         // x, y, w, h (px, top-left origin)
        float color[4];        // fill colour, straight-alpha
        float borderColor[4];  // border-ring colour (used when borderWidth > 0)
        float radius;          // corner radius px; 0 = sharp
        float borderWidth;     // border ring px; 0 = no border
        float pad[2];          // → the QUAD_PARAMS float4 slot at offset 48
    };
    static_assert(sizeof(QuadInstance) == 64,
                   "QuadInstance must match overlay_quad.hlsli's QuadVSInput "
                   "per-instance layout byte-for-byte");

    inline constexpr D3D11_INPUT_ELEMENT_DESC kQuadInputLayout[] = {
        {"POSITION",          0, DXGI_FORMAT_R32G32_FLOAT,       0,  0,
         D3D11_INPUT_PER_VERTEX_DATA,   0},
        {"QUAD_RECT",         0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0,
         D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"QUAD_COLOR",        0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16,
         D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"QUAD_BORDER_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32,
         D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"QUAD_PARAMS",       0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48,
         D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };

    class Renderer {
      public:
        // RTV is supplied per-flush by the caller, not stored — so one
        // pipeline (state + buffers) can serve different targets (the
        // in-engine paint texture / shim, and the snapshot test's own
        // texture). dstWidth/dstHeight are the fixed pixel dimensions of
        // those targets; they pre-populate the cbuffer's texSize and
        // stay constant for the renderer's lifetime (always kTexW × kTexH).
        // renderWidth/renderHeight: PHYSICAL render-target (viewport) dims,
        // distinct from the LOGICAL dstWidth/dstHeight (cbuffer texSize) only
        // when the overlay is supersampled. Default 0 → "= dst" (factor 1.0,
        // legacy + golden-test path). Chrome rects are solid quads, so the
        // viewport stretch from logical→physical is lossless (no atlas, no
        // resampling) — only the viewport changes here.
        bool init(Microsoft::WRL::ComPtr<ID3D11Device>        device,
                  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx,
                  UINT                                        dstWidth,
                  UINT                                        dstHeight,
                  UINT                                        renderWidth  = 0,
                  UINT                                        renderHeight = 0);

        bool isReady() const noexcept { return m_ready; }

        // -------- Batched draw -----------------------------------------
        void beginBatch() noexcept;

        // Filled axis-aligned rect. `color` is straight-alpha RGBA.
        // The pointer is read immediately into the scratch vector, so
        // the caller can let `color` point at a temporary.
        // `cornerRadius` (pixels) rounds the four corners via the quad
        // shader's SDF path; 0 (the default) keeps the old sharp rect,
        // byte-for-byte. The shader clamps the radius to half the shorter
        // side, so an over-large value just yields a stadium.
        void addRect(float x, float y, float w, float h,
                     const float color[4], float cornerRadius = 0.0f);

        // Rounded filled rect with an optional border ring, both via the
        // quad shader's SDF. `fillColor` is the (possibly translucent)
        // interior; `borderColor` paints a ring `borderWidth` px wide just
        // inside the rounded outline (borderWidth <= 0 → no ring).
        // `cornerRadius` is clamped in-shader to half the shorter side.
        // Unlike two nested rects this is ONE quad, so a translucent fill
        // keeps its alpha — nothing opaque is drawn behind it.
        void addRoundedRect(float x, float y, float w, float h,
                            const float fillColor[4],
                            const float borderColor[4],
                            float borderWidth, float cornerRadius);

        // Flush: sets full pipeline state on the immediate context
        // (targeting `rtv`) and emits one DrawInstanced for all queued
        // rects. Returns true on success (incl. an empty no-op), false
        // only on a real GPU failure (buffer-grow / Map) — the caller
        // uses that to suppress a frame that would otherwise composite
        // over a freshly-cleared target.
        bool flush(ID3D11RenderTargetView* rtv);

      private:
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
        // LOGICAL target dimensions snapshotted at init for the cbuffer's
        // texSize; PHYSICAL render dims drive the flush-time viewport. Equal
        // unless supersampled.
        UINT                                            m_dstW = 0;
        UINT                                            m_dstH = 0;
        UINT                                            m_renderW = 0;
        UINT                                            m_renderH = 0;

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
