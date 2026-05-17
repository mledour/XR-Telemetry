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

// Pure string-to-filename helpers shared between the layer DLL and the
// standalone test binary. Header-only so neither side needs to link the
// other.
//
// Same helpers as in the fov_crop sibling layer — duplicated here on
// purpose so xr_telemetry can be cloned, built, and used standalone
// without dragging fov_crop into the dependency surface. If you change
// the rules, change both copies (search for sanitizeForFilename across
// the repo).

#include <filesystem>
#include <string>

namespace openxr_api_layer {

    // Turns a free-form OpenXR application name (e.g. "DiRT Rally 2.0",
    // "Le Mans Ultimate", "hello_xr") into a lowercase, filesystem-safe
    // slug suitable as a filename prefix. Rules:
    //   - uppercase ASCII is lowercased
    //   - any non-[a-z0-9] char becomes '_'
    //   - consecutive '_' are collapsed to one
    //   - trailing '_' is trimmed
    //   - empty or all-non-alphanumeric input yields "unknown_app"
    inline std::string sanitizeForFilename(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        bool lastWasSep = true; // treat start as separator to strip leading '_'
        for (char c : raw) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            const bool isAlnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (isAlnum) {
                out.push_back(c);
                lastWasSep = false;
            } else if (!lastWasSep) {
                out.push_back('_');
                lastWasSep = true;
            }
        }
        while (!out.empty() && out.back() == '_') out.pop_back();
        if (out.empty()) out = "unknown_app";
        return out;
    }

    // Resolves the path to the per-app settings file for the given raw
    // application name. Puts it alongside the global settings.json template
    // in the given config directory (typically localAppData\<layer-name>\).
    inline std::filesystem::path resolvePerAppConfigPath(const std::filesystem::path& configDir,
                                                         const std::string& appName) {
        return configDir / (sanitizeForFilename(appName) + "_settings.json");
    }

} // namespace openxr_api_layer
