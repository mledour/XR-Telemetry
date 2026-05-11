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

// Replacements for the symbols that framework/entry.cpp normally provides.
// The real entry.cpp also exposes xrNegotiateLoaderApiLayerInterface as a DLL
// export (via module.def), which we cannot and do not want to link into a
// console test binary. This file provides only the data definitions that
// layer.cpp and log.cpp reach for at runtime.

#include "pch.h"

namespace openxr_api_layer {
    // Looked up by layer.cpp::loadConfig. Tests set this to a temp directory
    // before calling the layer's xrCreateInstance.
    std::filesystem::path dllHome;
    std::filesystem::path localAppData;

    namespace log {
        // Referenced by log.cpp::InternalLog. Left closed so Log() only goes
        // to OutputDebugString — no spurious files under the CI workspace.
        std::ofstream logStream;
    } // namespace log
} // namespace openxr_api_layer
