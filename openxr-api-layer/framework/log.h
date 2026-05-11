// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
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

#include "pch.h"

namespace openxr_api_layer::log {

    TRACELOGGING_DECLARE_PROVIDER(g_traceProvider);

    extern TraceLoggingActivity<g_traceProvider> g_traceGlobal;

#define IsTraceEnabled() TraceLoggingProviderEnabled(g_traceProvider, 0, 0)

#define TraceLocalActivity(activity) TraceLoggingActivity<g_traceProvider> activity;

#define TLArg(var, ...) TraceLoggingValue(var, ##__VA_ARGS__)
#define TLPArg(var, ...) TraceLoggingPointer(var, ##__VA_ARGS__)
#ifdef _M_IX86
#define TLXArg TLArg
#else
#define TLXArg TLPArg
#endif

    // Closes the current file logger and reopens it at `path` in append
    // mode. Used by the layer to switch from the bootstrap log (written
    // during xrNegotiateLoaderApiLayerInterface, before the application
    // name is known) to a per-app log file, once xrCreateInstance hands
    // us an XrApplicationInfo.
    void reopenLogFile(const std::filesystem::path& path);

    // General logging function. Writes a timestamped line to OutputDebugString
    // and to the per-app log file (buffered; flushed on shutdown or on the
    // next ErrorLog).
    //
    // **Do not call from frame-thread hot paths.** Each call costs a few
    // microseconds for formatting plus a kernel transition for
    // OutputDebugStringA — and several hundred microseconds when a debugger
    // (DebugView, Visual Studio) is attached. This function is for init,
    // session-scoped diagnostics, and once-per-event guarded reporting (see
    // m_fovLogged / m_strippedAppLayerLogged in layer.cpp for the pattern).
    //
    // For per-frame instrumentation, use TraceLoggingWrite (ETW): ~50 ns
    // per event in steady state, no disk I/O, and the bench harness in this
    // repo already consumes the resulting traces.
    void Log(const char* fmt, ...);
    static inline void Log(const std::string_view& str) {
        Log(str.data());
    }

    // Debug logging function. Can make things very slow (only enabled on Debug builds).
    void DebugLog(const char* fmt, ...);
    static inline void DebugLog(const std::string_view& str) {
        Log(str.data());
    }

    // Error logging function. Flushes to disk immediately (a post-error
    // crash must not lose the message). Self-rate-limits after 100 calls
    // per process so a misbehaving caller can't fill the log file.
    void ErrorLog(const char* fmt, ...);
    static inline void ErrorLog(const std::string_view& str) {
        Log(str.data());
    }

} // namespace openxr_api_layer::log
