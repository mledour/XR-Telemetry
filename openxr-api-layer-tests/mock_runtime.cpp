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

#include "mock_runtime.h"

#include <cstring>
#include <string_view>

namespace mock {

    namespace {

        State g_state;

        // Cast a fresh handle value. We ignore the unused XrInstance/XrSession
        // args in these mocks — we only care that the layer threads them back
        // to us unchanged via its per-handle maps.
        template <typename H>
        H makeHandle() {
            return reinterpret_cast<H>(static_cast<uintptr_t>(++g_state.nextHandle));
        }

        // -----------------------------------------------------------------
        // Mock implementations. All signatures match the OpenXR 1.0 spec.
        // -----------------------------------------------------------------

        XrResult XRAPI_CALL m_xrGetInstanceProperties(XrInstance /*instance*/,
                                                     XrInstanceProperties* props) {
            if (!props) return XR_ERROR_VALIDATION_FAILURE;
            props->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
            std::strncpy(props->runtimeName, "MockRuntime", XR_MAX_RUNTIME_NAME_SIZE - 1);
            props->runtimeName[XR_MAX_RUNTIME_NAME_SIZE - 1] = '\0';
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrGetSystemProperties(XrInstance /*instance*/,
                                                   XrSystemId /*systemId*/,
                                                   XrSystemProperties* props) {
            if (!props) return XR_ERROR_VALIDATION_FAILURE;
            std::strncpy(props->systemName, "MockHMD", XR_MAX_SYSTEM_NAME_SIZE - 1);
            props->systemName[XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrGetSystem(XrInstance /*instance*/,
                                         const XrSystemGetInfo* /*getInfo*/,
                                         XrSystemId* systemId) {
            if (!systemId) return XR_ERROR_VALIDATION_FAILURE;
            // XrSystemId is a uint64_t; use a non-null synthetic value.
            *systemId = static_cast<XrSystemId>(++g_state.nextHandle);
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrCreateSession(XrInstance /*instance*/,
                                             const XrSessionCreateInfo* /*createInfo*/,
                                             XrSession* session) {
            if (!session) return XR_ERROR_VALIDATION_FAILURE;
            *session = makeHandle<XrSession>();
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrDestroySession(XrSession /*session*/) {
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrDestroyInstance(XrInstance /*instance*/) {
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrEnumerateInstanceExtensionProperties(const char* /*layerName*/,
                                                                     uint32_t capacity,
                                                                     uint32_t* count,
                                                                     XrExtensionProperties* /*props*/) {
            if (!count) return XR_ERROR_VALIDATION_FAILURE;
            *count = 0;
            (void)capacity;
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrEnumerateViewConfigurationViews(XrInstance /*instance*/,
                                                                XrSystemId /*systemId*/,
                                                                XrViewConfigurationType /*type*/,
                                                                uint32_t capacity,
                                                                uint32_t* count,
                                                                XrViewConfigurationView* views) {
            if (!count) return XR_ERROR_VALIDATION_FAILURE;
            *count = g_state.viewCount;
            if (capacity == 0) return XR_SUCCESS;
            if (capacity < g_state.viewCount) return XR_ERROR_SIZE_INSUFFICIENT;
            for (uint32_t i = 0; i < g_state.viewCount; ++i) {
                views[i].recommendedImageRectWidth = g_state.recommendedWidth;
                views[i].recommendedImageRectHeight = g_state.recommendedHeight;
                views[i].maxImageRectWidth = g_state.recommendedWidth * 2;
                views[i].maxImageRectHeight = g_state.recommendedHeight * 2;
                views[i].recommendedSwapchainSampleCount = 1;
                views[i].maxSwapchainSampleCount = 1;
            }
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrLocateViews(XrSession /*session*/,
                                           const XrViewLocateInfo* /*locateInfo*/,
                                           XrViewState* viewState,
                                           uint32_t capacity,
                                           uint32_t* count,
                                           XrView* views) {
            if (!count) return XR_ERROR_VALIDATION_FAILURE;
            *count = g_state.viewCount;
            if (capacity == 0) return XR_SUCCESS;
            if (capacity < g_state.viewCount) return XR_ERROR_SIZE_INSUFFICIENT;
            if (viewState) {
                viewState->viewStateFlags =
                    XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
            }
            for (uint32_t i = 0; i < g_state.viewCount; ++i) {
                views[i].pose = XrPosef{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
                if (i < g_state.locateFovs.size()) {
                    views[i].fov = g_state.locateFovs[i];
                } else {
                    views[i].fov = XrFovf{-0.9f, 0.9f, 0.7f, -0.7f};
                }
            }
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrCreateSwapchain(XrSession /*session*/,
                                               const XrSwapchainCreateInfo* createInfo,
                                               XrSwapchain* swapchain) {
            if (!createInfo || !swapchain) return XR_ERROR_VALIDATION_FAILURE;
            *swapchain = makeHandle<XrSwapchain>();
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrDestroySwapchain(XrSwapchain /*swapchain*/) {
            return XR_SUCCESS;
        }

        // Helmet-overlay companion stubs. The integration tests never
        // enable the overlay, so none of these is actually exercised
        // end-to-end — they only need to resolve at layer init time.
        // Returning XR_ERROR_FUNCTION_UNSUPPORTED when the overlay path
        // does call them surfaces as a graceful degrade in the layer
        // (isArmed() stays false) rather than a silent null-pointer
        // call, matching how the layer treats hostile runtimes.

        XrResult XRAPI_CALL m_xrCreateReferenceSpace(XrSession /*session*/,
                                                    const XrReferenceSpaceCreateInfo* /*info*/,
                                                    XrSpace* space) {
            if (!space) return XR_ERROR_VALIDATION_FAILURE;
            *space = makeHandle<XrSpace>();
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrDestroySpace(XrSpace /*space*/) {
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrEnumerateSwapchainFormats(XrSession /*session*/,
                                                         uint32_t capacity,
                                                         uint32_t* count,
                                                         int64_t* formats) {
            if (!count) return XR_ERROR_VALIDATION_FAILURE;
            *count = 1;
            if (capacity == 0) return XR_SUCCESS;
            if (capacity < 1) return XR_ERROR_SIZE_INSUFFICIENT;
            // DXGI_FORMAT_R8G8B8A8_UNORM = 28. The real format value does
            // not matter here — the overlay code never runs in tests.
            formats[0] = 28;
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrEnumerateSwapchainImages(XrSwapchain /*swapchain*/,
                                                        uint32_t capacity,
                                                        uint32_t* count,
                                                        XrSwapchainImageBaseHeader* /*images*/) {
            if (!count) return XR_ERROR_VALIDATION_FAILURE;
            *count = 0;
            (void)capacity;
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrAcquireSwapchainImage(XrSwapchain /*swapchain*/,
                                                    const XrSwapchainImageAcquireInfo* /*info*/,
                                                    uint32_t* index) {
            if (!index) return XR_ERROR_VALIDATION_FAILURE;
            *index = 0;
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrWaitSwapchainImage(XrSwapchain /*swapchain*/,
                                                  const XrSwapchainImageWaitInfo* /*info*/) {
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrReleaseSwapchainImage(XrSwapchain /*swapchain*/,
                                                     const XrSwapchainImageReleaseInfo* /*info*/) {
            return XR_SUCCESS;
        }

        XrResult XRAPI_CALL m_xrEndFrame(XrSession /*session*/, const XrFrameEndInfo* info) {
            g_state.endFrameCallCount++;
            g_state.lastEndFrameProjLayers.clear();
            if (!info) return XR_SUCCESS;
            for (uint32_t i = 0; i < info->layerCount; ++i) {
                const auto* base = info->layers[i];
                if (!base) continue;
                if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(base);
                    State::RecordedProjLayer rec;
                    rec.views.assign(proj->views, proj->views + proj->viewCount);
                    g_state.lastEndFrameProjLayers.push_back(std::move(rec));
                }
            }
            return XR_SUCCESS;
        }

    } // namespace

    State& state() {
        return g_state;
    }

    void reset() {
        g_state = State{};
    }

    XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance /*instance*/,
                                              const char* name,
                                              PFN_xrVoidFunction* function) {
        if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
        const std::string_view n(name);

        // Dispatch table. Any function the layer asks for but we don't map
        // returns XR_ERROR_FUNCTION_UNSUPPORTED, which surfaces as a clear
        // failure instead of a silent null-pointer call later.
        if (n == "xrGetInstanceProperties")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrGetInstanceProperties);
        else if (n == "xrGetSystemProperties")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrGetSystemProperties);
        else if (n == "xrGetSystem")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrGetSystem);
        else if (n == "xrCreateSession")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrCreateSession);
        else if (n == "xrDestroySession")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrDestroySession);
        else if (n == "xrDestroyInstance")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrDestroyInstance);
        else if (n == "xrEnumerateInstanceExtensionProperties")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrEnumerateInstanceExtensionProperties);
        else if (n == "xrEnumerateViewConfigurationViews")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrEnumerateViewConfigurationViews);
        else if (n == "xrLocateViews")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrLocateViews);
        else if (n == "xrCreateSwapchain")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrCreateSwapchain);
        else if (n == "xrDestroySwapchain")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrDestroySwapchain);
        else if (n == "xrCreateReferenceSpace")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrCreateReferenceSpace);
        else if (n == "xrDestroySpace")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrDestroySpace);
        else if (n == "xrEnumerateSwapchainFormats")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrEnumerateSwapchainFormats);
        else if (n == "xrEnumerateSwapchainImages")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrEnumerateSwapchainImages);
        else if (n == "xrAcquireSwapchainImage")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrAcquireSwapchainImage);
        else if (n == "xrWaitSwapchainImage")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrWaitSwapchainImage);
        else if (n == "xrReleaseSwapchainImage")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrReleaseSwapchainImage);
        else if (n == "xrEndFrame")
            *function = reinterpret_cast<PFN_xrVoidFunction>(m_xrEndFrame);
        else {
            *function = nullptr;
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        return XR_SUCCESS;
    }

} // namespace mock
