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
            int64_t end_frame_ns;      // OpenXrApi::xrEndFrame duration (runtime/
                                       //                       compositor overhead)
            int64_t frame_total_ns;    // tEnd - tEndPrev      (full cycle, includes
                                       //                       post-end sim/physics)
            int64_t gpu_time_ns;       // D3D11 timestamp delta (xrBeginFrame →
                                       //                       xrEndFrame on the GPU).
                                       //                       0 if no D3D11 binding or
                                       //                       result not yet available.
            int64_t period_ns;
            float headroom_pct;        // CPU headroom
            float gpu_headroom_pct;    // GPU headroom
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
                m_file << "frame,timestamp_qpc,wait_block_ns,pre_begin_ns,app_cpu_ns,"
                          "end_frame_ns,frame_total_ns,gpu_time_ns,period_ns,"
                          "headroom_pct,gpu_headroom_pct,should_render\n";
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
                        m_file << fmt::format("{},{},{},{},{},{},{},{},{},{:.2f},{:.2f},{}\n",
                                              rec.frame_index,
                                              rec.timestamp_qpc,
                                              rec.wait_block_ns,
                                              rec.pre_begin_ns,
                                              rec.app_cpu_ns,
                                              rec.end_frame_ns,
                                              rec.frame_total_ns,
                                              rec.gpu_time_ns,
                                              rec.period_ns,
                                              rec.headroom_pct,
                                              rec.gpu_headroom_pct,
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

        // -------------------------------------------------------------------
        // GpuTimer — D3D11 GPU-side timing for app frames.
        //
        // GPU timestamps are inherently asynchronous: a timestamp issued in
        // the command stream at xrBeginFrame_N's exit reflects the GPU's
        // wall clock at the moment that command actually runs on the GPU,
        // which is usually 1-3 frames after the CPU submitted it. So the
        // CPU thread can only READ a frame's GPU timings once the GPU is
        // done with that frame.
        //
        // We use a small ring of "query trios" (one D3D11_QUERY_TIMESTAMP_-
        // DISJOINT bracketing two D3D11_QUERY_TIMESTAMPs) to keep multiple
        // frames in flight on the GPU while we wait for results to land.
        // endFrameAndResolveOldest() is called from xrEndFrame:
        //
        //   1. Issue end-timestamp for the in-flight entry → entry full
        //   2. Advance the ring head
        //   3. Drain every slot whose GPU work has completed since the
        //      last poll, returning the MOST RECENTLY resolved result
        //      (older slots are still drained and their slot state is
        //      reset, but only the last one is returned — see the comment
        //      above the drain loop). On a clean steady state this means
        //      we return the slot that's exactly kRingSize-1 frames
        //      behind the producer; during a stutter several slots may
        //      resolve at once and only the latest is reported.
        //
        // The caller (OpenXrLayer::xrEndFrame) is responsible for matching
        // the resolved frame_index back to the FrameRecord queued at that
        // frame's xrEndFrame_N — see the m_pendingFrames deque on
        // OpenXrLayer below.
        //
        // If init() couldn't get a D3D11 device (Vulkan / OpenGL / null
        // binding / D3D12 not yet supported via D3D11On12), isActive()
        // stays false and beginFrame/queueAndResolve become no-ops that
        // return 0 — gpu_time_ns just reads as 0 in the CSV.
        struct GpuFrameResult {
            uint64_t frame_index;
            int64_t gpu_time_ns;
        };

        class GpuTimer {
          public:
            ~GpuTimer() {
                shutdown();
            }

            // device comes from XrGraphicsBindingD3D11KHR (in the app's
            // xrCreateSession.next chain — that struct only carries
            // ID3D11Device*, not the context). We pull the immediate
            // context from it via GetImmediateContext: it's the same
            // single-threaded context the app submits draws on, which is
            // what we want — D3D11 timestamp queries must be issued from
            // the same context that records the surrounding draws.
            //
            // Both device and context are AddRef'd by the ComPtr
            // assignments so the GPU can finish its frames even if the
            // app would otherwise release them right at xrDestroySession;
            // we release on shutdown().
            bool init(ID3D11Device* device) {
                if (!device) return false;
                if (m_active) {
                    // Idempotent like CsvWriter — but catch the spec
                    // violation "second xrCreateSession with a *different*
                    // ID3D11Device while the first session is still alive"
                    // in debug builds. Production-safe: we keep the
                    // first device and ignore the second silently.
                    assert(device == m_device.Get() &&
                           "GpuTimer::init called twice with different devices — "
                           "is the app calling xrCreateSession without "
                           "xrDestroySession in between?");
                    return true;
                }

                m_device = device;
                m_device->GetImmediateContext(m_context.ReleaseAndGetAddressOf());
                if (!m_context) {
                    m_device.Reset();
                    return false;
                }

                D3D11_QUERY_DESC disjointDesc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                D3D11_QUERY_DESC tsDesc{D3D11_QUERY_TIMESTAMP, 0};
                for (auto& slot : m_ring) {
                    if (FAILED(m_device->CreateQuery(&disjointDesc, slot.disjoint.GetAddressOf())) ||
                        FAILED(m_device->CreateQuery(&tsDesc, slot.start.GetAddressOf())) ||
                        FAILED(m_device->CreateQuery(&tsDesc, slot.end.GetAddressOf()))) {
                        // Some driver / debug-layer combination refused the
                        // query — degrade to "no GPU timing".
                        for (auto& s : m_ring) {
                            s.disjoint.Reset();
                            s.start.Reset();
                            s.end.Reset();
                        }
                        return false;
                    }
                    slot.state = SlotState::Idle;
                }
                m_writeIdx = 0;
                m_readIdx = 0;
                m_active = true;
                return true;
            }

            void shutdown() {
                if (!m_active) return;
                for (auto& slot : m_ring) {
                    slot.disjoint.Reset();
                    slot.start.Reset();
                    slot.end.Reset();
                    slot.state = SlotState::Idle;
                    slot.frame_index = 0;
                }
                m_context.Reset();
                m_device.Reset();
                m_active = false;
            }

            // Called from xrBeginFrame. Picks the next ring slot, issues
            // Begin(disjoint) and End(start). If the slot we want to use
            // is still in flight (the ring is full), we silently overwrite
            // it — the corresponding pending FrameRecord will get
            // gpu_time_ns = 0 because the resolve below won't see it.
            // Realistic with kRingSize=4: full ring only happens if the GPU
            // is >4 frames behind, in which case the app is dropping frames
            // anyway and a missing gpu_time on those frames is the least of
            // their problems.
            void beginFrame(uint64_t frame_index) {
                if (!m_active) return;
                auto& slot = m_ring[m_writeIdx];
                m_context->Begin(slot.disjoint.Get());
                m_context->End(slot.start.Get());
                slot.frame_index = frame_index;
                slot.state = SlotState::Started;
            }

            // Called from xrEndFrame, AFTER OpenXrApi::xrEndFrame returned.
            // Closes the in-flight slot's end-timestamp + disjoint, advances
            // the write head, then walks readIdx forward popping every slot
            // whose GPU work has finished. Returns the most-recently-resolved
            // result (or nullopt if no slot is ready this call).
            //
            // Multiple slots may resolve in one call after a stutter; we
            // currently return only the latest, which means brief stutters
            // produce a few CSV rows with gpu_time_ns=0 instead of holding
            // them back further. Good enough for V1; if it matters we'll
            // return std::vector<GpuFrameResult> later.
            std::optional<GpuFrameResult> endFrameAndResolveOldest() {
                if (!m_active) return std::nullopt;

                // Close the trio that beginFrame() opened. If the slot
                // is NOT Started (xrEndFrame fired without a matching
                // xrBeginFrame this cycle — spec violation, but we
                // tolerate it), we skip the End() calls but still advance
                // m_writeIdx. This keeps the ring's "one slot per
                // xrEndFrame call" cadence — the next xrBeginFrame will
                // open the next slot, not overwrite this dangling one.
                // The "self-healing" mode is asymmetric: after kRingSize
                // dangling cycles in a row, every slot is Idle again and
                // the next valid frame starts a fresh ring.
                auto& closing = m_ring[m_writeIdx];
                if (closing.state == SlotState::Started) {
                    m_context->End(closing.end.Get());
                    m_context->End(closing.disjoint.Get());
                    closing.state = SlotState::Pending;
                }
                m_writeIdx = (m_writeIdx + 1) % kRingSize;

                // Drain everything that's ready, keep the LAST result.
                std::optional<GpuFrameResult> latest;
                while (true) {
                    auto& slot = m_ring[m_readIdx];
                    if (slot.state != SlotState::Pending) break;

                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
                    const HRESULT hr = m_context->GetData(slot.disjoint.Get(),
                                                          &disjointData,
                                                          sizeof(disjointData),
                                                          D3D11_ASYNC_GETDATA_DONOTFLUSH);
                    if (hr != S_OK) break;  // not ready yet, stop draining

                    UINT64 tStart = 0, tEnd = 0;
                    if (m_context->GetData(slot.start.Get(), &tStart, sizeof(tStart),
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                        m_context->GetData(slot.end.Get(), &tEnd, sizeof(tEnd),
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
                        // Disjoint resolved but timestamps didn't — should
                        // not happen, but reset the slot and move on.
                        slot.state = SlotState::Idle;
                        m_readIdx = (m_readIdx + 1) % kRingSize;
                        continue;
                    }

                    int64_t gpuNs = 0;
                    if (!disjointData.Disjoint && disjointData.Frequency > 0 && tEnd > tStart) {
                        // (delta_ticks * 1e9) / Frequency without overflow.
                        const UINT64 delta = tEnd - tStart;
                        gpuNs = static_cast<int64_t>(
                            (delta / disjointData.Frequency) * 1'000'000'000ULL +
                            ((delta % disjointData.Frequency) * 1'000'000'000ULL) /
                                disjointData.Frequency);
                    }
                    latest = GpuFrameResult{slot.frame_index, gpuNs};
                    slot.state = SlotState::Idle;
                    m_readIdx = (m_readIdx + 1) % kRingSize;
                }
                return latest;
            }

            bool isActive() const { return m_active; }

          private:
            // Latency budget: at 90 Hz, 4 frames = ~44 ms — comfortably
            // above any realistic GPU→CPU result latency. Keep small to
            // limit pending CSV records the layer holds back at session
            // end.
            static constexpr size_t kRingSize = 4;

            enum class SlotState { Idle, Started, Pending };

            struct Slot {
                ComPtr<ID3D11Query> disjoint;
                ComPtr<ID3D11Query> start;
                ComPtr<ID3D11Query> end;
                uint64_t frame_index = 0;
                SlotState state = SlotState::Idle;
            };

            ComPtr<ID3D11Device> m_device;
            ComPtr<ID3D11DeviceContext> m_context;
            std::array<Slot, kRingSize> m_ring;
            size_t m_writeIdx = 0;
            size_t m_readIdx = 0;
            bool m_active = false;
        };

        // Walks the XrBaseInStructure-style `next` chain looking for the
        // first XrGraphicsBindingD3D11KHR. Returns nullptr if the app uses
        // a different graphics API (Vulkan, OpenGL, or D3D12 — the latter
        // is reachable via D3D11On12 in a follow-up commit).
        const XrGraphicsBindingD3D11KHR* findD3D11Binding(const void* nextChain) {
            const auto* base = reinterpret_cast<const XrBaseInStructure*>(nextChain);
            while (base) {
                if (base->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                    return reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(base);
                }
                base = base->next;
            }
            return nullptr;
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
            // If xrDestroySession was never called (e.g. host process is
            // tearing down without a graceful OpenXR shutdown), flush any
            // FrameRecord still waiting on a GPU result so they end up in
            // the CSV with gpu_time_ns=0 rather than disappearing.
            flushPendingFramesUnresolved();
            m_gpuTimer.shutdown();

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

        // ---- xrCreateSession ----------------------------------------------
        // Inspects createInfo->next for an XrGraphicsBindingD3D11KHR. If
        // present, stands up the D3D11 query ring used by xrBeginFrame /
        // xrEndFrame to measure app GPU time per frame. We never mutate
        // createInfo and we never fail the host's xrCreateSession over GPU-
        // timer init issues (CLAUDE.md rule 9: degrade gracefully). If the
        // app uses Vulkan, OpenGL, D3D12, or no binding at all, the timer
        // stays inactive and gpu_time_ns reads as 0 in the CSV.
        //
        // TODO V2.1: handle XrGraphicsBindingD3D12KHR via the D3D11On12
        // bridge already wired into pch.h.
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrCreateSession
        XrResult xrCreateSession(XrInstance instance,
                                 const XrSessionCreateInfo* createInfo,
                                 XrSession* session) override {
            const XrResult result = OpenXrApi::xrCreateSession(instance, createInfo, session);
            if (XR_FAILED(result) || !createInfo) {
                return result;
            }
            if (const auto* d3d11 = findD3D11Binding(createInfo->next)) {
                if (m_gpuTimer.init(d3d11->device)) {
                    Log("xr_telemetry: D3D11 GPU timer active\n");
                } else {
                    Log("xr_telemetry: D3D11 binding found but query creation failed; "
                        "gpu_time_ns will be 0 for this session\n");
                }
            } else {
                Log("xr_telemetry: no D3D11 binding in xrCreateSession.next (Vulkan / "
                    "OpenGL / D3D12 / null); gpu_time_ns will be 0 for this session\n");
            }
            return result;
        }

        // ---- xrDestroySession ---------------------------------------------
        // Tears down the D3D11 query objects so we don't leak. The framework
        // does NOT auto-handle xrDestroySession (the comment about that is
        // wrong; dispatch_generator.py only auto-handles xrCreateInstance,
        // xrDestroyInstance, xrGetInstanceProcAddr, and
        // xrEnumerateInstanceExtensionProperties). We have to override it
        // ourselves, like fov_crop does.
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrDestroySession
        XrResult xrDestroySession(XrSession session) override {
            // Flush any FrameRecords still waiting on a GPU result before
            // we release the queries. They'll be written with gpu_time_ns=0.
            flushPendingFramesUnresolved();
            m_gpuTimer.shutdown();
            return OpenXrApi::xrDestroySession(session);
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
        // Captured for two purposes:
        //   (1) tBegin sample → pre_begin_ns split (Wait→Begin housekeeping
        //       vs Begin→End render submission), used in CSV analysis.
        //   (2) Open the GPU timestamp window. The matching close happens in
        //       xrEndFrame; the resolved gpu_time_ns lands in a later CSV
        //       row (3-4 frame deferral due to GPU→CPU result latency).
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrBeginFrame
        XrResult xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) override {
            const int64_t tBegin = QpcNow();
            const XrResult result = OpenXrApi::xrBeginFrame(session, frameBeginInfo);
            if (XR_SUCCEEDED(result)) {
                m_tBegin.store(tBegin, std::memory_order_relaxed);
                // Only open a GPU timer slot if we have a valid wait pair.
                // Without one, xrEndFrame's first-frame guard skips the
                // FrameRecord push but our GpuTimer slot would still be
                // closed (recorded with frame_index = N), and the next
                // valid cycle would re-use the same m_frameIndex value N
                // when its End resolves — mis-attributing the previous
                // (skipped) cycle's GPU time to the next CPU record.
                //
                // Gating the open here on m_tWaitOut > 0 keeps GpuTimer
                // slots and FrameRecords in lockstep: both are only
                // produced once we've seen at least one successful
                // xrWaitFrame. The OpenXR spec forbids the wait-less
                // pattern anyway, but a hostile or buggy app would
                // otherwise silently shift gpu_time_ns by one frame.
                if (m_tWaitOut.load(std::memory_order_relaxed) > 0) {
                    // peek m_frameIndex without incrementing — xrEndFrame
                    // does the fetch_add and uses the SAME value.
                    m_gpuTimer.beginFrame(m_frameIndex.load(std::memory_order_relaxed));
                }
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

            // CLOSE THE GPU TIMESTAMP WINDOW BEFORE FORWARDING.
            //
            // GPU timestamps record the GPU's wall clock at the moment a
            // query command is executed in the command stream — they do
            // NOT depend on CPU-side timing. So the placement of
            // End(end)/End(disjoint) in the *stream* is what determines
            // what the gpu_time delta covers.
            //
            // If we issued these after OpenXrApi::xrEndFrame, the runtime
            // would have appended its own compositor / layer-composition
            // / projection-correction work to the stream first, and our
            // End commands would land AFTER it — folding the runtime's
            // GPU work into our app-frame measurement. On DR2 +
            // OpenComposite + Pimax OpenXR 0.1.0 that's ~7 ms of texture
            // copy and handoff, swamping the app's actual ~4 ms of work
            // and reporting ~0% GPU headroom instead of the ~64% an
            // OpenXR Toolkit overlay shows.
            //
            // Issuing them BEFORE the forward puts them in the stream
            // right after the app's last draw call, matching OXRT's
            // appGpuTimer.stop() position. The runtime's xrEndFrame
            // commands queue AFTER ours and are excluded from the
            // measurement.
            const auto resolved = m_gpuTimer.endFrameAndResolveOldest();

            const XrResult result = OpenXrApi::xrEndFrame(session, frameEndInfo);
            // end_frame_ns isolates the runtime + downstream layers' work
            // inside xrEndFrame (layer composition, projection correction,
            // compositor handoff). Useful for diagnosing runtime overhead —
            // young runtimes (e.g. Pimax OpenXR 0.1.0) can spend ~ms here
            // where mature compositors stay in the hundreds of µs.
            const int64_t tEndExit = QpcNow();
            const int64_t endFrameNs = QpcToNs(tEndExit - tEnd);

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
            // is fully ready — in that case we use 100% as the
            // "no measurement available" sentinel, matching the same
            // convention applied below for gpu_headroom_pct so analyses
            // can filter both columns the same way (`period_ns > 0`).
            float headroomPct = 100.0f;
            if (periodNs > 0) {
                const int64_t appPerCycleNs =
                    (frameTotalNs > 0) ? (frameTotalNs - waitBlockNs) : appCpuNs;
                const double ratio =
                    static_cast<double>(appPerCycleNs) / static_cast<double>(periodNs);
                headroomPct = static_cast<float>((1.0 - ratio) * 100.0);
            }
            const uint64_t frameIndex = m_frameIndex.fetch_add(1, std::memory_order_relaxed);

            // Build the FrameRecord for this frame. gpu_time_ns starts at
            // 0 (the GPU has barely started this frame's commands, let
            // alone finished them); patchAndDrainPending() overwrites both
            // it and gpu_headroom_pct once an older frame's GPU result
            // resolves.
            //
            // gpu_headroom_pct uses the standard formula with gpu_time_ns=0,
            // which yields 100%. This is what fpsVR / OpenXR Toolkit show
            // when GPU work isn't measured (e.g. the app uses Vulkan /
            // OpenGL / D3D12 / no binding — the GpuTimer stays inactive
            // and gpu_time_ns is 0 for every row of this session). The
            // alternative — a hardcoded 0.0 placeholder — read as "GPU
            // 100% saturated" to users comparing with OXRT, which was the
            // opposite of the intended semantics.
            //
            // Analyses that want to exclude unmeasured frames from GPU
            // statistics should filter `gpu_time_ns > 0`.
            // 100% when periodNs == 0 (same "unmeasured" sentinel as
            // headroomPct above); the formula's natural value with
            // gpu_time_ns=0 also yields 100%, so the two cases merge.
            const float gpuHeadroomPct = 100.0f;
            FrameRecord rec{
                frameIndex,
                tEnd,
                waitBlockNs,
                preBeginNs,
                appCpuNs,
                endFrameNs,
                frameTotalNs,
                /*gpu_time_ns=*/0,
                periodNs,
                headroomPct,
                gpuHeadroomPct,
                shouldRender,
            };

            // GPU timestamps were already closed BEFORE the OpenXrApi call
            // above (see the long comment up top). `resolved` is the
            // GpuFrameResult for whichever older frame's GPU work has
            // since finished — or std::nullopt if nothing is ready yet,
            // or the GpuTimer is inactive (no D3D11).
            //
            // Queue this frame's record and (if active) wait for GPU.
            // Pending deque grows by 1 each frame, shrinks by 1 each time
            // a result resolves. In steady state it stabilises at ~kRingSize
            // entries.
            if (m_gpuTimer.isActive()) {
                m_pendingFrames.push_back(rec);

                if (resolved.has_value()) {
                    patchAndDrainPending(resolved->frame_index, resolved->gpu_time_ns);
                }
            } else {
                // No GPU timer → no deferred path. Push immediately.
                m_csv.push(rec);
            }

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

        // GPU-side helpers ------------------------------------------------

        // The GpuTimer resolves frame N's GPU time in xrEndFrame_{N+latency}.
        // We hold each FrameRecord in m_pendingFrames between its own
        // xrEndFrame and the call where its GPU result arrives, then patch
        // the gpu_time_ns + gpu_headroom_pct in and push to the CsvWriter.
        //
        // Pending entries are FIFO and ordered by frame_index. When a result
        // for frame R arrives, every record at the front with frame_index
        // <= R is settled: the matching one gets the resolved gpu_time,
        // the older ones (if any — could happen on stutter) get 0.
        void patchAndDrainPending(uint64_t resolvedFrameIndex, int64_t gpuTimeNs) {
            while (!m_pendingFrames.empty()) {
                auto& front = m_pendingFrames.front();
                if (front.frame_index > resolvedFrameIndex) {
                    break;  // resolved older than everything pending — nothing to drain
                }
                if (front.frame_index == resolvedFrameIndex) {
                    front.gpu_time_ns = gpuTimeNs;
                    if (front.period_ns > 0) {
                        const double ratio =
                            static_cast<double>(gpuTimeNs) / static_cast<double>(front.period_ns);
                        front.gpu_headroom_pct = static_cast<float>((1.0 - ratio) * 100.0);
                    }
                }
                // Stale entries (frame_index < resolvedFrameIndex) keep
                // gpu_time_ns=0 — the ring overran them; their GPU work
                // result is gone. Push them anyway so the CSV stays
                // chronologically complete.
                m_csv.push(front);
                m_pendingFrames.pop_front();
                if (front.frame_index == resolvedFrameIndex) {
                    break;  // the matched frame is now flushed; stop here
                }
            }
        }

        // Called from xrDestroySession and from ~OpenXrLayer to flush
        // FrameRecords that never got a GPU result back (kRingSize-1 of
        // them in the steady state when the session ends, more on a stutter).
        // gpu_time_ns stays 0 — better an unmeasured row than a missing row.
        void flushPendingFramesUnresolved() {
            while (!m_pendingFrames.empty()) {
                m_csv.push(m_pendingFrames.front());
                m_pendingFrames.pop_front();
            }
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

        // GPU timing — single-threaded access from the frame-loop thread.
        // No atomics: every call into beginFrame / endFrameAndResolveOldest
        // is from xrBeginFrame / xrEndFrame, which the OpenXR spec
        // serialises within a session. The pending deque holds FrameRecords
        // waiting on a GPU result to land — typically kRingSize entries in
        // steady state, drains when xrDestroySession is called.
        GpuTimer m_gpuTimer;
        std::deque<FrameRecord> m_pendingFrames;
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
