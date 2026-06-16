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

    // RAII scoped timer for trace-gated ETW duration spans. Captures the start
    // tick on construction (only when the provider is enabled) and reports the
    // elapsed time in nanoseconds off a process-cached QPC frequency. The event
    // NAME must stay a string literal in the TraceLoggingWrite macro, so the
    // write itself remains at the call site; this factors out the
    // IsTraceEnabled() gate and the QueryPerformanceCounter / frequency
    // bookkeeping the per-frame instrumentation spans would otherwise repeat:
    //
    //     log::ScopedQpcSpan span;
    //     /* ...timed work... */
    //     if (span.enabled())
    //         TraceLoggingWrite(g_traceProvider, "my_event",
    //                           TLArg(span.elapsedNs(), "duration_ns"), ...);
    class ScopedQpcSpan {
      public:
        ScopedQpcSpan() noexcept : m_enabled(IsTraceEnabled()) {
            if (m_enabled) {
                LARGE_INTEGER c;
                QueryPerformanceCounter(&c);
                m_startTicks = c.QuadPart;
            }
        }
        bool enabled() const noexcept { return m_enabled; }
        // Nanoseconds since construction. Splits whole/fractional so the
        // ticks * 1e9 intermediate can't overflow int64 for any realistic
        // span (same strategy as detail::qpcToNs).
        int64_t elapsedNs() const noexcept {
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            const int64_t freq = frequency();
            if (freq <= 0) return 0;
            const int64_t ticks = c.QuadPart - m_startTicks;
            return (ticks / freq) * 1'000'000'000LL
                 + ((ticks % freq) * 1'000'000'000LL) / freq;
        }

      private:
        // QPC frequency is fixed for the process lifetime — query once.
        static int64_t frequency() noexcept {
            static const int64_t f = [] {
                LARGE_INTEGER q;
                QueryPerformanceFrequency(&q);
                return q.QuadPart;
            }();
            return f;
        }
        bool    m_enabled;
        int64_t m_startTicks = 0;
    };

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
