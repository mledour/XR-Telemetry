// MIT License
//
// Copyright (c) 2026 Michael Ledour
//
// Based on https://github.com/mbucchia/OpenXR-Layer-Template.
// Copyright (c) 2022-2023 Matthieu Bucchianeri
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
// Layer skeleton — replace this with your own logic.
//
// This file is the only one most layer authors need to touch. The
// framework/ files generate the dispatch table, handle loader
// negotiation, and route every intercepted xr* call to the matching
// method on the OpenXrLayer class below. Anything you DON'T override
// is passed through to the next layer / runtime unchanged.
//
// The shipped skeleton overrides exactly one function (xrCreateInstance)
// and just logs the application's name + the active runtime. Useful as
// a sanity check that the layer is loading, and as a starting point.
//
// To add a feature:
//   1. Add the OpenXR function name to override_functions in
//      framework/layer_apis.py.
//   2. Rebuild — the dispatch generator regenerates dispatch.gen.{h,cpp}
//      with a virtual method for that function.
//   3. Override the method on the OpenXrLayer class below, do whatever
//      you want, and either return your own XrResult or call the base
//      class implementation to forward downstream.
// =============================================================================

#include "pch.h"

#include "layer.h"
#include <log.h>
#include <util.h>

namespace openxr_api_layer {

    using namespace log;

    // Extensions the layer cares about. Empty = pass-through, the
    // layer never appears in xrEnumerateApiLayerProperties' extension
    // contributions. See framework/entry.cpp for how these are
    // surfaced during loader negotiation.
    const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions = {};
    const std::vector<std::string> blockedExtensions = {};
    const std::vector<std::string> implicitExtensions = {};

    // ETW trace provider g_traceProvider is already declared by
    // framework/log.h and defined by framework/log.cpp with a fixed
    // GUID. Use TraceLoggingWrite(g_traceProvider, "<event-name>", ...)
    // from inside method bodies (NOT at namespace scope — TraceLoggingWrite
    // is a do/while statement macro). The .wprp profile in scripts\
    // captures the matching GUID for Windows Performance Recorder.

    namespace {

        // One row of CSV. Pushed from the frame thread (xrEndFrame), drained
        // by a single background writer thread. POD so we can copy by value
        // through the queue without surprises.
        struct FrameRecord {
            uint64_t frame_index;
            int64_t timestamp_qpc;   // raw QPC tick at xrEndFrame entry
            int64_t wait_block_ns;
            int64_t app_cpu_ns;
            int64_t period_ns;
            float headroom_pct;
            bool should_render;
        };

        // Async CSV writer: the frame thread does an O(1) push under a
        // try_lock; a dedicated background thread (BELOW_NORMAL priority)
        // batches records and writes them to disk. Disk I/O never lands on
        // the frame thread.
        //
        // Backpressure policy: when the queue hits kMaxQueueSize, the
        // producer drops the OLDEST record (pop_front before push_back). A
        // counter is reported at session end. The producer never blocks the
        // frame thread; if it can't even grab the mutex (consumer mid-batch
        // copy) it drops the new record and increments the counter — rare
        // but accepted as the cost of "frame thread is sacred".
        class CsvWriter {
          public:
            ~CsvWriter() {
                stop();
            }

            // Returns false on file-open failure; the rest of push() then
            // becomes a no-op. Telemetry is best-effort; we never fail the
            // host's OpenXR call because we couldn't write our CSV.
            bool start(const std::filesystem::path& path) {
                m_file.open(path, std::ios::out | std::ios::trunc);
                if (!m_file.is_open()) {
                    return false;
                }
                m_file << "frame,timestamp_qpc,wait_block_ns,app_cpu_ns,period_ns,headroom_pct,should_render\n";
                m_file.flush();
                m_path = path;
                m_started.store(true, std::memory_order_release);
                m_thread = std::thread(&CsvWriter::writerLoop, this);
                return true;
            }

            void stop() {
                if (!m_started.exchange(false)) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(m_mtx);
                    m_stopping = true;
                }
                m_cv.notify_one();
                if (m_thread.joinable()) {
                    m_thread.join();
                }
                if (m_file.is_open()) {
                    // Final footer: writers / dropped counts for the session.
                    // It's a comment-style line — Pandas / Excel ignore it if
                    // they only sniff columns past the header. read_csv will
                    // need skipfooter=1 to parse cleanly; not worth more.
                    m_file << "# session_end written=" << m_written
                           << " dropped=" << m_dropped << "\n";
                    m_file.flush();
                    m_file.close();
                }
            }

            void push(const FrameRecord& rec) {
                if (!m_started.load(std::memory_order_acquire)) {
                    return;
                }
                // try_lock so the frame thread never blocks on the consumer's
                // batch copy. Cheap (~25 ns on Windows when uncontended) and
                // we accept the rare drop on contention.
                std::unique_lock<std::mutex> lock(m_mtx, std::try_to_lock);
                if (!lock.owns_lock()) {
                    m_dropped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (m_queue.size() >= kMaxQueueSize) {
                    m_queue.pop_front();  // drop oldest (per design choice)
                    m_dropped.fetch_add(1, std::memory_order_relaxed);
                }
                m_queue.push_back(rec);
                lock.unlock();
                m_cv.notify_one();
            }

            uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }
            uint64_t written() const { return m_written; }
            const std::filesystem::path& path() const { return m_path; }

          private:
            static constexpr size_t kMaxQueueSize = 4096;        // ~45 s @ 90 Hz
            static constexpr size_t kFlushEveryRecords = 256;     // ~3 s @ 90 Hz

            void writerLoop() {
                // Don't preempt the frame thread — disk hiccups stay on us.
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

                std::vector<FrameRecord> batch;
                batch.reserve(kMaxQueueSize);
                size_t recordsSinceFlush = 0;

                while (true) {
                    {
                        std::unique_lock<std::mutex> lock(m_mtx);
                        m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
                        batch.assign(m_queue.begin(), m_queue.end());
                        m_queue.clear();
                        if (m_stopping && batch.empty()) {
                            break;
                        }
                    }
                    for (const auto& rec : batch) {
                        // fmt::format here is fine — we're on the writer
                        // thread, not the frame thread. Output a single
                        // line per frame; bool as 0/1 for Pandas friendliness.
                        m_file << fmt::format("{},{},{},{},{},{:.2f},{}\n",
                                              rec.frame_index,
                                              rec.timestamp_qpc,
                                              rec.wait_block_ns,
                                              rec.app_cpu_ns,
                                              rec.period_ns,
                                              rec.headroom_pct,
                                              rec.should_render ? 1 : 0);
                    }
                    m_written += batch.size();
                    recordsSinceFlush += batch.size();
                    // Periodic flush — a crash should not lose more than
                    // kFlushEveryRecords frames of telemetry.
                    if (recordsSinceFlush >= kFlushEveryRecords) {
                        m_file.flush();
                        recordsSinceFlush = 0;
                    }
                }
            }

            std::ofstream m_file;
            std::filesystem::path m_path;
            std::thread m_thread;
            std::mutex m_mtx;
            std::condition_variable m_cv;
            std::deque<FrameRecord> m_queue;
            std::atomic<bool> m_started{false};
            std::atomic<uint64_t> m_dropped{0};
            uint64_t m_written = 0;          // writer-thread-only, no atomic
            bool m_stopping = false;          // guarded by m_mtx
        };

        // Build %LOCALAPPDATA%\<layer>\sessions\YYYY-MM-DD_HH-MM-SS_<appName>.csv.
        // appName is sanitised to keep only [A-Za-z0-9._-]; any other char →
        // '_'. Empty appName falls back to "unknown".
        std::filesystem::path buildSessionCsvPath(const std::string& appName) {
            const auto now = std::chrono::system_clock::now();
            const auto tt = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
            ::localtime_s(&tm, &tt);
            char timestamp[32];
            std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", &tm);

            std::string safeName;
            safeName.reserve(appName.size());
            for (char c : appName) {
                const auto uc = static_cast<unsigned char>(c);
                if (std::isalnum(uc) || c == '-' || c == '_' || c == '.') {
                    safeName.push_back(c);
                } else {
                    safeName.push_back('_');
                }
            }
            if (safeName.empty()) {
                safeName = "unknown";
            }

            const auto dir = openxr_api_layer::localAppData / "sessions";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);  // best-effort
            return dir / fmt::format("{}_{}.csv", timestamp, safeName);
        }

    }  // anonymous namespace


    class OpenXrLayer : public OpenXrApi {
    public:
        OpenXrLayer() {
            // Cache QPC frequency once. Used by every frame-thread timestamp
            // conversion below; constant for the process lifetime.
            LARGE_INTEGER freq{};
            QueryPerformanceFrequency(&freq);
            m_qpcFrequency = freq.QuadPart > 0 ? freq.QuadPart : 1;
        }
        // Singleton lifetime is managed by ResetInstance() in
        // framework/dispatch.gen.cpp, invoked from the auto-generated
        // xrDestroyInstance. m_csv.stop() in the destructor joins the writer
        // thread and writes the final footer with the dropped-frame count.
        ~OpenXrLayer() override {
            m_csv.stop();
            if (m_csvPath.has_filename()) {
                Log(fmt::format("xr_telemetry: session csv closed ({} records, {} dropped) at {}\n",
                                m_csv.written(),
                                m_csv.dropped(),
                                m_csvPath.string()));
            }
        }

        // ---- xrCreateInstance ---------------------------------------------
        // First call the loader hands to a freshly-loaded layer. The
        // baseclass implementation forwards to the next layer / runtime,
        // returning a real XrInstance handle we can then query for
        // identity (application name, runtime name+version).
        //
        // IMPORTANT: do NOT add 'xrCreateInstance' to override_functions
        // in framework/layer_apis.py — the framework already routes this
        // call to whatever virtual method you declare here. Listing it
        // there causes dispatch_generator.py to abort the build. Same
        // rule applies to xrDestroyInstance, xrGetInstanceProcAddr, and
        // xrEnumerateInstanceExtensionProperties.
        //
        // The "log application + runtime, then bypass if we don't
        // recognize anything" pattern is the recommended starting
        // shape — your layer can decide here whether it should run for
        // this process at all. CLAUDE.md's rule 9 (graceful degradation)
        // is enforced by setting m_bypassApiLayer when you want the
        // layer to become a no-op for an unsupported app/runtime.
        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateInstance
        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            // ==========================================================
            // DIAGNOSTIC BUILD — diag/dr2-crash
            // Every step prints via ErrorLog (synchronous flush). After a
            // DR2 crash, the LAST "DIAG_CI: <stage>" line in
            // %LOCALAPPDATA%\XR_APILAYER_MLEDOUR_xr_telemetry\
            // XR_APILAYER_MLEDOUR_xr_telemetry.log pinpoints where the
            // process died. NOT for production — Log/ErrorLog from this
            // path is fine because xrCreateInstance runs once, far from
            // the frame loop.
            // ==========================================================
            ErrorLog("DIAG_CI: 01 entered xrCreateInstance\n");

            if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
                ErrorLog("DIAG_CI: 01a invalid struct type — returning XR_ERROR_VALIDATION_FAILURE\n");
                return XR_ERROR_VALIDATION_FAILURE;
            }
            ErrorLog("DIAG_CI: 02 struct type validated\n");

            TraceLoggingWrite(g_traceProvider, "xrCreateInstance");
            ErrorLog("DIAG_CI: 03 TraceLoggingWrite OK\n");

            // Names the loader hands us — application as declared in
            // XrApplicationInfo, plus the runtime that materialised
            // beneath us once the baseclass returns.
            const std::string appName = createInfo->applicationInfo.applicationName;
            ErrorLog(fmt::format("DIAG_CI: 04 appName='{}' (len={})\n", appName, appName.size()));

            Log(fmt::format("xr_telemetry {} starting for application '{}'\n",
                             VersionString, appName));
            ErrorLog("DIAG_CI: 05 initial Log() returned\n");

            ErrorLog("DIAG_CI: 06 about to call OpenXrApi::xrCreateInstance (chain to next layer / runtime)\n");
            const XrResult result = OpenXrApi::xrCreateInstance(createInfo);
            ErrorLog(fmt::format("DIAG_CI: 07 base xrCreateInstance returned {} ({})\n",
                                 static_cast<int>(result),
                                 XR_SUCCEEDED(result) ? "SUCCESS" : "FAILED"));
            if (XR_FAILED(result)) {
                return result;
            }

            // Runtime identity. Useful in logs when supporting multiple
            // runtimes — anti-cheat reports often reference the runtime
            // by name. Skip gracefully if the runtime doesn't fill in
            // properties for any reason; we never want to fail the
            // host's xrCreateInstance because of our own diagnostics.
            ErrorLog("DIAG_CI: 08 about to query xrGetInstanceProperties\n");
            XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
            const XrResult propsResult = OpenXrApi::xrGetInstanceProperties(GetXrInstance(), &instanceProperties);
            ErrorLog(fmt::format("DIAG_CI: 09 xrGetInstanceProperties returned {}\n", static_cast<int>(propsResult)));
            if (XR_SUCCEEDED(propsResult)) {
                Log(fmt::format("Runtime: {} {}.{}.{}\n",
                                 instanceProperties.runtimeName,
                                 XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                 XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                 XR_VERSION_PATCH(instanceProperties.runtimeVersion)));
                ErrorLog("DIAG_CI: 10 runtime identity Log() returned\n");
            } else {
                ErrorLog("DIAG_CI: 10 runtime identity skipped (props query failed)\n");
            }

            // Open the per-session CSV file. Failures here are non-fatal —
            // the layer keeps running as a pure pass-through if we can't
            // create the file (read-only LOCALAPPDATA, disk full, …).
            ErrorLog("DIAG_CI: 11 about to call buildSessionCsvPath\n");
            m_csvPath = buildSessionCsvPath(appName);
            ErrorLog(fmt::format("DIAG_CI: 12 csv path = '{}'\n", m_csvPath.string()));

            ErrorLog("DIAG_CI: 13 about to call m_csv.start (file open + thread spawn)\n");
            const bool csvStarted = m_csv.start(m_csvPath);
            ErrorLog(fmt::format("DIAG_CI: 14 m_csv.start returned {}\n", csvStarted ? "true" : "false"));
            if (csvStarted) {
                Log(fmt::format("xr_telemetry: writing per-frame CSV to {}\n", m_csvPath.string()));
                ErrorLog("DIAG_CI: 15 csv-path Log() returned\n");
            } else {
                ErrorLog(fmt::format("xr_telemetry: failed to open CSV at {} — telemetry disabled for this session\n",
                                     m_csvPath.string()));
                ErrorLog("DIAG_CI: 15 csv start failed branch returned\n");
            }

            ErrorLog(fmt::format("DIAG_CI: 99 returning {} from xrCreateInstance\n", static_cast<int>(result)));
            return result;
        }

        // ---- xrWaitFrame --------------------------------------------------
        // Pure observation; never mutates frameWaitInfo or frameState. The
        // QPC samples around the base call let us derive wait_block_ns (how
        // long the runtime made us wait — high = compositor had headroom).
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrWaitFrame
        XrResult xrWaitFrame(XrSession session,
                             const XrFrameWaitInfo* frameWaitInfo,
                             XrFrameState* frameState) override {
            const int64_t tWaitIn = QpcNow();
            const XrResult result = OpenXrApi::xrWaitFrame(session, frameWaitInfo, frameState);
            const int64_t tWaitOut = QpcNow();

            if (XR_SUCCEEDED(result) && frameState) {
                m_tWaitIn.store(tWaitIn, std::memory_order_relaxed);
                m_tWaitOut.store(tWaitOut, std::memory_order_relaxed);
                m_predictedPeriodNs.store(frameState->predictedDisplayPeriod, std::memory_order_relaxed);
                m_lastShouldRender.store(frameState->shouldRender == XR_TRUE, std::memory_order_relaxed);
            }
            return result;
        }

        // ---- xrBeginFrame -------------------------------------------------
        // Captured for completeness (begin→end split is useful in trace
        // analysis to separate "wait→begin housekeeping" from "render
        // submission") but not used in the headroom math itself.
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrBeginFrame
        XrResult xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) override {
            const int64_t tBegin = QpcNow();
            const XrResult result = OpenXrApi::xrBeginFrame(session, frameBeginInfo);
            if (XR_SUCCEEDED(result)) {
                m_tBegin.store(tBegin, std::memory_order_relaxed);
            }
            return result;
        }

        // ---- xrEndFrame ---------------------------------------------------
        // Closes the per-frame timing window. Samples tEnd BEFORE forwarding
        // so the measurement matches the moment the app committed the frame,
        // not the runtime's compositor work that follows.
        //
        // Output goes through the async CsvWriter (push() is a try_lock +
        // deque op, never blocks; the writer thread does the disk I/O).
        //
        // IMPORTANT: do NOT call Log() / fmt::format / OutputDebugStringA
        // from this path, not even gated to every Nth frame. OutputDebugString
        // takes the kernel-wide DBWinMutex; the Pimax OpenXR compositor (in
        // a separate process) acquires the same mutex for its own logging
        // and a few-hundred-µs contention spike here is enough to make the
        // compositor drop frames — manifesting as a black HMD. Bisected on
        // PR #1: math + ETW renders fine, math + ETW + 1 Hz Log() kills the
        // HMD. ETW (TraceLoggingWrite) itself is lock-free and would be safe;
        // we chose CSV instead for the UX (no wpr/tracerpt round-trip).
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrEndFrame
        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            const int64_t tEnd = QpcNow();
            const XrResult result = OpenXrApi::xrEndFrame(session, frameEndInfo);

            const int64_t tWaitIn = m_tWaitIn.load(std::memory_order_relaxed);
            const int64_t tWaitOut = m_tWaitOut.load(std::memory_order_relaxed);
            const int64_t periodNs = m_predictedPeriodNs.load(std::memory_order_relaxed);
            const bool shouldRender = m_lastShouldRender.load(std::memory_order_relaxed);

            // First frame guard: xrEndFrame can fire before we ever observed
            // a successful xrWaitFrame. Skip the math to avoid bogus deltas.
            if (tWaitIn == 0 || tWaitOut == 0) {
                return result;
            }

            const int64_t waitBlockNs = QpcToNs(tWaitOut - tWaitIn);
            const int64_t appCpuNs = QpcToNs(tEnd - tWaitOut);

            // CPU headroom: fraction of the predicted display period the app
            // did NOT spend. Negative => app is CPU-bound this frame. Gated
            // on period > 0 because some runtimes briefly report 0 before
            // the session is fully ready.
            float headroomPct = 0.0f;
            if (periodNs > 0) {
                const double ratio = static_cast<double>(appCpuNs) / static_cast<double>(periodNs);
                headroomPct = static_cast<float>((1.0 - ratio) * 100.0);
            }
            const uint64_t frameIndex = m_frameIndex.fetch_add(1, std::memory_order_relaxed);

            // Push to the async CSV writer. push() is bounded-time (a
            // try_lock + deque manipulation, ~30-50 ns when uncontended);
            // disk I/O happens entirely on the writer thread.
            m_csv.push(FrameRecord{
                frameIndex,
                tEnd,           // raw QPC tick; converted to time in analysis
                waitBlockNs,
                appCpuNs,
                periodNs,
                headroomPct,
                shouldRender,
            });

            return result;
        }

        // ----------------------------------------------------------------
        // EXAMPLE OVERRIDES — uncomment + add the function name to
        // framework/layer_apis.py's override_functions list to wire
        // them up. The framework will regenerate dispatch.gen.{h,cpp}
        // on the next build to expose the matching virtual method.
        //
        //   XrResult xrLocateViews(XrSession session,
        //                          const XrViewLocateInfo* viewLocateInfo,
        //                          XrViewState* viewState,
        //                          uint32_t viewCapacityInput,
        //                          uint32_t* viewCountOutput,
        //                          XrView* views) override {
        //       const XrResult result = OpenXrApi::xrLocateViews(
        //           session, viewLocateInfo, viewState,
        //           viewCapacityInput, viewCountOutput, views);
        //       if (XR_SUCCEEDED(result) && !m_bypassApiLayer && views) {
        //           // ... mutate views[i].fov / views[i].pose per your
        //           // feature here ...
        //       }
        //       return result;
        //   }
        // ----------------------------------------------------------------

      private:
        // QPC helpers — QueryPerformanceCounter is monotonic, sub-microsecond,
        // and free of kernel transitions on modern Windows. We do NOT use
        // XrFrameState.predictedDisplayTime as a clock: it's a predicted
        // display point, not a measurement point, and its mapping to QPC is
        // runtime-defined.
        int64_t QpcNow() const {
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            return c.QuadPart;
        }
        // Convert QPC ticks to nanoseconds without overflowing on long runs:
        // split into whole-seconds + remainder.
        int64_t QpcToNs(int64_t ticks) const {
            const int64_t freq = m_qpcFrequency;
            const int64_t whole = ticks / freq;
            const int64_t rem = ticks % freq;
            return whole * 1'000'000'000LL + (rem * 1'000'000'000LL) / freq;
        }

        bool m_bypassApiLayer = false;
        int64_t m_qpcFrequency = 1;

        // Frame timing state — written from Wait/Begin thread, read from End
        // thread (may differ under OpenComposite where sim and render threads
        // are separate). Relaxed atomics are sufficient because the OpenXR
        // frame-loop contract guarantees at most one frame in flight between
        // Wait and End, so no field is concurrently written.
        std::atomic<int64_t> m_tWaitIn{0};
        std::atomic<int64_t> m_tWaitOut{0};
        std::atomic<int64_t> m_tBegin{0};
        std::atomic<int64_t> m_predictedPeriodNs{0};
        std::atomic<bool> m_lastShouldRender{true};
        std::atomic<uint64_t> m_frameIndex{0};

        // Async CSV writer + the path we opened (for the destructor log).
        CsvWriter m_csv;
        std::filesystem::path m_csvPath;
    };

    // Singleton accessor used by framework/dispatch.cpp.
    OpenXrApi* GetInstance() {
        static std::unique_ptr<OpenXrLayer> instance = std::make_unique<OpenXrLayer>();
        return instance.get();
    }

    // dllHome / localAppData are defined in framework/entry.cpp for the
    // DLL build, and in openxr-api-layer-tests/test_stubs.cpp for the
    // standalone test binary. Their declarations live in layer.h so
    // every TU that needs them just includes that.

} // namespace openxr_api_layer
