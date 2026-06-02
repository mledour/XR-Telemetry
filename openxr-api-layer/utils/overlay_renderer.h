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

#pragma once

// =============================================================================
// overlay_renderer.h — in-headset HUD renderer. Owns the OpenXR swapchain
// for the overlay quad, the glyph atlas + GPU pipelines that paint into it,
// and the histogram rings that drive the mini bars.
//
// Two concrete implementations live in overlay_renderer.cpp:
//   - D3D11OverlayRenderer: app uses D3D11 → we paint directly into the
//     swapchain image via the chrome-shape / glyph-atlas / bar shaders.
//   - D3D12OverlayRenderer: app uses D3D12 → we bridge via D3D11On12 and
//     paint the same shaders directly into the wrapped swapchain image
//     (no private shim, no per-frame CopyResource).
//
// Callers (layer.cpp) get an abstract `OverlayRenderer` back from the
// matching factory and use it the same way for both paths.
//
// Vulkan / OpenGL hosts are NOT supported — the factories for those
// don't exist. The layer logs "overlay disabled for Vulkan/OpenGL
// hosts" and skips the renderer entirely; CSV writing still runs.
// =============================================================================

#include "pch.h"

#include "histogram_ring.h"
#include "overlay_aggregator.h"

#include <memory>
#include <string>

namespace openxr_api_layer {
    class OpenXrApi;
}

namespace openxr_api_layer::detail {

    // Abstract renderer interface. Holds the lifecycle the layer drives:
    //   1. ctor: bakes the glyph atlas (DirectWrite), creates the GPU
    //            pipelines (chrome shapes / glyph atlas / bars) + the
    //            OpenXR swapchain. If init fails (atlas bake or a
    //            pipeline init), isReady() returns false and the rest of
    //            the API is a no-op. The caller logs and degrades.
    //   2. pushFrameSample: per-frame, fed from fanoutRecord — adds
    //                       samples to the per-cycle CPU and GPU time
    //                       histogram rings (one per column of the
    //                       two-column fpsVR-style HUD).
    //   3. renderAndCompose: per-frame from xrEndFrame, paints the
    //                        latest snapshot into the next swapchain
    //                        image, returns a pointer to the
    //                        XrCompositionLayerBaseHeader the caller
    //                        appends to xrEndFrame's layer array. Pose
    //                        is computed from the view-space passed in
    //                        + the settings position/scale.
    //   4. dtor: tears down all D3D + OpenXR resources before the
    //            session's device handle disappears.
    class OverlayRenderer {
      public:
        virtual ~OverlayRenderer() = default;

        // True once the constructor succeeded — the glyph atlas, the
        // three GPU pipelines, and the OpenXR swapchain are alive.
        // False if init failed (atlas bake or a pipeline init). Caller
        // skips render calls in that case.
        virtual bool isReady() const noexcept = 0;

        // Feed one fully-resolved frame's metrics. Cheap (two
        // HistogramRing::push calls). Safe to call every frame even
        // when the overlay is currently hidden — the rings stay warm
        // so toggling the HUD back on shows recent history immediately.
        virtual void pushFrameSample(int64_t cpu_per_cycle_ns, int64_t gpu_time_ns) = 0;

        // Paint the snapshot into the next swapchain image and return
        // the composition layer to append to xrEndFrame. Returns nullptr
        // when nothing should be drawn this frame (snap.valid == false,
        // or any OpenXR / D3D call along the path failed).
        //
        // The returned pointer is owned by the renderer; it stays valid
        // until the next renderAndCompose() call or destruction.
        virtual const XrCompositionLayerBaseHeader* renderAndCompose(
            XrSpace viewSpace,
            const OverlaySnapshot& snap,
            const std::string& position,
            float scale) = 0;
    };

    // Factory: app uses D3D11 directly. `api` is the layer chain
    // (OpenXrApi*, dispatched through to the next layer / runtime) —
    // the renderer goes through it for xrCreateSwapchain, xrEnumerate
    // SwapchainImages, xrAcquireSwapchainImage, etc. `device` is the
    // binding device received via XrGraphicsBindingD3D11KHR in
    // xrCreateSession. Returns nullptr if anything in the init path
    // fails (the caller logs and degrades).
    std::unique_ptr<OverlayRenderer> makeD3D11OverlayRenderer(
        OpenXrApi* api, XrSession session, ID3D11Device* device);

    // Factory: app uses D3D12. We bridge via D3D11On12 (the wrapper that
    // exposes the D3D12 device as an ID3D11Device so our D3D11 shader
    // pipelines can paint into its textures). `device` + `queue` come from
    // XrGraphicsBinding
    // D3D12KHR. Returns nullptr if init fails.
    std::unique_ptr<OverlayRenderer> makeD3D12OverlayRenderer(
        OpenXrApi* api, XrSession session,
        ID3D12Device* device, ID3D12CommandQueue* queue);

    // -------- Snapshot / test entry point ---------------------------------
    //
    // The histogram ring template parameter must match the in-engine
    // kRingSize. We expose that as kOverlayHistoRingSize here so test
    // code can declare matching rings without hard-coding the value
    // (and the cpp's static_assert below guards against drift).
    //
    // 133 = the bar count that fills the GPU histogram strip exactly
    // with the fixed 4-px-bar / 1-px-gap layout: 133×4 + 132×1 = 664 px
    // (the strip's inner width). Picking it leaves zero margin and keeps
    // every bar pixel-aligned. ~133 samples ≈ 1.5 s @ 90 Hz.
    constexpr std::size_t kOverlayHistoRingSize = 133;

    // -------- GPU-path snapshot entry point (Task 18) ---------------------
    //
    // Renders the overlay HUD through the SAME GPU pipeline the in-headset
    // D3D11 path uses (chrome shapes + glyph atlas + instanced bars,
    // shaders sampled / blended on the GPU) into `target` — a caller-owned
    // BGRA8_UNORM texture sized kTexW × kTexH with D3D11_BIND_RENDER_TARGET.
    // The caller reads `target` back via a staging copy to obtain pixels
    // for the golden comparison. This is the snapshot coverage of the
    // actual shipping render path (the legacy D2D snapshot entry was
    // retired with the rest of the D2D layer).
    //
    // `device` may be any D3D11 device (BGRA_SUPPORT not required — the
    // GPU shaders render BGRA8 regardless). errOut: optional
    // failure-step string for a useful doctest message on failure.
    bool renderOverlayToTextureD3D11(
        ID3D11Device* device,
        ID3D11DeviceContext* ctx,
        ID3D11Texture2D* target,
        const OverlaySnapshot& snap,
        const HistogramRing<kOverlayHistoRingSize>& cpuRing,
        const HistogramRing<kOverlayHistoRingSize>& gpuRing,
        std::string* errOut = nullptr);

} // namespace openxr_api_layer::detail
