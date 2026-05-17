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
// hotkey.h — pure helpers for the "mode=hotkey" feature of xr_telemetry's
// settings.
//
// Two responsibilities:
//   1. Parse a (key-name, [modifier-names]) pair from JSON into a HotkeySpec
//      that holds a Windows VK code + four modifier booleans.
//   2. Edge-detect a "currently-held" boolean across consecutive ticks so
//      the layer triggers exactly once per physical press of the combo,
//      not once per frame for as long as the key is held.
//
// Stays self-contained — does NOT include pch.h or <Windows.h>. The VK
// constants are reproduced as plain integers so the test binary can
// exercise the parser on macOS / Linux without the Windows SDK headers.
// The values are bit-for-bit the standard winuser.h VK_* macros, sourced
// from the Microsoft documentation.
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace openxr_api_layer::detail {

    // Result of parsing the JSON "hotkey" block. The four mod_* booleans
    // track which modifier keys must ALSO be held when `vk` is pressed for
    // the combo to fire. `vk == 0` is the "no hotkey configured / parse
    // failure" sentinel — HotkeyEdgeDetector treats it as never-triggered.
    struct HotkeySpec {
        int vk = 0;
        bool ctrl = false;
        bool shift = false;
        bool alt = false;
        bool win = false;
        bool valid() const noexcept { return vk != 0; }
    };

    // Case-insensitive ASCII compare. Folds A-Z to a-z, leaves the rest
    // untouched. Header-only because it's a small loop and we don't want a
    // separate .cpp for one helper. Pure: no allocations, no locale.
    inline bool iequalsAscii(const std::string& a, const std::string& b) noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return true;
    }

    // Resolves a JSON-style key name ("F11", "T", "Space", …) to its
    // Windows VK code. Returns 0 for unknown names so the caller can
    // distinguish "no hotkey" from a valid VK_LBUTTON (which we never
    // accept anyway — see the alphanumeric / function-key whitelist).
    //
    // Supported names (case-insensitive):
    //   - A-Z                        → VK_A..VK_Z (use the ASCII upper-
    //                                  case as the code, per winuser.h)
    //   - 0-9                        → VK_0..VK_9 (ASCII digit)
    //   - F1-F24                     → VK_F1 (0x70) .. VK_F24 (0x87)
    //   - Space, Tab, Enter / Return, Escape / Esc, Backspace
    //   - Insert, Delete, Home, End, PageUp / PgUp, PageDown / PgDn
    //   - Up, Down, Left, Right
    //
    // Punctuation is NOT supported on purpose — locale-dependent
    // mapping (an AZERTY ';' is not a QWERTY ';') makes those keys a bad
    // user experience for a hotkey. Function keys + letters/digits are
    // layout-invariant on Windows.
    inline int vkFromName(const std::string& name) noexcept {
        if (name.empty()) return 0;

        // Single ASCII alphanumeric: VK code == uppercase ASCII / digit.
        if (name.size() == 1) {
            const char c = name[0];
            if (c >= 'a' && c <= 'z') return c - 'a' + 'A';   // 'a' → 0x41
            if (c >= 'A' && c <= 'Z') return c;
            if (c >= '0' && c <= '9') return c;               // '0' → 0x30
            return 0;
        }

        // F1..F24
        if ((name[0] == 'F' || name[0] == 'f') &&
            (name.size() == 2 || name.size() == 3)) {
            int n = 0;
            for (size_t i = 1; i < name.size(); ++i) {
                if (name[i] < '0' || name[i] > '9') { n = 0; break; }
                n = n * 10 + (name[i] - '0');
            }
            if (n >= 1 && n <= 24) return 0x70 + (n - 1);     // VK_F1 = 0x70
        }

        // Named keys.
        struct NameVk { const char* name; int vk; };
        static constexpr NameVk kNamed[] = {
            {"Space",     0x20}, {"Tab",       0x09},
            {"Enter",     0x0D}, {"Return",    0x0D},
            {"Escape",    0x1B}, {"Esc",       0x1B},
            {"Backspace", 0x08},
            {"Insert",    0x2D}, {"Delete",    0x2E}, {"Del",      0x2E},
            {"Home",      0x24}, {"End",       0x23},
            {"PageUp",    0x21}, {"PgUp",      0x21},
            {"PageDown",  0x22}, {"PgDn",      0x22},
            {"Up",        0x26}, {"Down",      0x28},
            {"Left",      0x25}, {"Right",     0x27},
        };
        for (const auto& e : kNamed) {
            if (iequalsAscii(name, e.name)) return e.vk;
        }
        return 0;
    }

    // Parses a JSON "hotkey" block — `keyName` is the main key, `modifiers`
    // is the list of modifier strings. Unknown key name → invalid spec
    // (vk == 0). Unknown modifier names are silently ignored so a typo
    // in one modifier doesn't disable the entire hotkey (the user's main
    // intent — the key — still resolves).
    //
    // Recognised modifier names (case-insensitive):
    //   ctrl / control      → ctrl=true
    //   shift               → shift=true
    //   alt                 → alt=true
    //   win / windows / cmd → win=true
    inline HotkeySpec parseHotkey(const std::string& keyName,
                                  const std::vector<std::string>& modifiers) noexcept {
        HotkeySpec spec;
        spec.vk = vkFromName(keyName);
        for (const auto& m : modifiers) {
            if (iequalsAscii(m, "ctrl") || iequalsAscii(m, "control")) spec.ctrl = true;
            else if (iequalsAscii(m, "shift"))                          spec.shift = true;
            else if (iequalsAscii(m, "alt"))                            spec.alt = true;
            else if (iequalsAscii(m, "win") || iequalsAscii(m, "windows")
                                            || iequalsAscii(m, "cmd"))  spec.win = true;
        }
        return spec;
    }

    // Builds a human-readable label like "Ctrl+Shift+T" for log messages.
    // Order is stable (Ctrl → Shift → Alt → Win → key) so users can grep
    // a log without ambiguity. Returns "<none>" if spec.valid() is false.
    inline std::string formatHotkey(const HotkeySpec& spec) {
        if (!spec.valid()) return "<none>";
        std::string out;
        if (spec.ctrl)  out += "Ctrl+";
        if (spec.shift) out += "Shift+";
        if (spec.alt)   out += "Alt+";
        if (spec.win)   out += "Win+";
        // Render F-keys with their number, named keys with their canonical
        // capitalisation, alphanumerics as-is in uppercase.
        if (spec.vk >= 0x70 && spec.vk <= 0x87) {
            out += "F" + std::to_string(spec.vk - 0x70 + 1);
        } else if ((spec.vk >= 'A' && spec.vk <= 'Z') ||
                   (spec.vk >= '0' && spec.vk <= '9')) {
            out += static_cast<char>(spec.vk);
        } else {
            // Look the name up in the named table (linear scan is fine,
            // this only runs on bootstrap / state changes).
            struct VkName { int vk; const char* name; };
            static constexpr VkName kNamed[] = {
                {0x20, "Space"}, {0x09, "Tab"}, {0x0D, "Enter"}, {0x1B, "Escape"},
                {0x08, "Backspace"}, {0x2D, "Insert"}, {0x2E, "Delete"},
                {0x24, "Home"}, {0x23, "End"}, {0x21, "PageUp"}, {0x22, "PageDown"},
                {0x26, "Up"}, {0x28, "Down"}, {0x25, "Left"}, {0x27, "Right"},
            };
            for (const auto& e : kNamed) {
                if (e.vk == spec.vk) { out += e.name; return out; }
            }
            out += "VK(0x" + std::to_string(spec.vk) + ")";
        }
        return out;
    }

    // Rising-edge detector. Pass `pressed` in once per tick (the layer
    // calls this once per xrEndFrame from the frame thread). Returns
    // true exactly once per low→high transition; false when the key is
    // still held, just released, or never-pressed.
    //
    // Keeping this stateful object owned by the caller (rather than
    // baking the previous-state into HotkeySpec) lets tests drive it
    // with synthetic sequences without poking statics.
    //
    // No reset() — settings are NOT hot-reloaded (see the README "What's
    // deliberately NOT here" note), so the only consumers of a fresh
    // detector are a freshly-constructed OpenXrLayer / TelemetryFixture,
    // which already get one via the default-initialised m_prev. Re-add
    // a reset path the day live-reload ships.
    class HotkeyEdgeDetector {
      public:
        // Returns true on rising edge, false otherwise. `pressed == false`
        // always resets the latch so the NEXT press re-triggers.
        bool tick(bool pressed) noexcept {
            const bool rising = pressed && !m_prev;
            m_prev = pressed;
            return rising;
        }
      private:
        bool m_prev = false;
    };

} // namespace openxr_api_layer::detail
