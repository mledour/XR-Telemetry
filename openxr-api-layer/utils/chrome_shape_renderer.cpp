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

#include "chrome_shape_renderer.h"

#include "framework/log.h"   // Log()

// Same offline-compiled overlay_quad bytecode HistogramBarRenderer
// pulls in. Each generated header declares a `static const BYTE
// g_<filename>[]` (internal linkage), so including in two TUs gives
// each TU its own copy of the constants — ODR-safe, ~2 KB extra
// per binary for the duplicated bytecode.
#include "overlay_quad_vs.h"
#include "overlay_quad_ps.h"

namespace openxr_api_layer::utils::chrome_shapes {

    // Log() lives in openxr_api_layer::log — pull it in with an
    // explicit using-declaration the same way overlay_renderer.cpp
    // and glyph_atlas_renderer.cpp do.
    using ::openxr_api_layer::log::Log;

    using Microsoft::WRL::ComPtr;

    bool Renderer::init(ComPtr<ID3D11Device>        device,
                        ComPtr<ID3D11DeviceContext> ctx,
                        UINT                        dstWidth,
                        UINT                        dstHeight) {
        if (!device || !ctx || dstWidth == 0 || dstHeight == 0) return false;

        m_device = std::move(device);
        m_ctx    = std::move(ctx);
        m_dstW   = dstWidth;
        m_dstH   = dstHeight;

        auto fail = [](const char* step) {
            Log(fmt::format(
                "xr_telemetry: ChromeShapeRenderer init failed at "
                "step: {}\n", step));
            return false;
        };

        if (!createPipeline()) return fail("createPipeline");
        if (!createBuffers())  return fail("createBuffers");

        m_scratch.reserve(kInitialInstances);

        m_ready = true;
        return true;
    }

    bool Renderer::createPipeline() {
        if (FAILED(m_device->CreateVertexShader(
                g_overlay_quad_vs, sizeof(g_overlay_quad_vs),
                nullptr, m_vs.GetAddressOf()))) return false;
        if (FAILED(m_device->CreatePixelShader(
                g_overlay_quad_ps, sizeof(g_overlay_quad_ps),
                nullptr, m_ps.GetAddressOf()))) return false;

        // Mirror HistogramBarRenderer's quad input layout exactly.
        const D3D11_INPUT_ELEMENT_DESC il[] = {
            {"POSITION",   0, DXGI_FORMAT_R32G32_FLOAT,       0,  0,
             D3D11_INPUT_PER_VERTEX_DATA,   0},
            {"QUAD_RECT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0,
             D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"QUAD_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16,
             D3D11_INPUT_PER_INSTANCE_DATA, 1},
        };
        if (FAILED(m_device->CreateInputLayout(
                il, _countof(il),
                g_overlay_quad_vs, sizeof(g_overlay_quad_vs),
                m_layout.GetAddressOf())))
            return false;

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable    = TRUE;
        bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(m_device->CreateBlendState(
                &bd, m_blend.GetAddressOf()))) return false;

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = FALSE;
        rd.DepthClipEnable = TRUE;
        if (FAILED(m_device->CreateRasterizerState(
                &rd, m_raster.GetAddressOf()))) return false;

        return true;
    }

    bool Renderer::createBuffers() {
        const float quadVerts[] = {
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 1.0f, 1.0f, 1.0f,
        };
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(quadVerts);
        bd.Usage     = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA srd{};
        srd.pSysMem = quadVerts;
        if (FAILED(m_device->CreateBuffer(
                &bd, &srd, m_quadVB.GetAddressOf()))) return false;

        if (!m_batch.init(m_device, m_ctx,
                          static_cast<UINT>(sizeof(QuadInstance)),
                          kInitialInstances, kMaxInstances,
                          "ChromeShapeRenderer")) return false;

        // texSize derived once from the init dstWidth/dstHeight. IMMUTABLE
        // because the swapchain image dimensions are fixed for the
        // renderer's lifetime.
        const QuadConstants init{
            { static_cast<float>(m_dstW), static_cast<float>(m_dstH) },
            { 0.0f, 0.0f }
        };
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(init);
        cbd.Usage     = D3D11_USAGE_IMMUTABLE;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA cbsrd{};
        cbsrd.pSysMem = &init;
        if (FAILED(m_device->CreateBuffer(
                &cbd, &cbsrd, m_cb.GetAddressOf()))) return false;

        return true;
    }

    void Renderer::beginBatch() noexcept {
        m_scratch.clear();
    }

    void Renderer::addRect(float x, float y, float w, float h,
                            const float color[4]) {
        if (!m_ready || w <= 0.0f || h <= 0.0f) return;
        QuadInstance q{};
        q.rect[0]  = x;     q.rect[1]  = y;
        q.rect[2]  = w;     q.rect[3]  = h;
        q.color[0] = color[0]; q.color[1] = color[1];
        q.color[2] = color[2]; q.color[3] = color[3];
        m_scratch.push_back(q);
    }

    void Renderer::addOutline(float x, float y, float w, float h,
                               float strokeWidth, const float color[4]) {
        if (strokeWidth <= 0.0f) return;
        // Top + bottom strokes run the full width; the side strokes
        // are inset by `strokeWidth` so they butt against the
        // horizontals without overlapping. On opaque BG (every chrome
        // colour we paint is alpha=1) this is purely cosmetic, but
        // it stays consistent if a future translucent stroke colour
        // ever ships.
        addRect(x,             y,             w,            strokeWidth,  color);  // top
        addRect(x,             y + h - strokeWidth, w,      strokeWidth,  color);  // bottom
        addRect(x,             y + strokeWidth,     strokeWidth, h - 2 * strokeWidth, color); // left
        addRect(x + w - strokeWidth, y + strokeWidth, strokeWidth, h - 2 * strokeWidth, color); // right
    }

    bool Renderer::flush(ID3D11RenderTargetView* rtv) {
        // Returns true if the chrome was drawn (or there was nothing to
        // draw), false only on a real GPU failure (buffer-grow / Map),
        // so the caller can suppress a frame that would otherwise
        // composite bars over a freshly-cleared, chrome-less target.
        //
        // Scratch is owned by beginBatch() (the ~10 Hz chrome rebuild)
        // and persists between flushes so every frame can re-upload the
        // same chrome. So NONE of the bail-outs here clear it — a
        // transient failure must leave the last-good scratch intact so
        // the next frame's flush retries instead of dropping the chrome
        // until the next beginBatch.
        if (!m_ready || !rtv) return false;
        if (m_scratch.empty()) return true;

        const UINT count = static_cast<UINT>(m_scratch.size());
        if (!m_batch.upload({ { m_scratch.data(), count } })) return false;

        // Full pipeline state — caller is assumed to have left the
        // device in an unknown configuration. Viewport uses the same
        // (dstW, dstH) the cbuffer was built with, so pixel-space NDC
        // math in the VS stays consistent across paints.
        m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{
            0.0f, 0.0f,
            static_cast<float>(m_dstW), static_cast<float>(m_dstH),
            0.0f, 1.0f};
        m_ctx->RSSetViewports(1, &vp);
        m_ctx->RSSetState(m_raster.Get());
        const float bf[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        m_ctx->OMSetBlendState(m_blend.Get(), bf, 0xffffffffu);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        m_ctx->IASetInputLayout(m_layout.Get());

        ID3D11Buffer* vbs[2] = {m_quadVB.Get(), m_batch.buffer()};
        const UINT strides[2] = {sizeof(float) * 2, sizeof(QuadInstance)};
        const UINT offsets[2] = {0, 0};
        m_ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);

        m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
        m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
        m_ctx->VSSetConstantBuffers(0, 1, m_cb.GetAddressOf());

        m_ctx->DrawInstanced(4, count, 0, 0);
        // Scratch is NOT cleared — kept alive between flushes so the
        // caller can re-flush the same chrome onto every swapchain
        // image without paying the drawChrome rebuild cost each frame.
        // beginBatch() is the only way to drop scratch.
        return true;
    }

}   // namespace openxr_api_layer::utils::chrome_shapes
