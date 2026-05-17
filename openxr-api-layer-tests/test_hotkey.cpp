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
// test_hotkey.cpp — unit tests on the hotkey parser (vkFromName + parse
// Hotkey + formatHotkey) and the rising-edge detector. Drives them with
// synthetic inputs so the test binary stays portable (no GetAsyncKeyState).
// =============================================================================

#include <doctest/doctest.h>

#include "utils/hotkey.h"

using openxr_api_layer::detail::HotkeySpec;
using openxr_api_layer::detail::HotkeyEdgeDetector;
using openxr_api_layer::detail::vkFromName;
using openxr_api_layer::detail::parseHotkey;
using openxr_api_layer::detail::formatHotkey;
using openxr_api_layer::detail::iequalsAscii;

// =============================================================================
// vkFromName — name → VK code resolution.
// =============================================================================

TEST_CASE("vkFromName: empty returns 0") {
    CHECK(vkFromName("") == 0);
}

TEST_CASE("vkFromName: single-letter case-insensitive maps to uppercase VK code") {
    // VK_A == 0x41 == 'A'.
    CHECK(vkFromName("A") == 0x41);
    CHECK(vkFromName("a") == 0x41);
    CHECK(vkFromName("Z") == 0x5A);
    CHECK(vkFromName("z") == 0x5A);
}

TEST_CASE("vkFromName: digits map to '0'..'9' ASCII") {
    CHECK(vkFromName("0") == 0x30);
    CHECK(vkFromName("9") == 0x39);
}

TEST_CASE("vkFromName: F1..F24") {
    CHECK(vkFromName("F1") == 0x70);
    CHECK(vkFromName("F11") == 0x7A);
    CHECK(vkFromName("F24") == 0x87);
    // Mixed case.
    CHECK(vkFromName("f1") == 0x70);
}

TEST_CASE("vkFromName: F0 and F25 are not valid") {
    CHECK(vkFromName("F0") == 0);
    CHECK(vkFromName("F25") == 0);
}

TEST_CASE("vkFromName: named keys (case-insensitive)") {
    CHECK(vkFromName("Space") == 0x20);
    CHECK(vkFromName("SPACE") == 0x20);
    CHECK(vkFromName("Enter") == 0x0D);
    CHECK(vkFromName("Return") == 0x0D);
    CHECK(vkFromName("Esc") == 0x1B);
    CHECK(vkFromName("Escape") == 0x1B);
    CHECK(vkFromName("PageUp") == 0x21);
    CHECK(vkFromName("pgdn") == 0x22);
    CHECK(vkFromName("Up") == 0x26);
}

TEST_CASE("vkFromName: unknown name returns 0") {
    CHECK(vkFromName("NotAKey") == 0);
    CHECK(vkFromName(";") == 0);              // punctuation deliberately rejected
    CHECK(vkFromName("Win") == 0);            // modifier, not a key
}

// =============================================================================
// parseHotkey — full block parse.
// =============================================================================

TEST_CASE("parseHotkey: default Ctrl+Shift+T (the shipped template)") {
    const auto spec = parseHotkey("T", {"ctrl", "shift"});
    REQUIRE(spec.valid());
    CHECK(spec.vk == 0x54);                   // 'T'
    CHECK(spec.ctrl);
    CHECK(spec.shift);
    CHECK_FALSE(spec.alt);
    CHECK_FALSE(spec.win);
}

TEST_CASE("parseHotkey: empty modifier list = bare key") {
    const auto spec = parseHotkey("F11", {});
    REQUIRE(spec.valid());
    CHECK(spec.vk == 0x7A);
    CHECK_FALSE(spec.ctrl);
    CHECK_FALSE(spec.shift);
    CHECK_FALSE(spec.alt);
    CHECK_FALSE(spec.win);
}

TEST_CASE("parseHotkey: modifier aliases all resolve") {
    CHECK(parseHotkey("T", {"control"}).ctrl);
    CHECK(parseHotkey("T", {"windows"}).win);
    CHECK(parseHotkey("T", {"cmd"}).win);
}

TEST_CASE("parseHotkey: unknown modifier silently ignored, key still parses") {
    const auto spec = parseHotkey("T", {"meta", "shift"});
    REQUIRE(spec.valid());
    CHECK(spec.vk == 0x54);
    CHECK(spec.shift);
    CHECK_FALSE(spec.ctrl);  // 'meta' is not recognised; only shift takes effect
}

TEST_CASE("parseHotkey: unknown key → invalid spec") {
    const auto spec = parseHotkey("NotAKey", {"ctrl"});
    CHECK_FALSE(spec.valid());
    CHECK(spec.vk == 0);
    // Modifiers still parse, but the spec is unusable.
    CHECK(spec.ctrl);
}

// =============================================================================
// formatHotkey — human-readable label round-trip.
// =============================================================================

TEST_CASE("formatHotkey: invalid spec renders <none>") {
    HotkeySpec invalid;
    CHECK(formatHotkey(invalid) == "<none>");
}

TEST_CASE("formatHotkey: Ctrl+Shift+T") {
    const auto spec = parseHotkey("T", {"ctrl", "shift"});
    CHECK(formatHotkey(spec) == "Ctrl+Shift+T");
}

TEST_CASE("formatHotkey: stable modifier ordering (Ctrl → Shift → Alt → Win)") {
    // User wrote modifiers in a weird order; output is canonicalised.
    const auto spec = parseHotkey("F5", {"alt", "ctrl", "win"});
    CHECK(formatHotkey(spec) == "Ctrl+Alt+Win+F5");
}

TEST_CASE("formatHotkey: named keys round-trip") {
    CHECK(formatHotkey(parseHotkey("Space", {})) == "Space");
    CHECK(formatHotkey(parseHotkey("PageUp", {"ctrl"})) == "Ctrl+PageUp");
}

// =============================================================================
// HotkeyEdgeDetector — rising-edge latch.
// =============================================================================

TEST_CASE("HotkeyEdgeDetector: idle, never pressed") {
    HotkeyEdgeDetector ed;
    CHECK_FALSE(ed.tick(false));
    CHECK_FALSE(ed.tick(false));
    CHECK_FALSE(ed.tick(false));
}

TEST_CASE("HotkeyEdgeDetector: single press fires exactly once") {
    HotkeyEdgeDetector ed;
    CHECK(ed.tick(true));         // rising edge
    CHECK_FALSE(ed.tick(true));   // still held
    CHECK_FALSE(ed.tick(true));
    CHECK_FALSE(ed.tick(false));  // release
}

TEST_CASE("HotkeyEdgeDetector: re-press after release fires again") {
    HotkeyEdgeDetector ed;
    CHECK(ed.tick(true));         // first rising
    CHECK_FALSE(ed.tick(false));
    CHECK(ed.tick(true));         // second rising
    CHECK_FALSE(ed.tick(false));
}

// =============================================================================
// iequalsAscii — sanity check on the case-fold helper.
// =============================================================================

TEST_CASE("iequalsAscii: ASCII case folding") {
    CHECK(iequalsAscii("ctrl", "CTRL"));
    CHECK(iequalsAscii("Shift", "shift"));
    CHECK_FALSE(iequalsAscii("ctrl", "ctrol"));
    CHECK_FALSE(iequalsAscii("ctrl", ""));
}
