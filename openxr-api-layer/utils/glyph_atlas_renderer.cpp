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

#include "glyph_atlas_renderer.h"

#include "framework/log.h"   // Log()

// Offline-compiled shader bytecode (FxCompile → $(IntDir)). Each header
// declares a `static const BYTE g_<filename>[]` (internal linkage), so
// including the same generated header in multiple TUs is ODR-safe — each
// TU gets its own copy of the bytecode constants.
#include "overlay_text_vs.h"
#include "overlay_text_ps.h"

namespace openxr_api_layer::utils::glyph_atlas {

    // Log() lives in openxr_api_layer::log. EXPLICIT using-declaration
    // (not a using-directive) — matches the pattern in overlay_renderer.cpp
    // which uses the same form to dodge an MSVC name-lookup quirk inside
    // nested namespaces.
    using ::openxr_api_layer::log::Log;

    using Microsoft::WRL::ComPtr;

    // ===================================================================
    // init()
    // ===================================================================
    bool Renderer::init(ComPtr<ID3D11Device>        device,
                        ComPtr<ID3D11DeviceContext> ctx,
                        UINT                        dstWidth,
                        UINT                        dstHeight,
                        const BuildResult&          atlas) {
        if (!device || !ctx || dstWidth == 0 || dstHeight == 0) return false;
        if (atlas.atlasWidth == 0 || atlas.atlasHeight == 0) return false;
        if (atlas.bitmap.empty()) return false;

        m_device = std::move(device);
        m_ctx    = std::move(ctx);
        m_dstW   = dstWidth;
        m_dstH   = dstHeight;
        // Snapshot the atlas dimensions before createBuffers — it bakes
        // them (with texSize) into an IMMUTABLE constant buffer, so they
        // must be set first.
        m_atlasW = atlas.atlasWidth;
        m_atlasH = atlas.atlasHeight;

        // Per-step failure logging — same pattern as HistogramBarRenderer.
        // Loses a tiny amount of code but makes "text rendering disabled"
        // reports actionable.
        auto fail = [](const char* step) {
            Log(fmt::format(
                "xr_telemetry: GlyphAtlasRenderer init failed at step: {}\n",
                step));
            return false;
        };

        // RTV is supplied per-flush — see Renderer::flush.

        if (!createPipeline())       return fail("createPipeline");
        if (!createBuffers())        return fail("createBuffers");
        if (!createAtlasTexture(atlas)) return fail("createAtlasTexture");

        // Copy the atlas tables onto our long-lived storage. The bitmap
        // itself was already pushed into the GPU texture by
        // createAtlasTexture (CreateTexture2D + SUBRESOURCE_DATA), so the
        // CPU-side bitmap can stay shared with whatever the caller's
        // BuildResult holder owns.
        m_glyphs       = atlas.glyphs;
        m_faceMetrics  = atlas.faceMetrics;

        m_dynamicScratch.reserve(kInitialInstances);
        m_staticScratch.reserve(kInitialStaticInstances);

        m_ready = true;
        return true;
    }

    // ===================================================================
    // createPipeline() — shaders, input layout, sampler, blend, raster.
    // ===================================================================
    bool Renderer::createPipeline() {
        if (FAILED(m_device->CreateVertexShader(
                g_overlay_text_vs, sizeof(g_overlay_text_vs),
                nullptr, m_vs.GetAddressOf())))
            return false;
        if (FAILED(m_device->CreatePixelShader(
                g_overlay_text_ps, sizeof(g_overlay_text_ps),
                nullptr, m_ps.GetAddressOf())))
            return false;

        // Input layout: slot 0 = per-vertex unit-quad corner (R32G32),
        // slot 1 = per-instance TextInstance (12 floats packed).
        const D3D11_INPUT_ELEMENT_DESC il[] = {
            {"POSITION",     0, DXGI_FORMAT_R32G32_FLOAT,        0,  0,
             D3D11_INPUT_PER_VERTEX_DATA,    0},
            {"TEXT_RECT",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  1,  0,
             D3D11_INPUT_PER_INSTANCE_DATA,  1},
            {"TEXT_UV_RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,  1, 16,
             D3D11_INPUT_PER_INSTANCE_DATA,  1},
            {"TEXT_COLOR",   0, DXGI_FORMAT_R32G32B32A32_FLOAT,  1, 32,
             D3D11_INPUT_PER_INSTANCE_DATA,  1},
        };
        if (FAILED(m_device->CreateInputLayout(
                il, _countof(il),
                g_overlay_text_vs, sizeof(g_overlay_text_vs),
                m_layout.GetAddressOf())))
            return false;

        // POINT clamp sampler: the atlas is laid out at pixel-aligned
        // destination positions, so nearest-neighbour sampling avoids
        // softening the glyph edges. Bilinear would smear AA grays across
        // the 1-pixel padding border between glyphs — POINT keeps each
        // sample inside its source glyph.
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(m_device->CreateSamplerState(
                &sd, m_sampler.GetAddressOf())))
            return false;

        // Straight alpha-over — same as bars/quad so chrome + bars + text
        // composite consistently against the shim background.
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
                &bd, m_blend.GetAddressOf())))
            return false;

        // No culling, no scissor by default. Text usually paints inside
        // the chrome/panel rects already — callers can scissor externally
        // if a future layout needs hard clipping (e.g. a panel that
        // overflows on long FPS strings).
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = FALSE;
        rd.DepthClipEnable = TRUE;
        if (FAILED(m_device->CreateRasterizerState(
                &rd, m_raster.GetAddressOf())))
            return false;

        return true;
    }

    // ===================================================================
    // createBuffers() — unit-quad VB, instance VB, constant buffer.
    // ===================================================================
    bool Renderer::createBuffers() {
        // Unit quad — same 4-corner triangle strip as bars/quad. The
        // VS lerps each corner against the per-instance rect.
        const float quadVerts[] = {
            0.0f, 0.0f,  1.0f, 0.0f,
            0.0f, 1.0f,  1.0f, 1.0f,
        };
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(quadVerts);
            bd.Usage     = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA srd{};
            srd.pSysMem = quadVerts;
            if (FAILED(m_device->CreateBuffer(
                    &bd, &srd, m_quadVB.GetAddressOf())))
                return false;
        }

        // Dynamic instance buffer (growth + upload owned by m_batch).
        if (!m_batch.init(m_device.Get(), m_ctx.Get(),
                          static_cast<UINT>(sizeof(TextInstance)),
                          kInitialInstances, kMaxInstances,
                          "GlyphAtlasRenderer")) return false;

        // Constant buffer: IMMUTABLE. Both texSize (the target dims) and
        // atlasSize are fixed for the renderer's lifetime, so we bake
        // them once here and never touch the cbuffer again — no per-frame
        // Map/Unmap on the hot path (matches chrome_shapes::Renderer,
        // which does the same for its texSize cbuffer).
        const TextConstants tc{
            { static_cast<float>(m_dstW),   static_cast<float>(m_dstH) },
            { static_cast<float>(m_atlasW), static_cast<float>(m_atlasH) }
        };
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = sizeof(TextConstants);
        bd.Usage          = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA srd{};
        srd.pSysMem = &tc;
        if (FAILED(m_device->CreateBuffer(
                &bd, &srd, m_cb.GetAddressOf())))
            return false;

        return true;
    }

    // ===================================================================
    // createAtlasTexture() — upload R8 bitmap into a Texture2D + SRV.
    // ===================================================================
    bool Renderer::createAtlasTexture(const BuildResult& atlas) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width            = atlas.atlasWidth;
        td.Height           = atlas.atlasHeight;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_IMMUTABLE;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA srd{};
        srd.pSysMem     = atlas.bitmap.data();
        srd.SysMemPitch = atlas.atlasWidth;   // row stride in bytes = width (R8)

        if (FAILED(m_device->CreateTexture2D(
                &td, &srd, m_atlasTex.GetAddressOf())))
            return false;

        if (FAILED(m_device->CreateShaderResourceView(
                m_atlasTex.Get(), nullptr, m_atlasSRV.GetAddressOf())))
            return false;

        return true;
    }

    // ===================================================================
    // beginStaticBatch / beginDynamicBatch / drawRun / measure / metrics
    // ===================================================================
    void Renderer::beginStaticBatch() noexcept {
        m_activeStatic = true;
        m_staticScratch.clear();
    }

    void Renderer::beginDynamicBatch() noexcept {
        m_activeStatic = false;
        m_dynamicScratch.clear();
    }

    float Renderer::drawRun(GlyphFace      face,
                            uint16_t       sizePx,
                            float          penX,
                            float          baselineY,
                            const wchar_t* s,
                            std::size_t    n,
                            const float    color[4]) {
        if (!m_ready || n == 0 || !s) return penX;

        // Append into whichever tier beginStaticBatch / beginDynamicBatch
        // last selected (dynamic by default).
        std::vector<TextInstance>& scratch =
            m_activeStatic ? m_staticScratch : m_dynamicScratch;

        float x = penX;
        for (std::size_t i = 0; i < n; ++i) {
            const wchar_t cp = s[i];
            const GlyphKey key = makeGlyphKey(face, sizePx, static_cast<uint16_t>(cp));
            auto it = m_glyphs.find(key);
            if (it == m_glyphs.end()) {
                // Missing glyph: advance by the face's space width so
                // the rest of the string still lands at the right
                // position. Drops the visible character — the renderer
                // never crashes for a stray codepoint.
                x += fallbackAdvance(face, sizePx);
                continue;
            }
            const GlyphInfo& gi = it->second;

            // Whitespace glyph (w == 0 || h == 0): no instance, just
            // advance the pen.
            if (gi.w > 0 && gi.h > 0) {
                // Quad destination = pen-space + glyph bearings.
                //   left   = x + bearingX
                //   top    = baselineY - bearingY  (bearingY is from
                //                                    baseline up, positive)
                TextInstance ti{};
                ti.rect[0] = x + static_cast<float>(gi.bearingX);
                ti.rect[1] = baselineY - static_cast<float>(gi.bearingY);
                ti.rect[2] = static_cast<float>(gi.w);
                ti.rect[3] = static_cast<float>(gi.h);
                ti.uvRect[0] = static_cast<float>(gi.u);
                ti.uvRect[1] = static_cast<float>(gi.v);
                ti.uvRect[2] = static_cast<float>(gi.w);
                ti.uvRect[3] = static_cast<float>(gi.h);
                ti.color[0] = color[0];
                ti.color[1] = color[1];
                ti.color[2] = color[2];
                ti.color[3] = color[3];
                scratch.push_back(ti);
            }
            x += gi.advanceX;
        }
        return x;
    }

    float Renderer::measure(GlyphFace      face,
                            uint16_t       sizePx,
                            const wchar_t* s,
                            std::size_t    n) const {
        if (!s || n == 0) return 0.0f;
        float w = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const GlyphKey key =
                makeGlyphKey(face, sizePx, static_cast<uint16_t>(s[i]));
            auto it = m_glyphs.find(key);
            if (it != m_glyphs.end()) {
                w += it->second.advanceX;
            } else {
                w += fallbackAdvance(face, sizePx);
            }
        }
        return w;
    }

    const FaceMetrics* Renderer::metrics(GlyphFace face, uint16_t sizePx) const {
        const auto it = m_faceMetrics.find(makeFaceMetricsKey(face, sizePx));
        return it == m_faceMetrics.end() ? nullptr : &it->second;
    }

    float Renderer::fallbackAdvance(GlyphFace face, uint16_t sizePx) const {
        // Try the space glyph at the same face+size — should always be
        // baked because ' ' is in the standard ASCII charset.
        const GlyphKey spaceKey =
            makeGlyphKey(face, sizePx, static_cast<uint16_t>(L' '));
        auto it = m_glyphs.find(spaceKey);
        if (it != m_glyphs.end()) return it->second.advanceX;

        // No space glyph baked for this face (e.g. a digits-only face like
        // Barlow) — fall back to a half-em advance so a stray codepoint
        // leaves a visible GAP instead of zero-advancing and overprinting the
        // rest of the run on top of itself. Still never crashes.
        return static_cast<float>(sizePx) * 0.5f;
    }

    // ===================================================================
    // flush() — single DrawInstanced for all queued glyphs.
    // ===================================================================
    bool Renderer::flush(ID3D11RenderTargetView* rtv) {
        // Returns true if the text was drawn (or there was nothing to
        // draw — a successful no-op), false only on a real GPU failure
        // (buffer-grow / Map) so the caller can suppress a frame that
        // would otherwise composite over a freshly-cleared target.
        //
        // Both scratch tiers are owned by beginStaticBatch /
        // beginDynamicBatch and persist between flushes so every frame
        // re-uploads the same text. None of the bail-outs clear them — a
        // transient failure must leave the last-good scratches intact so
        // the next frame retries rather than blanking the text until the
        // next begin*Batch.
        if (!m_ready || !rtv) return false;

        // One DrawInstanced over both tiers: static labels first, then
        // dynamic values, uploaded contiguously into the instance buffer.
        const UINT staticCount   = static_cast<UINT>(m_staticScratch.size());
        const UINT dynamicCount  = static_cast<UINT>(m_dynamicScratch.size());
        const UINT instanceCount = staticCount + dynamicCount;
        if (instanceCount == 0) return true;

        // Grow-if-needed + DISCARD-map upload of both tiers, in order
        // (static labels first, then dynamic values), via the shared
        // InstancedBatchBuffer.
        if (!m_batch.upload({ { m_staticScratch.data(),  staticCount },
                              { m_dynamicScratch.data(), dynamicCount } }))
            return false;

        // The cbuffer (texSize + atlasSize) is IMMUTABLE — baked once in
        // createBuffers, no per-frame upload.

        // Set the full pipeline state — same posture as the bars
        // renderer's drawPanel, the caller is assumed to have left the
        // state in an unknown configuration (chrome shapes, bars, app).
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

        ID3D11Buffer* vbs[2]    = {m_quadVB.Get(), m_batch.buffer()};
        const UINT    strides[2] = {sizeof(float) * 2, sizeof(TextInstance)};
        const UINT    offsets[2] = {0, 0};
        m_ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);

        m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
        m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);

        // The VS reads texSize (b0), the PS reads nothing from b0 — but
        // binding to both stages is harmless and makes the binding
        // pattern symmetric with HistogramBarRenderer's bindBarPipeline().
        m_ctx->VSSetConstantBuffers(0, 1, m_cb.GetAddressOf());

        m_ctx->PSSetShaderResources(0, 1, m_atlasSRV.GetAddressOf());
        m_ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

        m_ctx->DrawInstanced(4, instanceCount, 0, 0);
        // Neither scratch is cleared — both kept alive between flushes so
        // the caller can re-flush the same text onto every swapchain image
        // without paying the drawChrome rebuild cost each frame. The
        // begin*Batch calls are the only way to drop a tier's scratch.
        return true;
    }

}   // namespace openxr_api_layer::utils::glyph_atlas
