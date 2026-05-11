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

#include "pch.h"

namespace {
    constexpr uint32_t k_maxLoggedErrors = 100;
    uint32_t g_globalErrorCount = 0;
} // namespace

namespace openxr_api_layer::log {
    extern std::ofstream logStream;

    // {cbf3adcd-42b1-4c38-830c-91980af201f8}
    TRACELOGGING_DEFINE_PROVIDER(g_traceProvider,
                                 "OpenXRTemplate",
                                 (0xcbf3adcd, 0x42b1, 0x4c38, 0x83, 0x0c, 0x91, 0x98, 0x0a, 0xf2, 0x01, 0xf8));

    TraceLoggingActivity<g_traceProvider> g_traceActivity;

    namespace {

        // Serializes access to logStream and reopenLogFile. Layers that
        // do anything off the frame thread (live-config watcher, async
        // texture upload, helper threads) will emit Log() from there;
        // without this lock those would race against the frame thread's
        // own Log() calls. OutputDebugStringA is documented as serialized
        // by the kernel, so it stays outside the lock to keep the
        // critical section short.
        std::mutex g_logMutex;

        // Writes a timestamped line through the framework log pipeline.
        // forceFlush=true persists the line to disk immediately — used by
        // ErrorLog so a post-error crash still leaves the message on disk.
        // forceFlush=false leaves the line in the std::ofstream buffer (a
        // few KB) and lets the OS persist on the next overflow or on
        // process exit (logStream's destructor flushes when it closes).
        // The init burst — xrCreateInstance fires ~15 Log() calls in
        // sequence — used to be ~15 forced fsyncs; with forceFlush=false
        // it's now one buffered write per call and a single implicit
        // flush at shutdown.
        void InternalLog(const char* fmt, va_list va, bool forceFlush) {
            const std::time_t now = std::time(nullptr);

            char buf[1024];
            size_t offset = std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %z: ", std::localtime(&now));
            vsnprintf_s(buf + offset, sizeof(buf) - offset, _TRUNCATE, fmt, va);
            OutputDebugStringA(buf);
            std::lock_guard<std::mutex> lock(g_logMutex);
            if (logStream.is_open()) {
                logStream << buf;
                if (forceFlush) {
                    logStream.flush();
                }
            }
        }
    } // namespace

    void reopenLogFile(const std::filesystem::path& path) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (logStream.is_open()) {
            logStream.flush();
            logStream.close();
        }
        // Append mode: multiple runs of the same app accumulate in the
        // same file, so the user can compare consecutive launches when
        // hunting a regression.
        logStream.open(path.string(), std::ios_base::app);
    }

    void Log(const char* fmt, ...) {
        va_list va;
        va_start(va, fmt);
        InternalLog(fmt, va, /*forceFlush=*/false);
        va_end(va);
    }

    void ErrorLog(const char* fmt, ...) {
        if (g_globalErrorCount++ < k_maxLoggedErrors) {
            va_list va;
            va_start(va, fmt);
            // Errors flush immediately: if the host crashes shortly after
            // (which is often why an error fired in the first place), the
            // message must already be on disk for post-mortem debugging.
            InternalLog(fmt, va, /*forceFlush=*/true);
            va_end(va);
            if (g_globalErrorCount == k_maxLoggedErrors) {
                Log("Maximum number of errors logged. Going silent.");
            }
        }
    }

    void DebugLog(const char* fmt, ...) {
#ifdef _DEBUG
        va_list va;
        va_start(va, fmt);
        InternalLog(fmt, va, /*forceFlush=*/false);
        va_end(va);
#endif
    }

} // namespace openxr_api_layer::log
