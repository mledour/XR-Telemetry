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
//       "enabled": true,
//       "mode": "auto" | "hotkey",
//       "hotkey": { "key": "T", "modifiers": ["ctrl", "shift"] }
//     },
//     "overlay": { ... }                ← reserved, not parsed yet
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

#include <string>
#include <vector>

namespace openxr_api_layer::detail {

    // How the per-frame CSV writer is driven. Auto: opens at xrCreateSession
    // and closes at xrEndSession (the original always-on behaviour). Hotkey:
    // stays closed until the user presses the configured combo; toggles open/
    // closed on each subsequent press.
    enum class LogMode { Auto, Hotkey };

    // Parsed "log" block. `hotkey` is only consulted when mode == Hotkey, but
    // it's always populated from the JSON (or the default) so logs can show
    // the bound combo regardless of mode.
    struct LogSettings {
        bool enabled = true;
        LogMode mode = LogMode::Auto;
        HotkeySpec hotkey{};
    };

    // Top-level settings struct. Overlay is reserved — the field exists so
    // adding overlay parsing later doesn't require renaming anything.
    struct TelemetrySettings {
        LogSettings log{};
        // Reserved for the future in-headset overlay. Intentionally empty
        // today: the parser tolerates an "overlay" block in the JSON but
        // doesn't extract anything from it yet.
        struct OverlaySettings {
        } overlay{};
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

    // The default hotkey shipped with the template. Defined as a function
    // (rather than a constexpr) so it can use the same parseHotkey() path
    // tests exercise — guarantees the documented default is itself valid.
    inline HotkeySpec defaultHotkey() noexcept {
        return parseHotkey("T", {"ctrl", "shift"});
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
        result.settings.log.enabled = true;
        result.settings.log.mode = LogMode::Auto;
        result.settings.log.hotkey = defaultHotkey();

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
            result.settings.log.enabled = getBoolOr(log, "enabled", true);
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
        // Tolerated, not parsed. When we add real overlay settings later,
        // extend the OverlaySettings struct and pull fields here. Per-app
        // files that already contain an `overlay` block (from the future
        // template) will not need migration.
        (void)doc.FindMember("overlay");

        return result;
    }

} // namespace openxr_api_layer::detail
