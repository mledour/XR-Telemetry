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
#include <cstring>
#include <initializer_list>
#include <utility>

#include <d3d11.h>
#include <wrl/client.h>

#include "framework/log.h"   // ::openxr_api_layer::log::Log + fmt (via pch.h)

namespace openxr_api_layer::utils {

    // ===================================================================
    // InstancedBatchBuffer — the D3D11 instanced-batch buffer mechanics
    // shared by glyph_atlas::Renderer (text) and chrome_shapes::Renderer
    // (shapes). Both used to carry a verbatim copy of:
    //   * a DYNAMIC instance vertex buffer + its power-of-two growth,
    //     capped at a max with a Log() + bail on overflow;
    //   * a DISCARD-mapped upload of the scratch into that buffer.
    // Factoring it here means a fix to the growth or map-failure handling
    // lands in ONE place instead of being hand-synced across two renderers.
    //
    // What stays with each renderer (deliberately NOT here): its scratch
    // vector(s), the immutable quad VB + cbuffer, the pipeline objects
    // (shaders / layout / blend / raster / SRV+sampler), the full pipeline-
    // state binding, and the DrawInstanced call. This class owns only the
    // instance buffer + its sizing + the upload.
    //
    // Failure contract (matches both original renderers): on ANY failure
    // path — cap overflow, CreateBuffer failure, or Map failure — the
    // existing buffer contents are left intact (we only Swap() in a new
    // buffer on a successful grow, and DISCARD-map only after the grow
    // succeeds). The caller never clears its scratch on a failed flush, so
    // the next frame re-uploads the last-good batch and self-heals.
    // ===================================================================
    class InstancedBatchBuffer {
      public:
        // One source range to upload: a scratch's first element + the
        // number of instances in it. `data` may point at a typed array
        // (e.g. TextInstance* / QuadInstance*); it's read as raw bytes
        // using the instanceSize fixed at init().
        struct Span {
            const void* data;
            UINT        count;
        };

        // Create the initial buffer. `instanceSize` is sizeof the caller's
        // per-instance struct; `debugName` tags the cap-overflow log so it
        // reads the same as the old per-renderer message. Call once from
        // the renderer's createBuffers(), after device/ctx are known, in
        // place of the old growInstanceBuffer(initialInstances). Returns
        // false on device failure (caller logs + degrades).
        bool init(Microsoft::WRL::ComPtr<ID3D11Device>        device,
                  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx,
                  UINT                                        instanceSize,
                  UINT                                        initialInstances,
                  UINT                                        maxInstances,
                  const char*                                 debugName) {
            m_device       = std::move(device);
            m_ctx          = std::move(ctx);
            m_instanceSize = instanceSize;
            m_initial      = initialInstances;
            m_max          = maxInstances;
            m_debugName    = debugName;
            return grow(initialInstances);
        }

        // Ensure capacity for the total of `spans`, then DISCARD-map and
        // copy every span contiguously in order. Returns true on success
        // (including an empty no-op), false only on cap overflow / device
        // failure / Map failure — the caller suppresses the frame so it
        // doesn't composite over a freshly-cleared target. The instance
        // count to pass to DrawInstanced is the same sum the caller already
        // computes from its scratch sizes.
        bool upload(std::initializer_list<Span> spans) {
            UINT count = 0;
            for (const Span& s : spans) count += s.count;
            if (count == 0) return true;

            if (count > m_capacity && !grow(count)) return false;

            D3D11_MAPPED_SUBRESOURCE map{};
            if (FAILED(m_ctx->Map(m_buffer.Get(), 0,
                    D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                return false;
            }
            auto* dst = static_cast<uint8_t*>(map.pData);
            for (const Span& s : spans) {
                const SIZE_T bytes =
                    static_cast<SIZE_T>(s.count) * m_instanceSize;
                if (bytes) {
                    std::memcpy(dst, s.data, bytes);
                    dst += bytes;
                }
            }
            m_ctx->Unmap(m_buffer.Get(), 0);
            return true;
        }

        // The instance VB to bind at slot 1 of IASetVertexBuffers (slot 0
        // stays the caller's immutable quad VB, whose stride differs per
        // renderer). Valid after a successful init().
        ID3D11Buffer* buffer() const noexcept { return m_buffer.Get(); }

      private:
        // Power-of-two growth, capped at m_max. Logs + bails past the cap;
        // returns false on CreateBuffer failure. The new buffer replaces
        // the old one only on success (Swap), so a failed grow leaves the
        // prior buffer — and thus the last-good batch — usable.
        bool grow(UINT desired) {
            UINT next = m_capacity ? m_capacity : m_initial;
            while (next < desired) next *= 2;
            if (next > m_max) {
                ::openxr_api_layer::log::Log(fmt::format(
                    "xr_telemetry: {} instance buffer request {} exceeds "
                    "cap {} — bailing\n",
                    m_debugName, desired, m_max));
                return false;
            }
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth      = next * m_instanceSize;
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            Microsoft::WRL::ComPtr<ID3D11Buffer> buf;
            if (FAILED(m_device->CreateBuffer(
                    &bd, nullptr, buf.GetAddressOf()))) {
                return false;
            }
            m_buffer.Swap(buf);
            m_capacity = next;
            return true;
        }

        Microsoft::WRL::ComPtr<ID3D11Device>        m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;
        Microsoft::WRL::ComPtr<ID3D11Buffer>        m_buffer;
        UINT        m_capacity     = 0;   // instances the buffer can hold
        UINT        m_instanceSize = 0;   // bytes per instance
        UINT        m_initial      = 0;   // initial / minimum capacity
        UINT        m_max          = 0;   // hard cap (runaway-growth guard)
        const char* m_debugName    = "InstancedBatch";
    };

}   // namespace openxr_api_layer::utils
