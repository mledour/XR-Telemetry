// MIT License
//
// Copyright (c) <<YEAR>> <<AUTHOR_NAME>>
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
// test_example.cpp — minimal example so you can see the test loop runs.
//
// Replace / delete this file once you have real tests. Recommended layout:
//
//   - test_<your_helper>.cpp        unit tests on pure helpers (your
//                                   crop math, your config parser, etc.).
//                                   Just #include the header and assert.
//                                   No mock runtime needed.
//
//   - test_integration.cpp          end-to-end tests that drive the
//                                   OpenXrLayer class through the
//                                   mock_runtime — useful for things
//                                   that touch xrLocateViews,
//                                   xrEnumerateViewConfigurationViews,
//                                   xrEndFrame, etc.
//
// The doctest framework is included via the pre-built doctest.h under
// external/doctest/. main.cpp is the entry point; this file just
// contributes TEST_CASEs.
// =============================================================================

#include <doctest/doctest.h>

TEST_CASE("template: 1 + 1 == 2") {
    CHECK(1 + 1 == 2);
}

// Example of an integration-style test (commented out — uncomment and
// expand once your layer overrides actual OpenXR functions). The
// mock_runtime is a header-only OpenXR runtime that fakes
// xrCreateInstance, xrCreateSession, xrEnumerateViewConfigurationViews,
// xrLocateViews and xrEndFrame in-process — no real GPU, no real HMD,
// no loader.
//
// #include "mock_runtime.h"
//
// TEST_CASE("layer: pass-through xrCreateInstance keeps the runtime view of the instance valid") {
//     mock_runtime::MockRuntime mock;
//     mock_runtime::LayerHarness layer(&mock);
//
//     XrApplicationInfo appInfo{};
//     std::strcpy(appInfo.applicationName, "hello_xr");
//     appInfo.applicationVersion = 1;
//     std::strcpy(appInfo.engineName, "test");
//     appInfo.engineVersion = 1;
//     appInfo.apiVersion = XR_API_VERSION_1_0;
//
//     XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
//     info.applicationInfo = appInfo;
//
//     XrInstance instance = XR_NULL_HANDLE;
//     CHECK(layer.api()->xrCreateInstance(&info) == XR_SUCCESS);
//     CHECK(mock.lastApplicationName() == "hello_xr");
// }
