// MIT License
//
// Copyright (c) 2026 Michael Ledour
//
// Based on https://github.com/mbucchia/OpenXR-Layer-Template.
// Copyright (c) 2022-2023 Matthieu Bucchianeri
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

// =============================================================================
// Layer skeleton — replace this with your own logic.
//
// This file is the only one most layer authors need to touch. The
// framework/ files generate the dispatch table, handle loader
// negotiation, and route every intercepted xr* call to the matching
// method on the OpenXrLayer class below. Anything you DON'T override
// is passed through to the next layer / runtime unchanged.
//
// The shipped skeleton overrides exactly one function (xrCreateInstance)
// and just logs the application's name + the active runtime. Useful as
// a sanity check that the layer is loading, and as a starting point.
//
// To add a feature:
//   1. Add the OpenXR function name to override_functions in
//      framework/layer_apis.py.
//   2. Rebuild — the dispatch generator regenerates dispatch.gen.{h,cpp}
//      with a virtual method for that function.
//   3. Override the method on the OpenXrLayer class below, do whatever
//      you want, and either return your own XrResult or call the base
//      class implementation to forward downstream.
// =============================================================================

#include "pch.h"

#include "layer.h"
#include <log.h>
#include <util.h>

namespace openxr_api_layer {

    using namespace log;

    // Extensions the layer cares about. Empty = pass-through, the
    // layer never appears in xrEnumerateApiLayerProperties' extension
    // contributions. See framework/entry.cpp for how these are
    // surfaced during loader negotiation.
    const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions = {};
    const std::vector<std::string> blockedExtensions = {};
    const std::vector<std::string> implicitExtensions = {};

    // ETW trace provider g_traceProvider is already declared by
    // framework/log.h and defined by framework/log.cpp with a fixed
    // GUID. Use TraceLoggingWrite(g_traceProvider, "<event-name>", ...)
    // from inside method bodies (NOT at namespace scope — TraceLoggingWrite
    // is a do/while statement macro). The .wprp profile in scripts\
    // captures the matching GUID for Windows Performance Recorder.

    class OpenXrLayer : public OpenXrApi {
    public:
        OpenXrLayer() {
            // Cache QPC frequency once. Used by every frame-thread timestamp
            // conversion below; constant for the process lifetime.
            LARGE_INTEGER freq{};
            QueryPerformanceFrequency(&freq);
            m_qpcFrequency = freq.QuadPart > 0 ? freq.QuadPart : 1;
        }
        ~OpenXrLayer() override = default;

        // ---- xrCreateInstance ---------------------------------------------
        // First call the loader hands to a freshly-loaded layer. The
        // baseclass implementation forwards to the next layer / runtime,
        // returning a real XrInstance handle we can then query for
        // identity (application name, runtime name+version).
        //
        // IMPORTANT: do NOT add 'xrCreateInstance' to override_functions
        // in framework/layer_apis.py — the framework already routes this
        // call to whatever virtual method you declare here. Listing it
        // there causes dispatch_generator.py to abort the build. Same
        // rule applies to xrDestroyInstance, xrGetInstanceProcAddr, and
        // xrEnumerateInstanceExtensionProperties.
        //
        // The "log application + runtime, then bypass if we don't
        // recognize anything" pattern is the recommended starting
        // shape — your layer can decide here whether it should run for
        // this process at all. CLAUDE.md's rule 9 (graceful degradation)
        // is enforced by setting m_bypassApiLayer when you want the
        // layer to become a no-op for an unsupported app/runtime.
        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateInstance
        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider, "xrCreateInstance");

            // Names the loader hands us — application as declared in
            // XrApplicationInfo, plus the runtime that materialised
            // beneath us once the baseclass returns.
            const std::string appName = createInfo->applicationInfo.applicationName;
            Log(fmt::format("xr_telemetry {} starting for application '{}'\n",
                             VersionString, appName));

            const XrResult result = OpenXrApi::xrCreateInstance(createInfo);
            if (XR_FAILED(result)) {
                return result;
            }

            // Runtime identity. Useful in logs when supporting multiple
            // runtimes — anti-cheat reports often reference the runtime
            // by name. Skip gracefully if the runtime doesn't fill in
            // properties for any reason; we never want to fail the
            // host's xrCreateInstance because of our own diagnostics.
            XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
            if (XR_SUCCEEDED(OpenXrApi::xrGetInstanceProperties(GetXrInstance(), &instanceProperties))) {
                Log(fmt::format("Runtime: {} {}.{}.{}\n",
                                 instanceProperties.runtimeName,
                                 XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                 XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                 XR_VERSION_PATCH(instanceProperties.runtimeVersion)));
            }

            // TODO(xr_telemetry): Decide here whether your layer
            // should be active for this app/runtime combination. If
            // not, set m_bypassApiLayer = true to make every other
            // override below a no-op pass-through. See CLAUDE.md
            // rule 9 — never crash the host because your layer can't
            // do its job; degrade to bypass instead.

            return result;
        }

        // ---- xrWaitFrame --------------------------------------------------
        // Pure observation; never mutates frameWaitInfo or frameState. The
        // QPC samples around the base call let us derive wait_block_ns (how
        // long the runtime made us wait — high = compositor had headroom).
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrWaitFrame
        XrResult xrWaitFrame(XrSession session,
                             const XrFrameWaitInfo* frameWaitInfo,
                             XrFrameState* frameState) override {
            const int64_t tWaitIn = QpcNow();
            const XrResult result = OpenXrApi::xrWaitFrame(session, frameWaitInfo, frameState);
            const int64_t tWaitOut = QpcNow();

            if (XR_SUCCEEDED(result) && frameState) {
                m_tWaitIn.store(tWaitIn, std::memory_order_relaxed);
                m_tWaitOut.store(tWaitOut, std::memory_order_relaxed);
                m_predictedPeriodNs.store(frameState->predictedDisplayPeriod, std::memory_order_relaxed);
                m_lastShouldRender.store(frameState->shouldRender == XR_TRUE, std::memory_order_relaxed);
            }
            return result;
        }

        // ---- xrBeginFrame -------------------------------------------------
        // Captured for completeness (begin→end split is useful in trace
        // analysis to separate "wait→begin housekeeping" from "render
        // submission") but not used in the headroom math itself.
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrBeginFrame
        XrResult xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) override {
            const int64_t tBegin = QpcNow();
            const XrResult result = OpenXrApi::xrBeginFrame(session, frameBeginInfo);
            if (XR_SUCCEEDED(result)) {
                m_tBegin.store(tBegin, std::memory_order_relaxed);
            }
            return result;
        }

        // ---- xrEndFrame ---------------------------------------------------
        // Closes the per-frame timing window. Samples tEnd BEFORE forwarding
        // so the measurement matches the moment the app committed the frame,
        // not the runtime's compositor work that follows.
        //
        // IMPORTANT: do NOT call Log() / fmt::format / OutputDebugStringA
        // from this path, not even gated to every Nth frame. OutputDebugString
        // takes the kernel-wide DBWinMutex; the Pimax OpenXR compositor (in
        // a separate process) acquires the same mutex for its own logging
        // and a few-hundred-µs contention spike here is enough to make the
        // compositor drop frames — manifesting as a black HMD. We discovered
        // this by bisection: math + ETW = renders, math + ETW + 1Hz Log = no
        // pixels in HMD. ETW (TraceLoggingWrite) is lock-free and safe.
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrEndFrame
        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            const int64_t tEnd = QpcNow();
            const XrResult result = OpenXrApi::xrEndFrame(session, frameEndInfo);

            const int64_t tWaitIn = m_tWaitIn.load(std::memory_order_relaxed);
            const int64_t tWaitOut = m_tWaitOut.load(std::memory_order_relaxed);
            const int64_t periodNs = m_predictedPeriodNs.load(std::memory_order_relaxed);
            const bool shouldRender = m_lastShouldRender.load(std::memory_order_relaxed);

            // First frame guard: xrEndFrame can fire before we ever observed
            // a successful xrWaitFrame. Skip the math to avoid bogus deltas.
            if (tWaitIn == 0 || tWaitOut == 0) {
                return result;
            }

            const int64_t waitBlockNs = QpcToNs(tWaitOut - tWaitIn);
            const int64_t appCpuNs = QpcToNs(tEnd - tWaitOut);

            // CPU headroom: fraction of the predicted display period the app
            // did NOT spend. Negative => app is CPU-bound this frame. Gated
            // on period > 0 because some runtimes briefly report 0 before
            // the session is fully ready.
            float headroomPct = 0.0f;
            if (periodNs > 0) {
                const double ratio = static_cast<double>(appCpuNs) / static_cast<double>(periodNs);
                headroomPct = static_cast<float>((1.0 - ratio) * 100.0);
            }
            const uint64_t frameIndex = m_frameIndex.fetch_add(1, std::memory_order_relaxed);

            // Per-frame ETW event. Lock-free; the canonical output of the
            // layer. Capture with scripts\Tracing.wprp + WPA/PerfView.
            TraceLoggingWrite(g_traceProvider,
                              "FrameMetrics",
                              TLArg(frameIndex, "frame"),
                              TLArg(waitBlockNs, "wait_block_ns"),
                              TLArg(appCpuNs, "app_cpu_ns"),
                              TLArg(periodNs, "period_ns"),
                              TLArg(headroomPct, "headroom_pct"),
                              TLArg(shouldRender, "should_render"));

            return result;
        }

        // ----------------------------------------------------------------
        // EXAMPLE OVERRIDES — uncomment + add the function name to
        // framework/layer_apis.py's override_functions list to wire
        // them up. The framework will regenerate dispatch.gen.{h,cpp}
        // on the next build to expose the matching virtual method.
        //
        //   XrResult xrLocateViews(XrSession session,
        //                          const XrViewLocateInfo* viewLocateInfo,
        //                          XrViewState* viewState,
        //                          uint32_t viewCapacityInput,
        //                          uint32_t* viewCountOutput,
        //                          XrView* views) override {
        //       const XrResult result = OpenXrApi::xrLocateViews(
        //           session, viewLocateInfo, viewState,
        //           viewCapacityInput, viewCountOutput, views);
        //       if (XR_SUCCEEDED(result) && !m_bypassApiLayer && views) {
        //           // ... mutate views[i].fov / views[i].pose per your
        //           // feature here ...
        //       }
        //       return result;
        //   }
        // ----------------------------------------------------------------

      private:
        // QPC helpers — QueryPerformanceCounter is monotonic, sub-microsecond,
        // and free of kernel transitions on modern Windows. We do NOT use
        // XrFrameState.predictedDisplayTime as a clock: it's a predicted
        // display point, not a measurement point, and its mapping to QPC is
        // runtime-defined.
        int64_t QpcNow() const {
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            return c.QuadPart;
        }
        // Convert QPC ticks to nanoseconds without overflowing on long runs:
        // split into whole-seconds + remainder.
        int64_t QpcToNs(int64_t ticks) const {
            const int64_t freq = m_qpcFrequency;
            const int64_t whole = ticks / freq;
            const int64_t rem = ticks % freq;
            return whole * 1'000'000'000LL + (rem * 1'000'000'000LL) / freq;
        }

        bool m_bypassApiLayer = false;
        int64_t m_qpcFrequency = 1;

        // Frame timing state — written from Wait/Begin thread, read from End
        // thread (may differ under OpenComposite where sim and render threads
        // are separate). Relaxed atomics are sufficient because the OpenXR
        // frame-loop contract guarantees at most one frame in flight between
        // Wait and End, so no field is concurrently written.
        std::atomic<int64_t> m_tWaitIn{0};
        std::atomic<int64_t> m_tWaitOut{0};
        std::atomic<int64_t> m_tBegin{0};
        std::atomic<int64_t> m_predictedPeriodNs{0};
        std::atomic<bool> m_lastShouldRender{true};
        std::atomic<uint64_t> m_frameIndex{0};
    };

    // Singleton accessor used by framework/dispatch.cpp.
    OpenXrApi* GetInstance() {
        static std::unique_ptr<OpenXrLayer> instance = std::make_unique<OpenXrLayer>();
        return instance.get();
    }

    // dllHome / localAppData are defined in framework/entry.cpp for the
    // DLL build, and in openxr-api-layer-tests/test_stubs.cpp for the
    // standalone test binary. Their declarations live in layer.h so
    // every TU that needs them just includes that.

} // namespace openxr_api_layer
