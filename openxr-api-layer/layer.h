// MIT License
//
// Copyright (c) <<YEAR>> <<AUTHOR_NAME>>
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

#pragma once

// The layer's public surface to the framework. Anything you need to
// expose to framework/entry.cpp or to your own test binary lives
// here; layer-specific helpers stay private inside layer.cpp's
// anonymous namespace.

#include "framework/dispatch.gen.h"

// version.h is generated at pre-build by scripts\Generate-VersionRc.ps1
// from the adjacent version.h.in template. It defines LAYER_VERSION
// with the same value that feeds the DLL's VERSIONINFO resource, so
// the version shown in logs always matches the version shown in
// File Explorer properties.
#include "version.h"

namespace openxr_api_layer {

    const std::string LayerName = LAYER_NAME;
    const std::string VersionString = LAYER_VERSION;

    // Singleton accessor used by framework/dispatch.cpp to dispatch
    // intercepted calls to your OpenXrLayer subclass.
    OpenXrApi* GetInstance();

    // The path where the DLL is loaded from (e.g. to find a data file
    // shipped next to the DLL in the install directory).
    extern std::filesystem::path dllHome;

    // A user-writable path (typically
    // %LOCALAPPDATA%\<layer-name>\) — good place to put logs,
    // per-app settings files, or anything else the layer mutates at
    // runtime.
    extern std::filesystem::path localAppData;

    // Extensions the layer advertises / blocks / implicitly enables.
    // See framework/entry.cpp for how these are surfaced to the OpenXR
    // loader during negotiation. Leaving them empty (the default in
    // this template) means the layer is a pure pass-through w.r.t.
    // extension visibility — recommended for a starting layer.
    extern const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions;
    extern const std::vector<std::string> blockedExtensions;
    extern const std::vector<std::string> implicitExtensions;

} // namespace openxr_api_layer
