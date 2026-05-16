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
        // by a single background writer thread.
        struct FrameRecord {
            uint64_t frame_index;
            int64_t timestamp_qpc;     // raw QPC tick at xrEndFrame entry
            int64_t wait_block_ns;     // tWaitOut - tWaitIn   (compositor throttle)
            int64_t pre_begin_ns;      // tBegin - tWaitOut    (wait→begin housekeeping)
            int64_t app_cpu_ns;        // tEnd - tWaitOut      (wait→end window)
            int64_t frame_total_ns;    // tEnd - tEndPrev      (full cycle, includes
                                       //                       post-end sim/physics)
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
            //
            // IDEMPOTENT & THREAD-SAFE — defence-in-depth, not the steady
            // state. With the singleton wired to the framework's
            // g_instance, each XrInstance gets a fresh OpenXrLayer (and a
            // fresh CsvWriter) — start() is called exactly once per
            // CsvWriter. This guard catches the static-singleton
            // anti-pattern (where the same writer would see two start()
            // calls and the second `m_thread = std::thread(...)` would
            // call std::terminate via [thread.thread.assign]) in case a
            // future refactor reintroduces it. It also serialises
            // concurrent start()/stop() against any caller that doesn't
            // honour the loader's serialisation guarantee.
            bool start(const std::filesystem::path& path) {
                std::lock_guard<std::mutex> lifeLock(m_lifetimeMtx);
                if (m_started.load(std::memory_order_acquire)) {
                    return true;
                }
                m_file.open(path, std::ios::out | std::ios::trunc);
                if (!m_file.is_open()) {
                    return false;
                }
                m_file << "frame,timestamp_qpc,wait_block_ns,pre_begin_ns,app_cpu_ns,frame_total_ns,period_ns,headroom_pct,should_render\n";
                m_file.flush();
                m_path = path;
                m_stopping = false;  // defensive: clear from any prior stop()
                m_started.store(true, std::memory_order_release);
                m_thread = std::thread(&CsvWriter::writerLoop, this);
                return true;
            }

            // stop() is robust against ExitProcess() shutdown. On Windows,
            // ExitProcess terminates every thread except the caller BEFORE
            // running DLL_PROCESS_DETACH (which is where this destructor
            // chain lives). If the writer thread was killed while holding
            // m_mtx, naively taking it would deadlock. We:
            //   1. try to acquire m_mtx with a 100 ms deadline,
            //   2. wait for the writer thread with a 100 ms deadline,
            //   3. detach + skip the footer if either step times out.
            // Normal session shutdown (cooperative, writer alive and free)
            // completes in well under 10 ms and writes the footer; the
            // bounded wait only kicks in for hard-shutdown paths.
            void stop() {
                std::lock_guard<std::mutex> lifeLock(m_lifetimeMtx);
                if (!m_started.exchange(false)) {
                    return;
                }

                using namespace std::chrono_literals;

                // Phase 1: signal m_stopping under m_mtx, with a deadline
                // so an orphaned mutex (writer killed mid-batch) doesn't
                // hang us.
                bool signalled = false;
                {
                    std::unique_lock<std::timed_mutex> lock(m_mtx, 100ms);
                    if (lock.owns_lock()) {
                        m_stopping = true;
                        signalled = true;
                    }
                }
                if (!signalled) {
                    // Mutex unreachable — writer thread is probably dead.
                    // Detach so ~std::thread doesn't call std::terminate.
                    if (m_thread.joinable()) {
                        m_thread.detach();
                    }
                    return;
                }
                m_cv.notify_one();

                // Phase 2: wait for the writer to exit. WaitForSingleObject
                // is the Win32 way to do "join with timeout"; std::thread
                // doesn't expose one.
                if (m_thread.joinable()) {
                    const HANDLE h = m_thread.native_handle();
                    const DWORD rc = ::WaitForSingleObject(h, 100);
                    if (rc == WAIT_OBJECT_0) {
                        m_thread.join();  // now non-blocking, just bookkeeping
                    } else {
                        // Writer didn't exit in 100 ms (or its handle is
                        // gone). Detach and skip the footer.
                        m_thread.detach();
                        return;
                    }
                }

                if (m_file.is_open()) {
                    if (m_written == 0) {
                        // No frames were written — this is almost certainly
                        // the OpenComposite / OXR Toolkit probe XrInstance
                        // (capability-check; never reaches the frame loop)
                        // OR an instance that was created+destroyed before
                        // any rendering started. Don't litter the sessions/
                        // folder with empty CSVs; close and delete.
                        m_file.close();
                        std::error_code ec;
                        std::filesystem::remove(m_path, ec);  // best-effort
                    } else {
                        // Real session — write the footer with split drop
                        // counters so a slow writer (try_lock) is
                        // distinguishable from a stuffed queue (queue_full)
                        // and from disk failures (disk_write). Comment-style
                        // line: Pandas reads it with pd.read_csv(p,
                        // comment='#'); Excel ignores the unparseable row.
                        m_file << "# session_end"
                               << " written=" << m_written
                               << " dropped_try_lock=" << m_droppedTryLock.load(std::memory_order_relaxed)
                               << " dropped_queue_full=" << m_droppedQueueFull.load(std::memory_order_relaxed)
                               << " dropped_disk_write=" << m_droppedDiskWrite
                               << "\n";
                        m_file.flush();
                        m_file.close();
                    }
                }
            }

            void push(const FrameRecord& rec) {
                if (!m_started.load(std::memory_order_acquire)) {
                    return;
                }
                // try_lock so the frame thread never blocks on the consumer's
                // batch copy. Cheap (~25 ns on Windows when uncontended) and
                // we accept the rare drop on contention.
                std::unique_lock<std::timed_mutex> lock(m_mtx, std::try_to_lock);
                if (!lock.owns_lock()) {
                    m_droppedTryLock.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (m_queue.size() >= kMaxQueueSize) {
                    m_queue.pop_front();  // drop oldest (per design choice)
                    m_droppedQueueFull.fetch_add(1, std::memory_order_relaxed);
                }
                m_queue.push_back(rec);
                lock.unlock();
                m_cv.notify_one();
            }

            uint64_t droppedTryLock() const { return m_droppedTryLock.load(std::memory_order_relaxed); }
            uint64_t droppedQueueFull() const { return m_droppedQueueFull.load(std::memory_order_relaxed); }
            uint64_t droppedTotal() const { return droppedTryLock() + droppedQueueFull() + m_droppedDiskWrite; }
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
                        std::unique_lock<std::timed_mutex> lock(m_mtx);
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
                        m_file << fmt::format("{},{},{},{},{},{},{},{:.2f},{}\n",
                                              rec.frame_index,
                                              rec.timestamp_qpc,
                                              rec.wait_block_ns,
                                              rec.pre_begin_ns,
                                              rec.app_cpu_ns,
                                              rec.frame_total_ns,
                                              rec.period_ns,
                                              rec.headroom_pct,
                                              rec.should_render ? 1 : 0);
                    }

                    // Disk-error detection: if the stream went bad mid-batch
                    // (disk full, ACL revoked, file handle invalidated) we
                    // count the batch as dropped, log once, and stop trying.
                    // Clearing the bits keeps the stream usable for a future
                    // attempt (the file system may recover) but we conserve
                    // CPU by skipping further formatting if the failure is
                    // sticky — we just keep counting drops.
                    if (!m_file.good()) {
                        m_droppedDiskWrite += batch.size();
                        if (!m_diskWriteFailedLogged.exchange(true)) {
                            ErrorLog("xr_telemetry: CSV write failed mid-session; "
                                     "subsequent records will be reported in the "
                                     "dropped_disk_write footer count.\n");
                        }
                        m_file.clear();  // reset failbit/badbit so we can retry next batch
                    } else {
                        m_written += batch.size();
                    }

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
            // timed_mutex so stop() can bound its wait via try_lock_for. push()
            // still uses try_to_lock; writerLoop() uses the standard blocking
            // acquire (it's the consumer — blocking is the whole point).
            std::timed_mutex m_mtx;
            std::condition_variable_any m_cv;     // _any because m_mtx is timed_mutex
            std::deque<FrameRecord> m_queue;
            std::atomic<bool> m_started{false};

            // Three causes of dropped records, surfaced separately in the
            // footer so a slow writer is distinguishable from a stuffed
            // queue and from real disk failures.
            std::atomic<uint64_t> m_droppedTryLock{0};   // producer try_lock failed
            std::atomic<uint64_t> m_droppedQueueFull{0}; // queue saturated, oldest popped
            uint64_t m_droppedDiskWrite = 0;             // writer-thread only

            uint64_t m_written = 0;             // writer-thread only, no atomic
            bool m_stopping = false;             // guarded by m_mtx

            // One-shot guard so a disk-failure storm only logs once.
            std::atomic<bool> m_diskWriteFailedLogged{false};

            // Serialises start()/stop() with each other against any future
            // concurrent caller. The OpenXR loader serialises today; this
            // doesn't depend on that staying true.
            std::mutex m_lifetimeMtx;
        };

        // Build %LOCALAPPDATA%\<layer>\sessions\YYYY-MM-DD_HH-MM-SSZ_<appName>.csv.
        // UTC (Z suffix) rather than local time: trace files are comparable
        // across machines and immune to DST transitions mid-session.
        // appName is sanitised to keep only [A-Za-z0-9._-]; any other char →
        // '_'. Empty appName falls back to "unknown".
        std::filesystem::path buildSessionCsvPath(const std::string& appName) {
            const auto now = std::chrono::system_clock::now();
            const auto tt = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
            char timestamp[32];
            if (::gmtime_s(&tm, &tt) == 0) {
                std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%SZ", &tm);
            } else {
                // gmtime_s failed (returns errno_t, non-zero on failure).
                // Fall back to a tick-based stamp so the filename is still
                // unique across consecutive sessions even without wallclock.
                std::snprintf(timestamp, sizeof(timestamp), "unknown-time-%lld",
                              static_cast<long long>(tt));
            }

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
        // SINGLETON LIFETIME — this destructor runs from
        // ResetInstance() (auto-generated xrDestroyInstance), so the
        // application's threads are still alive and the writer thread can
        // drain cooperatively in <10 ms. The bounded-time guards in
        // CsvWriter::stop() (timed_mutex + WaitForSingleObject) are
        // therefore defence-in-depth for crash / hard-shutdown paths,
        // not the steady-state contract.
        //
        // Probe XrInstances (OpenComposite, OXR Toolkit, capability
        // checks) never reach xrEndFrame, so CsvWriter::stop() deletes
        // their empty CSVs rather than leaving header-only litter in the
        // sessions/ folder.
        ~OpenXrLayer() override {
            const uint64_t written = m_csv.written();
            m_csv.stop();
            if (m_csvPath.has_filename()) {
                if (written == 0) {
                    Log(fmt::format("xr_telemetry: probe XrInstance — no frames; csv deleted: {}\n",
                                    m_csvPath.string()));
                } else {
                    Log(fmt::format("xr_telemetry: session csv closed "
                                    "(written={}, dropped_try_lock={}, dropped_queue_full={}) "
                                    "at {}\n",
                                    written,
                                    m_csv.droppedTryLock(),
                                    m_csv.droppedQueueFull(),
                                    m_csvPath.string()));
                }
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
            if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider, "xrCreateInstance");

            // Names the loader hands us — application as declared in
            // XrApplicationInfo, plus the runtime that materialised
            // beneath us once the baseclass returns.
            const std::string appName = createInfo->applicationInfo.applicationName;
            Log(fmt::format("xr_telemetry {} starting for application '{}'\n",
                             VersionString, appName));

            const XrResult result = OpenXrApi::xrCreateInstance(createInfo);
            if (XR_FAILED(result)) {
                return result;
            }

            // Runtime identity. Useful in logs when supporting multiple
            // runtimes — anti-cheat reports often reference the runtime
            // by name. Skip gracefully if the runtime doesn't fill in
            // properties for any reason; we never want to fail the
            // host's xrCreateInstance because of our own diagnostics.
            XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
            if (XR_SUCCEEDED(OpenXrApi::xrGetInstanceProperties(GetXrInstance(), &instanceProperties))) {
                Log(fmt::format("Runtime: {} {}.{}.{}\n",
                                 instanceProperties.runtimeName,
                                 XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                 XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                 XR_VERSION_PATCH(instanceProperties.runtimeVersion)));
            }

            // Open the per-session CSV file. Failures here are non-fatal —
            // the layer keeps running as a pure pass-through if we can't
            // create the file (read-only LOCALAPPDATA, disk full, …). We
            // log the failure once via ErrorLog so post-mortem analysis can
            // still see it.
            m_csvPath = buildSessionCsvPath(appName);
            if (m_csv.start(m_csvPath)) {
                Log(fmt::format("xr_telemetry: writing per-frame CSV to {}\n", m_csvPath.string()));
            } else {
                ErrorLog(fmt::format("xr_telemetry: failed to open CSV at {} — telemetry disabled for this session\n",
                                     m_csvPath.string()));
            }

            // TODO(xr_telemetry): Decide here whether your layer
            // should be active for this app/runtime combination. If
            // not, set m_bypassApiLayer = true to make every other
            // override below a no-op pass-through. See CLAUDE.md
            // rule 9 — never crash the host because your layer can't
            // do its job; degrade to bypass instead.

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
            const int64_t tBegin = m_tBegin.load(std::memory_order_relaxed);
            const int64_t periodNs = m_predictedPeriodNs.load(std::memory_order_relaxed);
            const bool shouldRender = m_lastShouldRender.load(std::memory_order_relaxed);

            // First frame guard: xrEndFrame can fire before we ever observed
            // a successful xrWaitFrame. Skip the math to avoid bogus deltas.
            if (tWaitIn == 0 || tWaitOut == 0) {
                return result;
            }

            const int64_t waitBlockNs = QpcToNs(tWaitOut - tWaitIn);
            const int64_t appCpuNs = QpcToNs(tEnd - tWaitOut);
            // pre_begin_ns splits app_cpu_ns into the housekeeping window
            // (Wait→Begin) and the actual render submission (Begin→End).
            // Gated on tBegin > 0 because xrBeginFrame may be skipped on
            // some session-transition frames; report 0 then so the user
            // can filter.
            const int64_t preBeginNs =
                (tBegin > tWaitOut) ? QpcToNs(tBegin - tWaitOut) : 0;

            // Full-cycle duration: end-to-end wall clock of the previous
            // frame. This is what fpsVR / OpenXR Toolkit / PresentMon
            // report as "app frame time" because it includes the
            // post-xrEndFrame work (sim, physics, AI, input polling) that
            // appCpuNs above cannot see — the app does that work BEFORE
            // calling xrWaitFrame for the next frame, so it falls
            // outside our wait→end window. Gated on tEndPrev > 0 (first
            // frame of session): 0 means "no previous frame to compare".
            const int64_t tEndPrev = m_tEndPrev.exchange(tEnd, std::memory_order_relaxed);
            const int64_t frameTotalNs = (tEndPrev > 0) ? QpcToNs(tEnd - tEndPrev) : 0;

            // CPU headroom: fraction of the predicted display period that
            // is NOT spent on app CPU work. Uses the full-cycle metric
            // (frame_total - wait_block) so the number matches fpsVR /
            // OpenXR Toolkit — those subtract the compositor wait from
            // the cycle duration to get "all app CPU per cycle".
            //
            // Falls back to the wait→end window (appCpuNs) only on the
            // first frame where we don't have a previous cycle yet.
            //
            // Negative ⇒ app is CPU-bound this cycle. Gated on period > 0
            // because some runtimes briefly report 0 before the session
            // is fully ready.
            float headroomPct = 0.0f;
            if (periodNs > 0) {
                const int64_t appPerCycleNs =
                    (frameTotalNs > 0) ? (frameTotalNs - waitBlockNs) : appCpuNs;
                const double ratio =
                    static_cast<double>(appPerCycleNs) / static_cast<double>(periodNs);
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
                preBeginNs,
                appCpuNs,
                frameTotalNs,
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
        // Previous frame's tEnd (QPC ticks). Used by xrEndFrame to compute
        // frame_total_ns = tEnd - tEndPrev, which captures the post-end
        // app work (sim/physics/AI) that wait→end alone misses. Updated
        // via atomic exchange so the read+write is one operation.
        std::atomic<int64_t> m_tEndPrev{0};
        std::atomic<int64_t> m_predictedPeriodNs{0};
        std::atomic<bool> m_lastShouldRender{true};
        std::atomic<uint64_t> m_frameIndex{0};

        // Async CSV writer + the path we opened (for the destructor log).
        CsvWriter m_csv;
        std::filesystem::path m_csvPath;
    };

    // Singleton accessor used by framework/dispatch.cpp.
    //
    // IMPORTANT — uses the framework's g_instance (declared in
    // dispatch.gen.h, defined in dispatch.gen.cpp). The auto-generated
    // xrDestroyInstance calls ResetInstance() which does
    // `g_instance.reset()`, so each XrInstance gets a fresh OpenXrLayer
    // with a fresh CsvWriter, fresh thread, fresh state. ~OpenXrLayer()
    // therefore runs DURING the application's lifetime (threads alive,
    // mutexes sane), not at static-destruction time after ExitProcess()
    // has killed every other thread.
    //
    // Do NOT replace this with `static std::unique_ptr<OpenXrLayer>
    // instance = std::make_unique<OpenXrLayer>();` — that anti-pattern
    // makes the singleton process-lifetime and ResetInstance() a no-op,
    // which silently breaks the OpenComposite probe-then-real init flow
    // (the writer thread from the probe instance is still alive when
    // the real instance tries to start its own). See
    // docs/DEVELOPMENT.md in OpenXR-Layer-Template-Plus for the full
    // story.
    OpenXrApi* GetInstance() {
        if (!g_instance) {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

    // dllHome / localAppData are defined in framework/entry.cpp for the
    // DLL build, and in openxr-api-layer-tests/test_stubs.cpp for the
    // standalone test binary. Their declarations live in layer.h so
    // every TU that needs them just includes that.

} // namespace openxr_api_layer
