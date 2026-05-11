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

// Minimal in-process "OpenXR runtime" used by the integration tests. This
// stand-in replaces the loader + driver + compositor with a dozen C functions
// that record what the layer submitted and let us hand back controllable
// return values. We wire it in by setting the layer's m_xrGetInstanceProcAddr
// to &mock::xrGetInstanceProcAddr and then exercising the OpenXrApi*
// virtuals directly.

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>

#include <cstdint>
#include <vector>

namespace mock {

    // What the "runtime" exposes and records across a test. Reset()ed between
    // TEST_CASEs. Kept as plain public fields so tests can both configure
    // inputs (before calling the layer) and assert on recorded outputs.
    struct State {
        // ---- Configurable inputs (what the next-runtime returns) ------------

        // xrEnumerateViewConfigurationViews returns these for each view.
        uint32_t recommendedWidth = 2000;
        uint32_t recommendedHeight = 2200;
        uint32_t viewCount = 2;

        // xrLocateViews returns these as the rendered FOVs. Indexed by view.
        // Defaults to a roughly-symmetric stereo HMD at ~±50° / ±40°.
        std::vector<XrFovf> locateFovs;

        // ---- Recorded outputs (what the layer submitted downstream) ---------

        // Last xrEndFrame payload that reached the mock runtime, deep-copied
        // so we can inspect it after the layer's xrEndFrame returns. We copy
        // the projection views out because the layer's modifiedViewsArrays
        // lifetime ends with the xrEndFrame call.
        struct RecordedProjLayer {
            std::vector<XrCompositionLayerProjectionView> views;
        };
        std::vector<RecordedProjLayer> lastEndFrameProjLayers;
        uint32_t endFrameCallCount = 0;

        // ---- Fake handle plumbing -------------------------------------------

        // The layer never dereferences handles — they're opaque pointers to
        // the runtime as far as it's concerned. We just hand out monotonically
        // increasing values cast to the right type so that equality checks in
        // the layer's internal maps (swapchain -> createInfo) behave.
        uint64_t nextHandle = 0x1000;
    };

    // Singleton: exactly one mock runtime per test process. Each TEST_CASE
    // calls reset() to return it to a known state.
    State& state();
    void reset();

    // Entry point we plug into OpenXrApi::m_xrGetInstanceProcAddr. Resolves
    // every xr* function the layer actually asks for to a mock implementation
    // in this file. Returns XR_ERROR_FUNCTION_UNSUPPORTED for anything we
    // haven't implemented so the layer's failure paths are exercised instead
    // of silently no-oping.
    XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance,
                                              const char* name,
                                              PFN_xrVoidFunction* function);

} // namespace mock
