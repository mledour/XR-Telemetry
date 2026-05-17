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
// test_settings.cpp — unit tests on parseSettings. Covers the happy path,
// the "missing fields → defaults" robustness contract, and the malformed-
// input failure modes. Since parseSettings is the only path between JSON
// text and the layer's runtime decision tree, every branch in here directly
// maps to "what happens if a user's settings.json looks like X".
// =============================================================================

#include <doctest/doctest.h>

#include "utils/settings.h"

using openxr_api_layer::detail::parseSettings;
using openxr_api_layer::detail::ParsedSettings;
using openxr_api_layer::detail::LogMode;
using openxr_api_layer::detail::OverlayMode;
using openxr_api_layer::detail::defaultHotkey;
using openxr_api_layer::detail::defaultOverlayHotkey;
using openxr_api_layer::detail::formatHotkey;

namespace {
    // The defaults documented in installer/default_settings.json:
    // log.enabled=true, log.mode=auto, log.hotkey=Ctrl+Shift+T,
    // overlay.enabled=false, overlay.mode=auto, overlay.hotkey=Ctrl+Shift+O,
    // overlay.refresh_hz=10, overlay.position=head_top_right.
    void checkDocumentedDefaults(const ParsedSettings& p) {
        CHECK(p.error.empty());
        CHECK(p.settings.log.enabled);
        CHECK(p.settings.log.mode == LogMode::Auto);
        CHECK(formatHotkey(p.settings.log.hotkey) == "Ctrl+Shift+T");
        CHECK_FALSE(p.settings.overlay.enabled);
        CHECK(p.settings.overlay.mode == OverlayMode::Auto);
        CHECK(formatHotkey(p.settings.overlay.hotkey) == "Ctrl+Shift+O");
        CHECK(p.settings.overlay.refresh_hz == 10);
        CHECK(p.settings.overlay.position == "head_top_right");
    }
}

// =============================================================================
// Defaults — everything missing must produce the "current behaviour" path.
// =============================================================================

TEST_CASE("parseSettings: empty input → documented defaults, no error") {
    checkDocumentedDefaults(parseSettings(""));
}

TEST_CASE("parseSettings: empty object → documented defaults, no error") {
    checkDocumentedDefaults(parseSettings("{}"));
}

TEST_CASE("parseSettings: comment-only object → documented defaults") {
    checkDocumentedDefaults(parseSettings(R"({ "_comment": "hi" })"));
}

// =============================================================================
// log block — happy paths.
// =============================================================================

TEST_CASE("parseSettings: full log block parses every field") {
    const auto p = parseSettings(R"({
        "log": {
            "enabled": true,
            "mode": "hotkey",
            "hotkey": { "key": "F11", "modifiers": ["ctrl", "alt"] }
        }
    })");
    REQUIRE(p.error.empty());
    CHECK(p.settings.log.enabled);
    CHECK(p.settings.log.mode == LogMode::Hotkey);
    CHECK(formatHotkey(p.settings.log.hotkey) == "Ctrl+Alt+F11");
}

TEST_CASE("parseSettings: log.enabled false propagates") {
    const auto p = parseSettings(R"({ "log": { "enabled": false } })");
    REQUIRE(p.error.empty());
    CHECK_FALSE(p.settings.log.enabled);
    // Other fields still defaulted.
    CHECK(p.settings.log.mode == LogMode::Auto);
}

TEST_CASE("parseSettings: mode strings are case-insensitive") {
    CHECK(parseSettings(R"({"log":{"mode":"HOTKEY"}})").settings.log.mode ==
          LogMode::Hotkey);
    CHECK(parseSettings(R"({"log":{"mode":"Auto"}})").settings.log.mode ==
          LogMode::Auto);
}

TEST_CASE("parseSettings: unknown mode falls back to auto (no error surfaced)") {
    const auto p = parseSettings(R"({ "log": { "mode": "nonsense" } })");
    CHECK(p.error.empty());
    CHECK(p.settings.log.mode == LogMode::Auto);
}

// =============================================================================
// hotkey sub-block defaults.
// =============================================================================

TEST_CASE("parseSettings: log present but hotkey omitted → default hotkey") {
    const auto p = parseSettings(R"({ "log": { "mode": "hotkey" } })");
    CHECK(p.error.empty());
    CHECK(formatHotkey(p.settings.log.hotkey) == "Ctrl+Shift+T");
}

TEST_CASE("parseSettings: hotkey with unknown key name falls back to default") {
    const auto p = parseSettings(R"({
        "log": { "hotkey": { "key": "NotAKey", "modifiers": ["ctrl"] } }
    })");
    CHECK(p.error.empty());
    // Default hotkey kicks in, not a partial parse of the user's modifiers.
    CHECK(formatHotkey(p.settings.log.hotkey) == "Ctrl+Shift+T");
}

TEST_CASE("parseSettings: bare key with no modifiers parses cleanly") {
    const auto p = parseSettings(R"({
        "log": { "hotkey": { "key": "F11", "modifiers": [] } }
    })");
    CHECK(p.error.empty());
    CHECK(formatHotkey(p.settings.log.hotkey) == "F11");
}

// =============================================================================
// Wrong types — fields that don't match their expected JSON type fall back
// to defaults silently. The robustness contract: a user who pasted a
// settings.json from a different layer doesn't crash xr_telemetry.
// =============================================================================

TEST_CASE("parseSettings: log.enabled with non-bool falls back to default") {
    const auto p = parseSettings(R"({ "log": { "enabled": "yes please" } })");
    CHECK(p.error.empty());
    CHECK(p.settings.log.enabled);  // default true
}

TEST_CASE("parseSettings: log.mode with non-string falls back to default") {
    const auto p = parseSettings(R"({ "log": { "mode": 42 } })");
    CHECK(p.error.empty());
    CHECK(p.settings.log.mode == LogMode::Auto);
}

TEST_CASE("parseSettings: hotkey block not an object → default hotkey, no error") {
    const auto p = parseSettings(R"({ "log": { "hotkey": "F11" } })");
    CHECK(p.error.empty());
    CHECK(formatHotkey(p.settings.log.hotkey) == "Ctrl+Shift+T");
}

TEST_CASE("parseSettings: modifiers array with non-strings filters silently") {
    const auto p = parseSettings(R"({
        "log": { "hotkey": { "key": "T", "modifiers": [42, "ctrl", null] } }
    })");
    CHECK(p.error.empty());
    // Only "ctrl" survives the filter.
    CHECK(formatHotkey(p.settings.log.hotkey) == "Ctrl+T");
}

// =============================================================================
// JSON-level failures — actual parse errors DO surface in `error`, but the
// returned settings struct is still the safe defaults so the caller can use
// it unconditionally.
// =============================================================================

TEST_CASE("parseSettings: malformed JSON surfaces error, settings = defaults") {
    const auto p = parseSettings(R"({"log": )");
    CHECK_FALSE(p.error.empty());
    CHECK(p.settings.log.enabled);
    CHECK(p.settings.log.mode == LogMode::Auto);
}

TEST_CASE("parseSettings: root is a JSON array, not an object") {
    const auto p = parseSettings(R"([1, 2, 3])");
    CHECK_FALSE(p.error.empty());
    CHECK(p.settings.log.enabled);
}

// =============================================================================
// overlay block — happy paths.
// =============================================================================

TEST_CASE("parseSettings: full overlay block parses every field") {
    const auto p = parseSettings(R"({
        "overlay": {
            "enabled": true,
            "mode": "hotkey",
            "hotkey": { "key": "F12", "modifiers": ["alt"] },
            "refresh_hz": 20,
            "position": "head_top_left"
        }
    })");
    REQUIRE(p.error.empty());
    CHECK(p.settings.overlay.enabled);
    CHECK(p.settings.overlay.mode == OverlayMode::Hotkey);
    CHECK(formatHotkey(p.settings.overlay.hotkey) == "Alt+F12");
    CHECK(p.settings.overlay.refresh_hz == 20);
    CHECK(p.settings.overlay.position == "head_top_left");
}

TEST_CASE("parseSettings: overlay defaults match the documented shipped template") {
    // Asserting defaults explicitly here so an accidental change to
    // defaults forces an update to BOTH the parser and the README.
    const auto p = parseSettings("{}");
    REQUIRE(p.error.empty());
    CHECK_FALSE(p.settings.overlay.enabled);
    CHECK(p.settings.overlay.mode == OverlayMode::Auto);
    CHECK(formatHotkey(p.settings.overlay.hotkey) == "Ctrl+Shift+O");
    CHECK(p.settings.overlay.refresh_hz == 10);
    CHECK(p.settings.overlay.position == "head_top_right");
}

TEST_CASE("parseSettings: overlay refresh_hz clamped to [1, 60]") {
    CHECK(parseSettings(R"({"overlay":{"refresh_hz":0}})").settings.overlay.refresh_hz == 1);
    CHECK(parseSettings(R"({"overlay":{"refresh_hz":-5}})").settings.overlay.refresh_hz == 1);
    CHECK(parseSettings(R"({"overlay":{"refresh_hz":300}})").settings.overlay.refresh_hz == 60);
    CHECK(parseSettings(R"({"overlay":{"refresh_hz":30}})").settings.overlay.refresh_hz == 30);
    // Exact boundary checks — 60 passes through, 61 clamps to 60, 1
    // is the lowest valid value. Locks the std::clamp(rawHz, 1, 60)
    // contract in case a future refactor changes the range.
    CHECK(parseSettings(R"({"overlay":{"refresh_hz":60}})").settings.overlay.refresh_hz == 60);
    CHECK(parseSettings(R"({"overlay":{"refresh_hz":61}})").settings.overlay.refresh_hz == 60);
    CHECK(parseSettings(R"({"overlay":{"refresh_hz":1}})").settings.overlay.refresh_hz == 1);
}

TEST_CASE("parseSettings: overlay refresh_hz accepts JSON floats too") {
    // A user pasting from another tool's config might write 10.0 instead
    // of 10 — the parser should still accept it.
    CHECK(parseSettings(R"({"overlay":{"refresh_hz":12.0}})").settings.overlay.refresh_hz == 12);
}

TEST_CASE("parseSettings: overlay mode strings are case-insensitive") {
    CHECK(parseSettings(R"({"overlay":{"mode":"HOTKEY"}})").settings.overlay.mode ==
          OverlayMode::Hotkey);
    CHECK(parseSettings(R"({"overlay":{"mode":"Auto"}})").settings.overlay.mode ==
          OverlayMode::Auto);
}

TEST_CASE("parseSettings: unknown overlay mode falls back to auto (no error surfaced)") {
    const auto p = parseSettings(R"({"overlay":{"mode":"whatever"}})");
    CHECK(p.error.empty());
    CHECK(p.settings.overlay.mode == OverlayMode::Auto);
}

TEST_CASE("parseSettings: overlay hotkey defaults to Ctrl+Shift+O when block missing") {
    const auto p = parseSettings(R"({"overlay":{"mode":"hotkey"}})");
    REQUIRE(p.error.empty());
    CHECK(formatHotkey(p.settings.overlay.hotkey) == "Ctrl+Shift+O");
}

TEST_CASE("parseSettings: overlay hotkey unknown key falls back to default") {
    const auto p = parseSettings(R"({
        "overlay": { "hotkey": { "key": "Squiggle", "modifiers": ["alt"] } }
    })");
    REQUIRE(p.error.empty());
    CHECK(formatHotkey(p.settings.overlay.hotkey) == "Ctrl+Shift+O");
}

TEST_CASE("parseSettings: overlay enabled with non-bool falls back to false default") {
    const auto p = parseSettings(R"({"overlay":{"enabled":"yes"}})");
    CHECK(p.error.empty());
    CHECK_FALSE(p.settings.overlay.enabled);
}

TEST_CASE("parseSettings: log and overlay blocks parsed independently") {
    // Common config: log on, overlay also on with custom hotkey. Both
    // blocks must read out cleanly without cross-contamination.
    const auto p = parseSettings(R"({
        "log":     { "enabled": true,  "mode": "auto",   "hotkey": {"key":"T","modifiers":["ctrl","shift"]} },
        "overlay": { "enabled": true,  "mode": "hotkey", "hotkey": {"key":"O","modifiers":["ctrl","shift"]}, "refresh_hz": 5 }
    })");
    REQUIRE(p.error.empty());
    CHECK(p.settings.log.enabled);
    CHECK(p.settings.log.mode == LogMode::Auto);
    CHECK(p.settings.overlay.enabled);
    CHECK(p.settings.overlay.mode == OverlayMode::Hotkey);
    CHECK(p.settings.overlay.refresh_hz == 5);
}

TEST_CASE("parseSettings: unknown top-level keys are tolerated") {
    // A user who tries random extensions should not get a parse error.
    const auto p = parseSettings(R"({
        "log": {}, "future_feature": true, "x": [1, 2]
    })");
    CHECK(p.error.empty());
    CHECK(p.settings.log.enabled);
}
