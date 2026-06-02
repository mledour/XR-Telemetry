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
// settings.h — pure parser for the xr_telemetry settings.json schema.
//
//   {
//     "log": {
//       "enabled": false,
//       "mode": "auto" | "hotkey",
//       "hotkey": { "key": "T", "modifiers": ["ctrl", "shift"] }
//     },
//     "overlay": { "enabled", "mode", "hotkey", "refresh_hz",
//                  "position", "scale" }
//   }
//
// Everything is permissive: missing fields fall back to the "preserve
// current behaviour" defaults (log enabled, auto mode, default hotkey).
// A malformed file returns the same defaults plus an error string the
// caller can log — we NEVER throw / abort because of bad settings; the
// layer's contract is to keep the host process alive (CLAUDE.md rule 9).
//
// Stays self-contained — does NOT include pch.h. Pulls only rapidjson
// (already on the include path via external/OpenXR-MixedReality) and the
// sibling hotkey.h. Tests #include this header directly.
// =============================================================================

#include "hotkey.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <string>
#include <vector>

namespace openxr_api_layer::detail {

    // How a feature is driven. Auto: active for the whole session. Hotkey:
    // stays off until the user presses the configured combo; toggles on/off
    // on each subsequent press. Used by BOTH log and overlay — they have
    // independent settings blocks and independent hotkeys, but they share
    // the enum so the parser and the call sites stay symmetric.
    enum class LogMode { Auto, Hotkey };
    enum class OverlayMode { Auto, Hotkey };

    // Parsed "log" block. `hotkey` is only consulted when mode == Hotkey, but
    // it's always populated from the JSON (or the default) so logs can show
    // the bound combo regardless of mode.
    //
    // Defaults are deliberately opt-in: enabled=false. A fresh install does
    // nothing until the user explicitly turns the CSV on (or the overlay),
    // so the layer never silently leaves artefacts on disk.
    struct LogSettings {
        bool enabled = false;
        LogMode mode = LogMode::Auto;
        HotkeySpec hotkey{};
    };

    // Parsed "overlay" block. Same shape as LogSettings + `refresh_hz` for
    // the snapshot cadence, a `position` string (head_top_right /
    // head_top_left / head_top_center / head_center; unknown values fall
    // back to head_top_right), and a `scale` multiplier.
    //
    // Defaults are deliberately opt-in: enabled=false. A user upgrading
    // from a pre-overlay layer gets the same CSV-only behaviour they had
    // before, and the HUD never appears uninvited.
    struct OverlaySettings {
        bool enabled = false;
        OverlayMode mode = OverlayMode::Auto;
        HotkeySpec hotkey{};
        int refresh_hz = 10;       // matches fpsvr's cadence
        std::string position = "head_top_right";
        // Multiplier on the default quad size. 1.0 = stock (head-locked,
        // off-axis in a corner of the FOV); 0.5 → half-size; 2.0 →
        // double. Anything outside [0.5, 2.0] is clamped so a typo
        // can't render a quad big enough to obscure the cockpit or
        // smaller than the font can be drawn legibly.
        float scale = 1.0f;
    };

    // Top-level settings struct.
    struct TelemetrySettings {
        LogSettings log{};
        OverlaySettings overlay{};
    };

    // Result of parseSettings — the parsed struct plus an optional diagnostic
    // the caller can stick into the log. error.empty() iff parsing succeeded
    // cleanly (no malformed JSON). The struct is still well-formed even when
    // error is non-empty: it falls back to the defaults, so callers can use
    // it unconditionally.
    struct ParsedSettings {
        TelemetrySettings settings;
        std::string error;
    };

    // The default hotkey shipped with the template for the LOG feature
    // (Ctrl+Shift+T). Defined as a function (rather than a constexpr) so
    // it can use the same parseHotkey() path tests exercise — guarantees
    // the documented default is itself valid.
    inline HotkeySpec defaultHotkey() noexcept {
        return parseHotkey("T", {"ctrl", "shift"});
    }

    // The default hotkey for the OVERLAY feature (Ctrl+Shift+O). Different
    // from the log default so a user with mode=hotkey on BOTH can drive
    // them independently without a chord collision.
    inline HotkeySpec defaultOverlayHotkey() noexcept {
        return parseHotkey("O", {"ctrl", "shift"});
    }

    namespace settings_parse_impl {
        // rapidjson "GetXxxOr" mini-shims so the main parser reads as a flat
        // list of "look for field, fall back if missing/wrong-type".
        inline bool getBoolOr(const rapidjson::Value& obj, const char* key,
                              bool defaultVal) noexcept {
            const auto it = obj.FindMember(key);
            if (it == obj.MemberEnd() || !it->value.IsBool()) return defaultVal;
            return it->value.GetBool();
        }
        inline std::string getStringOr(const rapidjson::Value& obj, const char* key,
                                       const std::string& defaultVal) {
            const auto it = obj.FindMember(key);
            if (it == obj.MemberEnd() || !it->value.IsString()) return defaultVal;
            return std::string(it->value.GetString(), it->value.GetStringLength());
        }
        inline std::vector<std::string> getStringArrayOr(
            const rapidjson::Value& obj, const char* key,
            const std::vector<std::string>& defaultVal) {
            const auto it = obj.FindMember(key);
            if (it == obj.MemberEnd() || !it->value.IsArray()) return defaultVal;
            std::vector<std::string> out;
            out.reserve(it->value.Size());
            for (const auto& v : it->value.GetArray()) {
                if (v.IsString()) out.emplace_back(v.GetString(), v.GetStringLength());
            }
            return out;
        }
        // Permissive int reader: accepts both integer-typed and float-typed
        // JSON values, since a user is likely to write `"refresh_hz": 10`
        // OR `"refresh_hz": 10.0` and both should work. Out-of-range floats
        // are clamped at the caller's boundary, not here.
        inline int getIntOr(const rapidjson::Value& obj, const char* key,
                            int defaultVal) noexcept {
            const auto it = obj.FindMember(key);
            if (it == obj.MemberEnd()) return defaultVal;
            if (it->value.IsInt())     return it->value.GetInt();
            if (it->value.IsDouble())  return static_cast<int>(it->value.GetDouble());
            return defaultVal;
        }
        // Permissive float reader: accepts integer-typed JSON too (a user
        // writing `"scale": 1` instead of `"scale": 1.0` is reasonable).
        inline float getFloatOr(const rapidjson::Value& obj, const char* key,
                                float defaultVal) noexcept {
            const auto it = obj.FindMember(key);
            if (it == obj.MemberEnd()) return defaultVal;
            if (it->value.IsDouble())  return static_cast<float>(it->value.GetDouble());
            if (it->value.IsInt())     return static_cast<float>(it->value.GetInt());
            return defaultVal;
        }
    } // namespace settings_parse_impl

    // Parse a settings.json text into a TelemetrySettings + error string.
    // Missing fields, wrong-type fields, and unknown enum values all fall
    // back to their documented defaults; only a JSON-level parse error (not
    // an object at the root, invalid syntax, …) populates `error`.
    inline ParsedSettings parseSettings(const std::string& jsonText) {
        using namespace settings_parse_impl;
        ParsedSettings result;
        // Apply the documented defaults up-front so EVERY return path below
        // produces a well-formed TelemetrySettings.
        result.settings.log.enabled = false;
        result.settings.log.mode = LogMode::Auto;
        result.settings.log.hotkey = defaultHotkey();
        result.settings.overlay.enabled = false;
        result.settings.overlay.mode = OverlayMode::Auto;
        result.settings.overlay.hotkey = defaultOverlayHotkey();
        result.settings.overlay.refresh_hz = 10;
        result.settings.overlay.position = "head_top_right";
        result.settings.overlay.scale = 1.0f;

        if (jsonText.empty()) {
            // Treat an empty file as "use defaults", silently. Matches the
            // bootstrap path where we copy the template (non-empty) on
            // first run; an empty per-app file is something a user
            // explicitly produced.
            return result;
        }

        rapidjson::Document doc;
        doc.Parse(jsonText.c_str(), jsonText.size());
        if (doc.HasParseError()) {
            result.error = std::string("JSON parse error at offset ") +
                           std::to_string(doc.GetErrorOffset()) + ": " +
                           rapidjson::GetParseError_En(doc.GetParseError());
            return result;
        }
        if (!doc.IsObject()) {
            result.error = "settings.json root is not a JSON object";
            return result;
        }

        // ---- log block --------------------------------------------------
        const auto logIt = doc.FindMember("log");
        if (logIt != doc.MemberEnd() && logIt->value.IsObject()) {
            const auto& log = logIt->value;
            result.settings.log.enabled = getBoolOr(log, "enabled", false);
            const std::string modeStr = getStringOr(log, "mode", "auto");
            if (iequalsAscii(modeStr, "hotkey")) {
                result.settings.log.mode = LogMode::Hotkey;
            } else {
                // Unknown mode strings fall back to auto. Don't surface as
                // an error — the user gets the current behaviour and the
                // typo doesn't disable telemetry.
                result.settings.log.mode = LogMode::Auto;
            }

            const auto hkIt = log.FindMember("hotkey");
            if (hkIt != log.MemberEnd() && hkIt->value.IsObject()) {
                const auto& hk = hkIt->value;
                const std::string keyName = getStringOr(hk, "key", "T");
                const auto mods = getStringArrayOr(hk, "modifiers", {"ctrl", "shift"});
                HotkeySpec parsed = parseHotkey(keyName, mods);
                // If the user wrote a name we can't resolve, keep the
                // documented default rather than disabling the hotkey
                // silently. Logged at the call site via formatHotkey on
                // both `parsed` and `defaultHotkey()`.
                result.settings.log.hotkey = parsed.valid() ? parsed : defaultHotkey();
            }
        }

        // ---- overlay block ----------------------------------------------
        // Same robustness contract as the log block: anything missing
        // falls back to the documented default, anything wrongly-typed
        // is silently ignored (don't break a user's session for a typo).
        const auto ovIt = doc.FindMember("overlay");
        if (ovIt != doc.MemberEnd() && ovIt->value.IsObject()) {
            const auto& ov = ovIt->value;
            result.settings.overlay.enabled = getBoolOr(ov, "enabled", false);

            const std::string modeStr = getStringOr(ov, "mode", "auto");
            result.settings.overlay.mode = iequalsAscii(modeStr, "hotkey")
                ? OverlayMode::Hotkey
                : OverlayMode::Auto;

            const auto hkIt = ov.FindMember("hotkey");
            if (hkIt != ov.MemberEnd() && hkIt->value.IsObject()) {
                const auto& hk = hkIt->value;
                const std::string keyName = getStringOr(hk, "key", "O");
                const auto mods = getStringArrayOr(hk, "modifiers", {"ctrl", "shift"});
                HotkeySpec parsed = parseHotkey(keyName, mods);
                // Same fallback as the log hotkey: unknown key name →
                // documented default (Ctrl+Shift+O) rather than disabling
                // the binding entirely. A typo never breaks the user's
                // ability to summon the HUD.
                result.settings.overlay.hotkey = parsed.valid() ? parsed : defaultOverlayHotkey();
            }

            // refresh_hz clamped to [1, 60]. Above 60 the per-frame jitter
            // exceeds the refresh window and snapshots stutter; below 1
            // the HUD goes catatonic. 10 Hz (fpsvr-like) stays the
            // recommended default.
            const int rawHz = getIntOr(ov, "refresh_hz", 10);
            result.settings.overlay.refresh_hz = std::clamp(rawHz, 1, 60);

            // `position` selects the head-locked quad placement.
            // Recognised values: head_top_right (default), head_top_left,
            // head_top_center, head_center. Unknown strings still get
            // stored verbatim — the renderer falls back to head_top_right
            // when it can't decode them, so a typo never disables the
            // overlay, it just lands in the default corner.
            //
            // Hard length cap: the recognised values are all <= 16
            // chars; anything past 64 is either junk or a malicious /
            // corrupted JSON trying to make us hold a multi-MB string
            // verbatim. Clamp to the default rather than allocate.
            // This matches the broader settings-parser philosophy
            // (validate inputs aggressively, fall back to documented
            // defaults rather than propagating garbage).
            {
                std::string rawPosition = getStringOr(ov, "position", "head_top_right");
                if (rawPosition.size() > 64) {
                    rawPosition = "head_top_right";
                }
                result.settings.overlay.position = std::move(rawPosition);
            }

            // `scale` clamped to [0.5, 2.0]. 1.0 ≙ the default quad size
            // (see kBaseWidth / kBaseHeight in overlay_layout.h); 0.5 → half,
            // 2.0 → double.
            const float rawScale = getFloatOr(ov, "scale", 1.0f);
            result.settings.overlay.scale = std::clamp(rawScale, 0.5f, 2.0f);
        }

        return result;
    }

} // namespace openxr_api_layer::detail
