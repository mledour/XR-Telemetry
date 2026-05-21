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
// for the overlay quad, the DirectWrite/D2D resources that paint text into
// it, and the histogram rings that drive the mini bars.
//
// Two concrete implementations live in overlay_renderer.cpp:
//   - D3D11OverlayRenderer: app uses D3D11 → we paint directly with D2D.
//   - D3D12OverlayRenderer: app uses D3D12 → we bridge via D3D11On12 so
//     D2D (which is D3D11-only) can still paint into the D3D12 swapchain.
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
#include "settings.h"

#include <memory>
#include <string>

namespace openxr_api_layer {
    class OpenXrApi;
}

// Forward declaration so consumers of this header (layer.cpp etc.) don't
// have to drag in <d2d1.h> just because the test entry point below takes
// an ID2D1RenderTarget*. pch.h pulls D3D11/D3D12 but not D2D — D2D only
// lives in overlay_renderer.cpp and the test TU. A bare struct fwd decl
// is enough for the pointer-parameter signature; the implementation in
// the .cpp sees the real definition via <d2d1.h>.
struct ID2D1RenderTarget;

namespace openxr_api_layer::detail {

    // Abstract renderer interface. Holds the lifecycle the layer drives:
    //   1. ctor: creates D2D / DirectWrite + the OpenXR swapchain.
    //            If init fails (BGRA8 unsupported, D3D resource fail),
    //            isReady() returns false and the rest of the API is a
    //            no-op. The caller logs and degrades.
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

        // True once the constructor succeeded — D2D, DirectWrite, the
        // OpenXR swapchain, and all the brushes are alive. False if
        // init failed (e.g. BGRA8 unsupported by the runtime). Caller
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
    //
    // `path` controls whether the renderer paints DIRECTLY into the
    // OpenXR swapchain image (skips a per-frame cross-device copy + 2
    // keyed-mutex acquires) or through a SHIM texture on a private
    // D3D11 device (the conservative default that works on every app).
    // Default Auto behaviour: try Direct, silently fall back to Shim if
    // the app's device is missing D3D11_CREATE_DEVICE_BGRA_SUPPORT or
    // the swapchain image isn't D2D-compatible. See settings.h
    // OverlayRendererPath for the full semantics.
    std::unique_ptr<OverlayRenderer> makeD3D11OverlayRenderer(
        OpenXrApi* api, XrSession session, ID3D11Device* device,
        OverlayRendererPath path = OverlayRendererPath::Auto);

    // Factory: app uses D3D12. We bridge via D3D11On12 (the wrapper that
    // exposes the D3D12 device as an ID3D11Device so D2D can paint into
    // its textures). `device` + `queue` come from XrGraphicsBinding
    // D3D12KHR. Returns nullptr if init fails.
    std::unique_ptr<OverlayRenderer> makeD3D12OverlayRenderer(
        OpenXrApi* api, XrSession session,
        ID3D12Device* device, ID3D12CommandQueue* queue);

    // -------- Snapshot / test entry point ---------------------------------
    //
    // Renders the overlay HUD to an arbitrary ID2D1RenderTarget — useful
    // for visual-regression tests (a WIC bitmap RT in a CI tool produces
    // a PNG that can be diffed against a golden image) and for offline
    // preview generation.
    //
    // Same paint pipeline as the in-game renderers, just decoupled from
    // OpenXR swapchain plumbing. Caller is responsible for the render
    // target's lifecycle (Begin/EndDraw not required — this function
    // handles them internally), but the brushes / text formats are
    // allocated fresh on every call so the function is suitable only
    // for one-shot rendering (NOT per-frame). Returns true on success.
    //
    // The histogram ring template parameter must match the in-engine
    // kRingSize. We expose that as kOverlayHistoRingSize here so test
    // code can declare matching rings without hard-coding the value
    // (and the cpp's static_assert below guards against drift).
    constexpr std::size_t kOverlayHistoRingSize = 120;

    // errOut: optional. On false return, populated with a short string
    // identifying which step failed (init / initBrushes / paint). Used
    // by the snapshot test to surface a useful failure message via
    // doctest INFO(); production callers leave it null.
    bool renderOverlayToTarget(
        ID2D1RenderTarget* rt,
        const OverlaySnapshot& snap,
        const HistogramRing<kOverlayHistoRingSize>& cpuRing,
        const HistogramRing<kOverlayHistoRingSize>& gpuRing,
        std::string* errOut = nullptr);

} // namespace openxr_api_layer::detail
