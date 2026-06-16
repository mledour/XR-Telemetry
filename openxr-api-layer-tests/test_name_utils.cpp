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

// =============================================================================
// test_name_utils.cpp — unit tests on sanitizeForFilename + resolvePerApp
// ConfigPath. The slug rules feed every per-app file lookup, so a regression
// would scatter telemetry across the wrong files on subsequent runs.
// =============================================================================

#include <doctest/doctest.h>

#include "utils/name_utils.h"

using openxr_api_layer::sanitizeForFilename;
using openxr_api_layer::resolvePerAppConfigPath;

TEST_CASE("sanitizeForFilename: typical mixed-case app name with spaces") {
    CHECK(sanitizeForFilename("DiRT Rally 2.0") == "dirt_rally_2_0");
}

TEST_CASE("sanitizeForFilename: already-clean snake_case stays the same") {
    CHECK(sanitizeForFilename("hello_xr") == "hello_xr");
}

TEST_CASE("sanitizeForFilename: collapses runs of separators") {
    CHECK(sanitizeForFilename("Le Mans  Ultimate") == "le_mans_ultimate");
    CHECK(sanitizeForFilename("A---B___C") == "a_b_c");
}

TEST_CASE("sanitizeForFilename: trims leading/trailing separators") {
    CHECK(sanitizeForFilename("  spaced  ") == "spaced");
    CHECK(sanitizeForFilename("__weird__") == "weird");
}

TEST_CASE("sanitizeForFilename: empty input falls back to unknown_app") {
    CHECK(sanitizeForFilename("") == "unknown_app");
    CHECK(sanitizeForFilename("   ") == "unknown_app");
    CHECK(sanitizeForFilename("!!!") == "unknown_app");
}

TEST_CASE("sanitizeForFilename: non-ASCII bytes become separators") {
    // We can't test UTF-8 round-trips here without dragging in ICU; the
    // contract is just "non-[a-z0-9] becomes _, with run-collapsing". A
    // hypothetical "Café" → 'C','a','f','\xC3','\xA9' → "caf" (the two
    // 0xC3/0xA9 bytes both fall into the separator branch).
    CHECK(sanitizeForFilename("Caf\xC3\xA9") == "caf");
}

TEST_CASE("resolvePerAppConfigPath: composes <dir>/<slug>_settings.json") {
    const std::filesystem::path dir = "C:\\Users\\me\\AppData\\Local\\xrt";
    const auto out = resolvePerAppConfigPath(dir, "DiRT Rally 2.0");
    CHECK(out.filename().string() == "dirt_rally_2_0_settings.json");
    CHECK(out.parent_path() == dir);
}
