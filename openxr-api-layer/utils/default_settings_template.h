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

// =============================================================================
// default_settings_template.h — single source of truth for the built-in
// JSON template the layer falls back to when neither installer-dropped
// settings.json nor a per-app file exist.
//
// MUST stay semantically aligned with installer/default_settings.json —
// installer users and ZIP users must see identical first-run behaviour.
// `test_telemetry.cpp` enforces this via parseSettings() round-trip
// equality at every build, so a drift between this constexpr and the
// installer-shipped file fails CI.
//
// Header-only so the layer DLL and the test binary share the exact same
// byte sequence (otherwise a copy/paste in two places would silently
// drift). Pure C-string, no dependencies — drag-and-drop safe.
// =============================================================================

namespace openxr_api_layer::detail {

    inline constexpr const char* kBuiltInDefaultSettings = R"({
  "_comment": "Default template for XR_APILAYER_MLEDOUR_xr_telemetry. Each OpenXR application gets a copy of this file the first time it runs (named <app>_settings.json next to this one). Edit values here to change the defaults that apply to NEW games; existing per-app files are never touched on subsequent runs.",
  "log": {
    "_comment": "Per-frame CSV capture. Enabled in hotkey mode by default (armed but dormant: no CSV is written until you press the hotkey). mode=auto opens a CSV at session start and closes it at session end. mode=hotkey keeps the CSV closed until the user presses the configured combo, then toggles open/closed on each subsequent press. The hotkey is polled once per frame inside xrEndFrame, so it only fires while the game has focus and is rendering OpenXR.",
    "enabled": true,
    "mode": "hotkey",
    "hotkey": {
      "_comment": "Recognised keys: A-Z, 0-9, F1-F24, Space, Tab, Enter, Escape, Backspace, Insert, Delete, Home, End, PageUp, PageDown, Up, Down, Left, Right. Punctuation is intentionally unsupported (locale-dependent). Recognised modifiers: ctrl, shift, alt, win. Unknown modifier names are ignored; an unknown key falls back to the documented default (Ctrl+Shift+T) so a typo never disables the hotkey entirely.",
      "key": "T",
      "modifiers": ["ctrl", "shift"]
    }
  },
  "overlay": {
    "_comment": "In-headset HUD showing fps / avg fps / cpu+gpu frametime / cpu+gpu utilisation %. Enabled in hotkey mode by default (armed but dormant: the HUD stays hidden until you press the hotkey). mode=auto displays the HUD for the whole session whenever enabled=true; mode=hotkey leaves it hidden until the user presses the configured combo, then toggles on/off. refresh_hz controls how often the displayed numbers update (1-60 Hz, clamped); 10 Hz matches fpsvr and is readable in motion. position places the quad (head_top_right default, or head_top_left / head_top_center / head_center); scale (0.5-2.0) multiplies its size. offset_x / offset_y nudge the quad centre in metres on top of position (+x right, +y up; negative = left/down), clamped to [-1,1], to push the HUD further into a corner or back off it. anchor selects the reference frame: head (default) keeps the HUD attached to the headset so it follows your gaze; world freezes it in the play space in front of you at the moment it turns on and leaves it there as you move (toggle it off then on to re-centre it).",
    "enabled": true,
    "mode": "hotkey",
    "hotkey": {
      "_comment": "Distinct from the log hotkey so users running both features in mode=hotkey can drive them independently. Same key/modifier syntax as log.hotkey.",
      "key": "O",
      "modifiers": ["ctrl", "shift"]
    },
    "refresh_hz": 10,
    "position": "head_top_right",
    "scale": 1.0,
    "anchor": "head",
    "offset_x": 0.0,
    "offset_y": 0.0
  }
}
)";

} // namespace openxr_api_layer::detail
