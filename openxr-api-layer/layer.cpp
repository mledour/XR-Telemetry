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
#include "telemetry_internals.h"
#include "utils/name_utils.h"
#include "utils/overlay_aggregator.h"
#include "utils/settings.h"
#include <log.h>
#include <util.h>

namespace openxr_api_layer {

    using namespace log;
    // FrameRecord, qpcToNs, gpuTimestampDeltaToNs, gpuTimestampPairToNs,
    // computeCpuHeadroomPct, computeGpuHeadroomPct, patchAndDrainPending,
    // flushPendingFramesUnresolved, findInTypedChain all live in
    // openxr_api_layer::detail (telemetry_internals.h) so the
    // test binary can exercise the math + bookkeeping bits without faking
    // an OpenXR session. Bring the type alias in here so the rest of this
    // file doesn't have to spell out detail:: every time.
    using detail::FrameRecord;

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
                // Reset per-file stat counters so the footer reflects only
                // THIS recording window. Matters in mode=hotkey where the
                // same CsvWriter is started/stopped multiple times per
                // session — otherwise drop counts would accumulate across
                // windows and the footer of the Nth file would show the
                // sum of all preceding ones.
                m_written = 0;
                m_droppedTryLock.store(0, std::memory_order_relaxed);
                m_droppedQueueFull.store(0, std::memory_order_relaxed);
                m_droppedDiskWrite = 0;
                m_diskWriteFailedLogged.store(false, std::memory_order_relaxed);
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
            // `deleteIfEmpty=true` (the default) deletes the file if zero
            // frames were written — used to suppress empty CSVs from
            // OpenComposite / OXRT probe XrInstances that never reach
            // the frame loop. `deleteIfEmpty=false` ALWAYS keeps the
            // file — used by the hotkey toggle path, where an empty
            // recording is an intentional user action ("did my hotkey
            // even fire?") and the trace is the binding-validation
            // signal we want to surface.
            void stop(bool deleteIfEmpty = true) {
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
                    if (m_written == 0 && deleteIfEmpty) {
                        // No frames were written — this is almost certainly
                        // the OpenComposite / OXR Toolkit probe XrInstance
                        // (capability-check; never reaches the frame loop)
                        // OR an instance that was created+destroyed before
                        // any rendering started. Don't litter the sessions/
                        // folder with empty CSVs; close and delete.
                        m_file.close();
                        std::error_code ec;
                        std::filesystem::remove(m_path, ec);  // best-effort
                    } else if (m_written == 0) {
                        // Hotkey toggle with zero frames between press →
                        // stop. Keep the empty file (just the header +
                        // footer) so users can confirm their binding
                        // fired even when they tap-tap by mistake.
                        m_file << "# session_end written=0 (hotkey toggle, "
                                  "no frames captured)\n";
                        m_file.flush();
                        m_file.close();
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

        // Build %LOCALAPPDATA%\<layer>\sessions\YYYY-MM-DD_HH-MM-SS.mmmZ_<appName>.csv.
        // UTC (Z suffix) rather than local time: trace files are comparable
        // across machines and immune to DST transitions mid-session.
        //
        // Millisecond resolution matters in mode=hotkey: a user double-tap
        // (press → stop → press) lands two paths within the same second,
        // and CsvWriter::start opens with std::ios::trunc — without the
        // .mmm component the second press would overwrite the first
        // recording. The .003-ish resolution we get from system_clock on
        // Windows is plenty to disambiguate human inputs.
        //
        // As an extra safety net, if a path STILL collides (system clock
        // not monotonic across reboots, simulator firing back-to-back
        // calls within the same millisecond, NTP resync rewinding the
        // wallclock mid-bench, …) we suffix _2 / _3 / … until we find
        // a free slot. Caps at 999 attempts — well above any plausible
        // bench script burst across an NTP resync — and beyond that
        // we hand back a colliding name and let CsvWriter::start fail
        // with a logged error.
        //
        // appName is sanitised to keep only [A-Za-z0-9._-]; any other char →
        // '_'. Empty appName falls back to "unknown".
        std::filesystem::path buildSessionCsvPath(const std::string& appName) {
            const auto now = std::chrono::system_clock::now();
            const auto tt = std::chrono::system_clock::to_time_t(now);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now.time_since_epoch()).count() % 1000;
            std::tm tm{};
            char timestamp[40];
            if (::gmtime_s(&tm, &tt) == 0) {
                const size_t n = std::strftime(timestamp, sizeof(timestamp),
                                                "%Y-%m-%d_%H-%M-%S", &tm);
                std::snprintf(timestamp + n, sizeof(timestamp) - n,
                               ".%03lldZ", static_cast<long long>(ms));
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

            const auto baseStem = fmt::format("{}_{}", timestamp, safeName);
            auto candidate = dir / (baseStem + ".csv");
            for (int counter = 2; counter < 1000; ++counter) {
                if (!std::filesystem::exists(candidate, ec)) break;
                candidate = dir / fmt::format("{}_{}.csv", baseStem, counter);
            }
            return candidate;
        }

        // -------------------------------------------------------------------
        // Settings bootstrap + load. Mirrors the fov_crop sibling layer's
        // contract: an installer drops `settings.json` into the user's
        // %LOCALAPPDATA%\<layer>\ as a TEMPLATE, and on first run for each
        // OpenXR application we copy that template to <app>_settings.json
        // (the per-app file). Subsequent runs read the per-app file
        // directly; the template is never re-touched.
        //
        // Three failure paths, all non-fatal — telemetry must keep
        // working even if the filesystem misbehaves:
        //   - template missing (ZIP install, never ran installer) →
        //     writeDefaultTemplate() drops a copy of the schema below.
        //   - per-app file missing → bootstrap from template, or from
        //     built-in defaults if the template is also absent.
        //   - parse error → parseSettings() returns documented defaults
        //     and a non-empty error string we log once.
        // -------------------------------------------------------------------

        // The JSON body the runtime falls back to when neither the
        // installer-dropped template nor the per-app file exist. MUST stay
        // byte-for-byte aligned with installer/default_settings.json —
        // installer users and ZIP users must see identical first-run
        // behaviour. A drift here would mean a ZIP user's defaults
        // disagree with the documented schema.
        //
        // No automated drift check today; a future improvement would be
        // a build-time / test-time comparison that reads
        // installer/default_settings.json and assert-equals it against
        // this constexpr. Tracked informally — when overlay PR2 adds
        // more fields, update BOTH copies in the same commit.
        constexpr const char* kBuiltInDefaultSettings = R"({
  "_comment": "Default template for XR_APILAYER_MLEDOUR_xr_telemetry. Each OpenXR application gets a copy of this file the first time it runs (named <app>_settings.json next to this one). Edit values here to change the defaults that apply to NEW games; existing per-app files are never touched on subsequent runs.",
  "log": {
    "_comment": "Per-frame CSV capture. mode=auto opens a CSV at session start and closes it at session end (the original always-on behaviour). mode=hotkey keeps the CSV closed until the user presses the configured combo, then toggles open/closed on each subsequent press. The hotkey is polled once per frame inside xrEndFrame, so it only fires while the game has focus and is rendering OpenXR.",
    "enabled": true,
    "mode": "auto",
    "hotkey": {
      "_comment": "Recognised keys: A-Z, 0-9, F1-F24, Space, Tab, Enter, Escape, Backspace, Insert, Delete, Home, End, PageUp, PageDown, Up, Down, Left, Right. Punctuation is intentionally unsupported (locale-dependent). Recognised modifiers: ctrl, shift, alt, win. Unknown modifier names are ignored; an unknown key falls back to the documented default (Ctrl+Shift+T) so a typo never disables the hotkey entirely.",
      "key": "T",
      "modifiers": ["ctrl", "shift"]
    }
  },
  "overlay": {
    "_comment": "In-headset HUD showing fps / avg fps / cpu+gpu frametime / cpu+gpu utilisation %. Off by default (opt-in feature). mode=auto displays the HUD for the whole session whenever enabled=true; mode=hotkey leaves it hidden until the user presses the configured combo, then toggles on/off. refresh_hz controls how often the displayed numbers update (1-60 Hz, clamped); 10 Hz matches fpsvr and is readable in motion. position is reserved for tweaking the placement of the head-locked quad in future versions.",
    "enabled": false,
    "mode": "auto",
    "hotkey": {
      "_comment": "Distinct from the log hotkey so users running both features in mode=hotkey can drive them independently. Same key/modifier syntax as log.hotkey.",
      "key": "O",
      "modifiers": ["ctrl", "shift"]
    },
    "refresh_hz": 10,
    "position": "head_top_right"
  }
}
)";

        // Writes the built-in default JSON to `outputPath` via a
        // write-then-rename pattern: open .tmp sibling, write,
        // best-effort flush of the user-space buffer, rename over the
        // target. A mid-write disk-full / read-only failure leaves
        // the .tmp behind (best-effort cleanup) and returns false —
        // the on-disk `outputPath` either doesn't exist (fresh
        // install) or stays intact (we never overwrite a non-existing
        // target with a partial file). Without this, a torn first-run
        // write would leave a zero-byte settings.json that the next
        // run sees as "exists" and never re-tries.
        //
        // We do NOT call FlushFileBuffers / fsync — the std::ofstream
        // flush only drains user-space buffers, and adding a true
        // disk-level fsync would block bootstrap on the disk's commit
        // (10s of ms on spinning rust). A hardware crash after rename
        // but before kernel commit can still lose the file; in that
        // case the NEXT layer run regenerates it from the constexpr,
        // so we trade durability for a faster first frame.
        //
        // Caller-side contract: only called when outputPath does NOT
        // already exist (the bootstrap branch above checks). Rename-
        // to-existing fails on Windows; we don't bother with replace_
        // file_t semantics because we never have a target to replace.
        bool writeBuiltInSettings(const std::filesystem::path& outputPath) {
            const auto tmpPath =
                std::filesystem::path(outputPath.string() + ".tmp");
            {
                std::ofstream out(tmpPath, std::ios::out | std::ios::trunc);
                if (!out.is_open()) return false;
                out << kBuiltInDefaultSettings;
                out.flush();
                if (!out.good()) {
                    // Disk full / read-only mid-write. Drop the partial
                    // file so the next run can retry from scratch.
                    out.close();
                    std::error_code ec;
                    std::filesystem::remove(tmpPath, ec);
                    return false;
                }
            }
            std::error_code ec;
            std::filesystem::rename(tmpPath, outputPath, ec);
            if (ec) {
                std::error_code ec2;
                std::filesystem::remove(tmpPath, ec2);  // best-effort
                return false;
            }
            return true;
        }

        // Reads `path` into a string. Returns std::nullopt if the file is
        // absent or unreadable (the caller treats both as "use defaults").
        std::optional<std::string> readWholeFile(const std::filesystem::path& path) {
            std::ifstream in(path, std::ios::in | std::ios::binary);
            if (!in.is_open()) return std::nullopt;
            std::ostringstream ss;
            ss << in.rdbuf();
            if (!in.good() && !in.eof()) return std::nullopt;
            return ss.str();
        }

        // The full bootstrap-and-parse flow. Called once at xrCreateInstance
        // when we know the app name. Returns a parsed ParsedSettings that
        // the caller stores on OpenXrLayer.
        detail::ParsedSettings bootstrapAndLoadSettings(
            const std::filesystem::path& configDir,
            const std::string& appName) {

            std::error_code ec;
            std::filesystem::create_directories(configDir, ec);  // best-effort

            const std::filesystem::path templatePath = configDir / "settings.json";
            const std::filesystem::path perAppPath =
                openxr_api_layer::resolvePerAppConfigPath(configDir, appName);

            // 1. Ensure the global template exists. ZIP / dev installs
            //    bypass the Inno Setup script that drops it; rebuild it
            //    from the built-in default so the user has something to
            //    edit. Never overwrites an existing template — preserves
            //    any user edits to global defaults.
            if (!std::filesystem::exists(templatePath)) {
                if (writeBuiltInSettings(templatePath)) {
                    Log(fmt::format("xr_telemetry: created template at {}\n",
                                     templatePath.string()));
                }
            }

            // 2. Bootstrap the per-app file if missing. Prefer copying the
            //    template (so user edits to global defaults are inherited
            //    by every NEW app), fall back to the built-in if no
            //    template materialised.
            //
            //    The exists() check is racy w.r.t. another XR instance
            //    being launched in parallel for the same app (double-
            //    click, OpenXR Tools probe firing alongside the game,
            //    …). copy_options::skip_existing makes the copy_file
            //    call itself the arbiter: if the perAppPath already
            //    exists by the time the kernel processes our request,
            //    it returns without clobbering. The bootstrapped
            //    bool still flips true (skip_existing is not an
            //    error), and the subsequent read picks up whichever
            //    process won the race — same template content either
            //    way, so the user sees no difference.
            if (!std::filesystem::exists(perAppPath)) {
                bool bootstrapped = false;
                if (std::filesystem::exists(templatePath)) {
                    std::filesystem::copy_file(
                        templatePath, perAppPath,
                        std::filesystem::copy_options::skip_existing, ec);
                    if (!ec) {
                        bootstrapped = true;
                        Log(fmt::format("xr_telemetry: bootstrapped {} from template\n",
                                         perAppPath.string()));
                    }
                }
                if (!bootstrapped && writeBuiltInSettings(perAppPath)) {
                    Log(fmt::format("xr_telemetry: bootstrapped {} with built-in defaults\n",
                                     perAppPath.string()));
                }
            }

            // 3. Read + parse. Missing file (everything above failed) =
            //    parser returns documented defaults; we log it once.
            const auto fileContent = readWholeFile(perAppPath);
            if (!fileContent) {
                Log(fmt::format("xr_telemetry: settings file unreadable at {}, "
                                 "using built-in defaults\n", perAppPath.string()));
                return detail::parseSettings("");
            }
            auto parsed = detail::parseSettings(*fileContent);
            if (!parsed.error.empty()) {
                ErrorLog(fmt::format("xr_telemetry: settings parse error at {}: {} — "
                                      "using built-in defaults\n",
                                      perAppPath.string(), parsed.error));
            }
            return parsed;
        }

        // Polls the OS for the exact combo described by `spec`. Returns
        // true only when the main key is held AND the modifier state
        // matches bit-for-bit — pressing Ctrl+Alt+Shift+T must NOT fire a
        // Ctrl+Shift+T binding (or every accidental Alt-graveyard would
        // toggle the recorder). See the README's AltGr caveat: on
        // European layouts, AltGr is reported as Ctrl+Alt, so a hotkey
        // bound with `ctrl` is matched by AltGr-augmented presses too.
        //
        // GetAsyncKeyState is a process-global, foreground-independent
        // poll of the keyboard state. The DESIRED foreground-scoping
        // comes from WHERE we call it: only inside xrEndFrame, which
        // only fires while this game's render loop is active. A user
        // running an OpenXR mirror in the background while typing in
        // Notepad would still see the combo register; we accept that
        // edge case rather than wire up a Win32 focus check (which
        // would mis-classify HMD-focused sessions).
        bool pollHotkeyPressed(const detail::HotkeySpec& spec) {
            if (!spec.valid()) return false;
            auto down = [](int vk) {
                return (::GetAsyncKeyState(vk) & 0x8000) != 0;
            };
            if (!down(spec.vk)) return false;
            if (spec.ctrl != down(VK_CONTROL)) return false;
            if (spec.shift != down(VK_SHIFT)) return false;
            if (spec.alt != down(VK_MENU)) return false;  // VK_MENU = Alt
            const bool winDown = down(VK_LWIN) || down(VK_RWIN);
            if (spec.win != winDown) return false;
            return true;
        }

        // -------------------------------------------------------------------
        // GPU-side timing for app frames.
        //
        // GPU timestamps are inherently asynchronous: a timestamp issued in
        // the command stream at xrBeginFrame_N's exit reflects the GPU's
        // wall clock at the moment that command actually runs on the GPU,
        // which is usually 1-3 frames after the CPU submitted it. So the
        // CPU thread can only READ a frame's GPU timings once the GPU is
        // done with that frame.
        //
        // We support two paths, picked at xrCreateSession based on which
        // graphics binding the app provided:
        //
        //   D3D11GpuTimer — issues D3D11 timestamp queries (a disjoint pair
        //                   bracketing two D3D11_QUERY_TIMESTAMPs) on the
        //                   app's immediate context.
        //
        //   D3D12GpuTimer — records D3D12_QUERY_TYPE_TIMESTAMP queries on
        //                   our own short command lists, submitted to the
        //                   app's command queue alongside the app's draws.
        //                   No D3D11On12 wrapper — we go straight to D3D12
        //                   so there's no translation layer between us and
        //                   the queue, and no risk of the wrapper inserting
        //                   barriers in our measured range. Matches the
        //                   pattern used by OpenXR Toolkit / fpsVR.
        //
        // Both expose the same IGpuTimer interface so OpenXrLayer holds a
        // single unique_ptr<IGpuTimer> and routes Begin/End/Resolve
        // through it without caring about the underlying API.
        //
        // Each timer uses a small ring of slots (kGpuRingSize) to keep
        // multiple frames in flight. endFrameAndResolveOldest():
        //
        //   1. Closes the in-flight slot opened by beginFrame()
        //   2. Advances the ring head
        //   3. Drains every slot whose GPU work has completed since the
        //      last poll, returning the MOST RECENTLY resolved result.
        //      Older slots are still drained and their state reset, but
        //      only the latest is returned — brief stutters produce a few
        //      CSV rows with gpu_time_ns=0 instead of being held back
        //      further.
        //
        // The caller (OpenXrLayer::xrEndFrame) matches the resolved
        // frame_index back to the FrameRecord queued at that frame's
        // xrEndFrame_N — see the m_pendingFrames deque on OpenXrLayer.
        //
        // If no compatible graphics binding is found (Vulkan / OpenGL /
        // null), m_gpuTimer stays null and beginFrame/queueAndResolve are
        // simply not called — gpu_time_ns reads as 0 in the CSV.
        struct GpuFrameResult {
            uint64_t frame_index;
            int64_t gpu_time_ns;
        };

        // Common shape both backends implement so OpenXrLayer doesn't need
        // to branch on the graphics API at the frame-loop call sites.
        class IGpuTimer {
          public:
            virtual ~IGpuTimer() = default;
            virtual void beginFrame(uint64_t frame_index) = 0;
            virtual std::optional<GpuFrameResult> endFrameAndResolveOldest() = 0;
        };

        // Shared ring constants. kGpuRingSize=4 → ~44 ms in flight at 90 Hz,
        // comfortably above any realistic GPU→CPU latency. Keep small to
        // limit pending CSV records the layer holds back at session end.
        constexpr size_t kGpuRingSize = 4;
        enum class GpuSlotState { Idle, Started, Pending };

        // ---- D3D11 path -----------------------------------------------------

        class D3D11GpuTimer : public IGpuTimer {
          public:
            ~D3D11GpuTimer() override {
                shutdown();
            }

            // device comes from XrGraphicsBindingD3D11KHR. We pull the
            // immediate context from it via GetImmediateContext — same
            // single-threaded context the app submits draws on, so D3D11
            // timestamp queries land in command-stream order vs the draws
            // without any explicit Flush.
            bool init(ID3D11Device* device) {
                if (!device) return false;
                // init() is called once per session from
                // std::make_unique<D3D11GpuTimer> in xrCreateSession.
                // Catch double-init in debug rather than papering over
                // it with a silent no-op that would mask the bug in
                // release.
                assert(!m_active &&
                       "D3D11GpuTimer::init called twice — call site should "
                       "use std::make_unique to ensure a fresh timer.");

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
                        for (auto& s : m_ring) {
                            s.disjoint.Reset();
                            s.start.Reset();
                            s.end.Reset();
                        }
                        return false;
                    }
                    slot.state = GpuSlotState::Idle;
                }
                m_writeIdx = 0;
                m_readIdx = 0;
                m_active = true;
                return true;
            }

            void beginFrame(uint64_t frame_index) override {
                if (!m_active) return;
                auto& slot = m_ring[m_writeIdx];
                m_context->Begin(slot.disjoint.Get());
                m_context->End(slot.start.Get());
                slot.frame_index = frame_index;
                slot.state = GpuSlotState::Started;
            }

            std::optional<GpuFrameResult> endFrameAndResolveOldest() override {
                if (!m_active) return std::nullopt;

                // Close the trio that beginFrame() opened. If the slot is
                // NOT Started (xrEndFrame fired without a matching
                // xrBeginFrame this cycle — spec violation, but we
                // tolerate it), we skip the End() calls but still advance
                // m_writeIdx. Self-healing: after kGpuRingSize dangling
                // cycles every slot is Idle again.
                auto& closing = m_ring[m_writeIdx];
                if (closing.state == GpuSlotState::Started) {
                    m_context->End(closing.end.Get());
                    m_context->End(closing.disjoint.Get());
                    closing.state = GpuSlotState::Pending;
                }
                m_writeIdx = (m_writeIdx + 1) % kGpuRingSize;

                std::optional<GpuFrameResult> latest;
                while (true) {
                    auto& slot = m_ring[m_readIdx];
                    if (slot.state != GpuSlotState::Pending) break;

                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
                    const HRESULT hr = m_context->GetData(slot.disjoint.Get(),
                                                          &disjointData,
                                                          sizeof(disjointData),
                                                          D3D11_ASYNC_GETDATA_DONOTFLUSH);
                    if (hr != S_OK) break;

                    UINT64 tStart = 0, tEnd = 0;
                    if (m_context->GetData(slot.start.Get(), &tStart, sizeof(tStart),
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                        m_context->GetData(slot.end.Get(), &tEnd, sizeof(tEnd),
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
                        slot.state = GpuSlotState::Idle;
                        m_readIdx = (m_readIdx + 1) % kGpuRingSize;
                        continue;
                    }

                    // The Disjoint flag lives on the disjoint query, not
                    // on the timestamp pair, so it stays at the call site.
                    // freq==0 and tEnd<=tStart are handled by the helper
                    // (shared with the D3D12 path).
                    const int64_t gpuNs = disjointData.Disjoint
                        ? 0
                        : detail::gpuTimestampPairToNs(tStart, tEnd,
                                                       disjointData.Frequency);
                    latest = GpuFrameResult{slot.frame_index, gpuNs};
                    slot.state = GpuSlotState::Idle;
                    m_readIdx = (m_readIdx + 1) % kGpuRingSize;
                }
                return latest;
            }

          private:
            void shutdown() {
                if (!m_active) return;
                for (auto& slot : m_ring) {
                    slot.disjoint.Reset();
                    slot.start.Reset();
                    slot.end.Reset();
                    slot.state = GpuSlotState::Idle;
                    slot.frame_index = 0;
                }
                m_context.Reset();
                m_device.Reset();
                m_active = false;
            }

            struct Slot {
                ComPtr<ID3D11Query> disjoint;
                ComPtr<ID3D11Query> start;
                ComPtr<ID3D11Query> end;
                uint64_t frame_index = 0;
                GpuSlotState state = GpuSlotState::Idle;
            };

            ComPtr<ID3D11Device> m_device;
            ComPtr<ID3D11DeviceContext> m_context;
            std::array<Slot, kGpuRingSize> m_ring;
            size_t m_writeIdx = 0;
            size_t m_readIdx = 0;
            bool m_active = false;
        };

        // ---- D3D12 path -----------------------------------------------------

        // D3D12 native timing. Records two D3D12_QUERY_TYPE_TIMESTAMP queries
        // per frame on our own command lists, submitted to the app's command
        // queue. ResolveQueryData copies the timestamps into a readback
        // buffer; we Map() that buffer when the per-slot fence has signaled
        // past the relevant ExecuteCommandLists.
        //
        // Why two separate command lists per slot (one for the start
        // timestamp, one for the end + resolve) rather than a single one?
        // Because the app submits its draws DIRECTLY to the queue between
        // our two timestamps — we have to submit the start half BEFORE the
        // app's draws (at xrBeginFrame) and the end half AFTER (at
        // xrEndFrame). One ExecuteCommandLists each, so 2 extra submissions
        // per frame on the app's queue. Same cost class as the explicit
        // Flushes a D3D11On12 wrapper would force; no translation layer.
        //
        // D3D12 has no disjoint query. We get the GPU clock frequency once
        // at init via ID3D12CommandQueue::GetTimestampFrequency() and trust
        // it for the session — same convention as OpenXR Toolkit /
        // xrframetools.
        class D3D12GpuTimer : public IGpuTimer {
          public:
            ~D3D12GpuTimer() override {
                shutdown();
            }

            bool init(ID3D12Device* device, ID3D12CommandQueue* queue) {
                if (!device || !queue) return false;
                // The init function is only called once per session via
                // std::make_unique<D3D12GpuTimer> in xrCreateSession. Catch
                // any future double-init in debug rather than papering
                // over it with a silent no-op (which would mask the bug
                // in release).
                assert(!m_active &&
                       "D3D12GpuTimer::init called twice — call site should "
                       "use std::make_unique to ensure a fresh timer.");

                // Validate the queue is one we can ExecuteCommandLists
                // direct command lists onto. The OpenXR spec requires this
                // (XR_KHR_D3D12_enable: "queue must be a direct queue"),
                // but a buggy app could pass a compute / copy queue, in
                // which case the rest of our init would succeed and
                // ExecuteCommandLists would fail silently every frame.
                const D3D12_COMMAND_QUEUE_DESC queueDesc = queue->GetDesc();
                if (queueDesc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                    return false;
                }
                // Propagate the queue's NodeMask to every D3D12 object we
                // create. Hardcoding 0 (node 0) breaks silently on multi-
                // adapter rigs where the app put its queue on node 1+ —
                // ExecuteCommandLists would fail at runtime. 99% of VR rigs
                // are single-GPU so NodeMask is 1<<0 = 1 → we route to
                // node 0 anyway; the propagation is the defensive move.
                const UINT nodeMask = queueDesc.NodeMask;

                m_device = device;
                m_queue = queue;
                m_frequency = 0;
                if (FAILED(m_queue->GetTimestampFrequency(&m_frequency)) || m_frequency == 0) {
                    // The queue type doesn't support timestamps (copy
                    // queues historically didn't on some HW). Degrade.
                    m_queue.Reset();
                    m_device.Reset();
                    return false;
                }

                // Query heap: 2 timestamp slots per ring entry (start, end).
                D3D12_QUERY_HEAP_DESC qhDesc{};
                qhDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                qhDesc.Count = static_cast<UINT>(kGpuRingSize * 2);
                qhDesc.NodeMask = nodeMask;
                if (FAILED(m_device->CreateQueryHeap(&qhDesc, IID_PPV_ARGS(m_queryHeap.GetAddressOf())))) {
                    m_queue.Reset();
                    m_device.Reset();
                    return false;
                }

                // Readback buffer: UINT64 × 2 per slot.
                D3D12_HEAP_PROPERTIES heapProps{};
                heapProps.Type = D3D12_HEAP_TYPE_READBACK;
                heapProps.CreationNodeMask = nodeMask;
                heapProps.VisibleNodeMask = nodeMask;
                D3D12_RESOURCE_DESC bufDesc{};
                bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bufDesc.Width = sizeof(UINT64) * 2 * kGpuRingSize;
                bufDesc.Height = 1;
                bufDesc.DepthOrArraySize = 1;
                bufDesc.MipLevels = 1;
                bufDesc.Format = DXGI_FORMAT_UNKNOWN;
                bufDesc.SampleDesc.Count = 1;
                bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (FAILED(m_device->CreateCommittedResource(
                        &heapProps, D3D12_HEAP_FLAG_NONE,
                        &bufDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr, IID_PPV_ARGS(m_readback.GetAddressOf())))) {
                    shutdownInternal();
                    return false;
                }

                // Per-slot (allocator, command list) pairs — one for the
                // begin submission, one for the end+resolve submission.
                // Created closed so the first Reset() in beginFrame /
                // endFrame works. Both use the same NodeMask as the queue.
                for (auto& slot : m_ring) {
                    for (auto* pair : { &slot.beginPair, &slot.endPair }) {
                        if (FAILED(m_device->CreateCommandAllocator(
                                D3D12_COMMAND_LIST_TYPE_DIRECT,
                                IID_PPV_ARGS(pair->allocator.GetAddressOf()))) ||
                            FAILED(m_device->CreateCommandList(
                                nodeMask, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                pair->allocator.Get(), nullptr,
                                IID_PPV_ARGS(pair->cmdList.GetAddressOf())))) {
                            shutdownInternal();
                            return false;
                        }
                        pair->cmdList->Close();
                    }
                    slot.state = GpuSlotState::Idle;
                }

                if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                 IID_PPV_ARGS(m_fence.GetAddressOf())))) {
                    shutdownInternal();
                    return false;
                }
                m_fenceValue = 0;
                m_writeIdx = 0;
                m_readIdx = 0;
                m_active = true;
                return true;
            }

            void beginFrame(uint64_t frame_index) override {
                if (!m_active) return;
                auto& slot = m_ring[m_writeIdx];
                auto& pair = slot.beginPair;

                // Allocator reset requires the previous submission using
                // it to have completed on the GPU. In steady state at
                // kGpuRingSize frames behind this is always true, but be
                // explicit — the D3D12 debug layer flags violations
                // viciously and a single missed reset corrupts later
                // recordings.
                if (slot.fenceValueAtBegin > 0 &&
                    !waitForFenceBounded(slot.fenceValueAtBegin, /*timeoutMs=*/100)) {
                    // GPU is >100 ms behind on this slot — drop the begin
                    // for this frame, the slot stays whatever it was. The
                    // matching end will see state != Started and skip too.
                    //
                    // Contract with OpenXrLayer::m_pendingFrames: xrEndFrame
                    // unconditionally pushes a FrameRecord for `frame_index`,
                    // so the orphan stays in the pending deque until either
                    // a later frame's GpuFrameResult arrives (patchAndDrain
                    // Pending flushes everything older than the resolved
                    // index with gpu_time_ns=0) or xrDestroySession runs
                    // flushPendingFramesUnresolved. No leak, no stuck row.
                    return;
                }

                if (FAILED(pair.allocator->Reset()) ||
                    FAILED(pair.cmdList->Reset(pair.allocator.Get(), nullptr))) {
                    return;
                }
                const UINT startIdx = static_cast<UINT>(m_writeIdx * 2);
                pair.cmdList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, startIdx);
                if (FAILED(pair.cmdList->Close())) {
                    return;
                }
                ID3D12CommandList* lists[] = { pair.cmdList.Get() };
                m_queue->ExecuteCommandLists(1, lists);
                slot.fenceValueAtBegin = ++m_fenceValue;
                m_queue->Signal(m_fence.Get(), slot.fenceValueAtBegin);

                slot.frame_index = frame_index;
                slot.state = GpuSlotState::Started;
            }

            std::optional<GpuFrameResult> endFrameAndResolveOldest() override {
                if (!m_active) return std::nullopt;

                auto& closing = m_ring[m_writeIdx];
                if (closing.state == GpuSlotState::Started) {
                    auto& pair = closing.endPair;
                    if (closing.fenceValueAtEnd > 0 &&
                        !waitForFenceBounded(closing.fenceValueAtEnd, /*timeoutMs=*/100)) {
                        // Same degrade as beginFrame: skip the close, the
                        // slot becomes orphan — its data will never be
                        // resolved but the ring keeps cycling.
                        closing.state = GpuSlotState::Idle;
                    } else if (SUCCEEDED(pair.allocator->Reset()) &&
                               SUCCEEDED(pair.cmdList->Reset(pair.allocator.Get(), nullptr))) {
                        const UINT startIdx = static_cast<UINT>(m_writeIdx * 2);
                        const UINT endIdx = startIdx + 1;
                        pair.cmdList->EndQuery(m_queryHeap.Get(),
                                               D3D12_QUERY_TYPE_TIMESTAMP, endIdx);
                        // Copy this slot's two timestamps from the query
                        // heap into the readback buffer at our per-slot
                        // offset. ResolveQueryData is a GPU operation —
                        // we Map() the buffer once the fence below
                        // signals.
                        pair.cmdList->ResolveQueryData(
                            m_queryHeap.Get(),
                            D3D12_QUERY_TYPE_TIMESTAMP,
                            startIdx, /*NumQueries=*/2,
                            m_readback.Get(),
                            /*AlignedDestinationBufferOffset=*/sizeof(UINT64) * startIdx);
                        if (SUCCEEDED(pair.cmdList->Close())) {
                            ID3D12CommandList* lists[] = { pair.cmdList.Get() };
                            m_queue->ExecuteCommandLists(1, lists);
                            closing.fenceValueAtEnd = ++m_fenceValue;
                            m_queue->Signal(m_fence.Get(), closing.fenceValueAtEnd);
                            closing.state = GpuSlotState::Pending;
                        } else {
                            closing.state = GpuSlotState::Idle;
                        }
                    } else {
                        closing.state = GpuSlotState::Idle;
                    }
                }
                m_writeIdx = (m_writeIdx + 1) % kGpuRingSize;

                std::optional<GpuFrameResult> latest;
                while (true) {
                    auto& slot = m_ring[m_readIdx];
                    if (slot.state != GpuSlotState::Pending) break;
                    if (m_fence->GetCompletedValue() < slot.fenceValueAtEnd) break;

                    // Read the two timestamps from the readback buffer.
                    // D3D12_RANGE narrows the Map to only the bytes we
                    // actually need.
                    const SIZE_T begin = sizeof(UINT64) * m_readIdx * 2;
                    const D3D12_RANGE readRange{begin, begin + sizeof(UINT64) * 2};
                    void* mapped = nullptr;
                    UINT64 tStart = 0, tEnd = 0;
                    if (SUCCEEDED(m_readback->Map(0, &readRange, &mapped)) && mapped) {
                        const auto* ts = reinterpret_cast<const UINT64*>(
                            reinterpret_cast<const std::byte*>(mapped) + begin);
                        tStart = ts[0];
                        tEnd = ts[1];
                        const D3D12_RANGE noWrite{0, 0};
                        m_readback->Unmap(0, &noWrite);
                    }

                    // freq==0 and tEnd<=tStart (the WMR-driver out-of-order
                    // bug) are handled by the helper, shared with D3D11.
                    const int64_t gpuNs =
                        detail::gpuTimestampPairToNs(tStart, tEnd, m_frequency);
                    latest = GpuFrameResult{slot.frame_index, gpuNs};
                    slot.state = GpuSlotState::Idle;
                    m_readIdx = (m_readIdx + 1) % kGpuRingSize;
                }
                return latest;
            }

          private:
            void shutdown() {
                // Drain any in-flight GPU work before releasing query heap
                // and command objects — D3D12 spec is strict that
                // resources referenced by submitted command lists must
                // outlive their execution.
                if (m_active && m_fence && m_queue) {
                    const UINT64 v = ++m_fenceValue;
                    m_queue->Signal(m_fence.Get(), v);
                    waitForFenceBounded(v, /*timeoutMs=*/1000);
                }
                shutdownInternal();
            }

            void shutdownInternal() {
                for (auto& slot : m_ring) {
                    slot.beginPair.allocator.Reset();
                    slot.beginPair.cmdList.Reset();
                    slot.endPair.allocator.Reset();
                    slot.endPair.cmdList.Reset();
                    slot.state = GpuSlotState::Idle;
                    slot.frame_index = 0;
                    slot.fenceValueAtBegin = 0;
                    slot.fenceValueAtEnd = 0;
                }
                m_readback.Reset();
                m_queryHeap.Reset();
                m_fence.Reset();
                m_queue.Reset();
                m_device.Reset();
                m_active = false;
            }

            // Returns true if the fence reached `value` within timeoutMs,
            // false on timeout. Used both to ensure allocator reuse is
            // safe and to wait for resolve readback to be ready.
            bool waitForFenceBounded(UINT64 value, DWORD timeoutMs) {
                if (m_fence->GetCompletedValue() >= value) return true;
                wil::unique_event_nothrow ev;
                if (FAILED(ev.create())) return false;
                if (FAILED(m_fence->SetEventOnCompletion(value, ev.get()))) return false;
                return WaitForSingleObject(ev.get(), timeoutMs) == WAIT_OBJECT_0;
            }

            struct AllocatorListPair {
                ComPtr<ID3D12CommandAllocator> allocator;
                ComPtr<ID3D12GraphicsCommandList> cmdList;
            };
            struct Slot {
                AllocatorListPair beginPair;
                AllocatorListPair endPair;
                UINT64 fenceValueAtBegin = 0;
                UINT64 fenceValueAtEnd = 0;
                uint64_t frame_index = 0;
                GpuSlotState state = GpuSlotState::Idle;
            };

            ComPtr<ID3D12Device> m_device;
            ComPtr<ID3D12CommandQueue> m_queue;
            ComPtr<ID3D12QueryHeap> m_queryHeap;
            ComPtr<ID3D12Resource> m_readback;
            ComPtr<ID3D12Fence> m_fence;
            UINT64 m_fenceValue = 0;
            UINT64 m_frequency = 0;
            std::array<Slot, kGpuRingSize> m_ring;
            size_t m_writeIdx = 0;
            size_t m_readIdx = 0;
            bool m_active = false;
        };

        // Walks the XrBaseInStructure-style `next` chain looking for the
        // first XrGraphicsBindingD3D11KHR. Returns nullptr if the app uses
        // a different graphics API (Vulkan, OpenGL, D3D12).
        //
        // The actual walk lives in detail::findInTypedChain (header-only,
        // pure pointer arithmetic) so a unit test can exercise it without
        // pulling in the OpenXR/D3D headers — see test_telemetry_math.cpp.
        const XrGraphicsBindingD3D11KHR* findD3D11Binding(const void* nextChain) {
            return reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(
                detail::findInTypedChain(nextChain, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR));
        }

        // Same walk for XrGraphicsBindingD3D12KHR. The struct carries an
        // ID3D12Device* AND an ID3D12CommandQueue* (unlike the D3D11
        // binding which is just the device) because D3D12 has no
        // immediate context — the app submits commands to a queue it
        // owns. We need that queue handle so D3D12GpuTimer can
        // ExecuteCommandLists onto the same stream the app draws on.
        const XrGraphicsBindingD3D12KHR* findD3D12Binding(const void* nextChain) {
            return reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(
                detail::findInTypedChain(nextChain, XR_TYPE_GRAPHICS_BINDING_D3D12_KHR));
        }

    }  // anonymous namespace


    class OpenXrLayer : public OpenXrApi {
    public:
        OpenXrLayer() {
            // Cache QPC frequency once. Used by every frame-thread timestamp
            // conversion below; constant for the process lifetime.
            //
            // Fallback to 10 MHz (the typical Windows QPC frequency on
            // modern hardware) rather than 1 if QueryPerformanceFrequency
            // ever returns 0. qpcToNs interprets `ticks / freq` as
            // seconds, so freq=1 would make every QPC delta blow up to
            // ~1e9× the intended nanosecond value — corrupting the CSV,
            // the headroom math, AND the overlay aggregator's refresh
            // deadline. 10 MHz is a sane, realistic guess that keeps the
            // numbers in the right ballpark even on a broken HAL clock.
            // The aggregator has its own < 1000 clamp as defense in depth.
            LARGE_INTEGER freq{};
            QueryPerformanceFrequency(&freq);
            m_qpcFrequency = freq.QuadPart > 0 ? freq.QuadPart : 10'000'000LL;
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
            m_gpuTimer.reset();

            const uint64_t written = m_csv.written();
            // Auto mode: keep the existing probe-instance protection
            // (delete an empty CSV from OpenComposite / OXRT capability
            // probes). Hotkey mode: any open CSV at this point came
            // from an explicit user press, so KEEP it on disk regardless
            // of frame count — it's the user-visible signal that their
            // binding worked.
            const bool deleteIfEmpty =
                m_settings.log.mode == detail::LogMode::Auto;
            m_csv.stop(deleteIfEmpty);
            if (m_csvPath.has_filename()) {
                if (written == 0 && deleteIfEmpty) {
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

            // PR1 verification: log the aggregator's final snapshot once
            // per xrInstance lifetime. Moved here from xrDestroySession
            // because OpenComposite's probe pattern fires xrDestroy
            // Session BEFORE the real gameplay session has accumulated
            // any frames — logging there produced a "session too short"
            // message even when the real session ran for minutes. The
            // destructor sees the cumulative aggregator state across
            // every session of this instance, which is what we want.
            //
            // The Log() uses the same "%s" + fmt::format(...).c_str()
            // dance as the CSV log above to dodge vsnprintf's printf-
            // style re-interpretation of "% util" inside the formatted
            // string (verified bug from an earlier build that read
            // "cpu=… (832588922944til)" instead of "(47% util)").
            if (m_overlayActive) {
                const auto& snap = m_overlay.snapshot();
                if (snap.valid) {
                    const std::string msg = fmt::format(
                        "xr_telemetry: overlay final snapshot — "
                        "fps={:.1f} (avg {:.1f}, target {:.1f}), "
                        "cpu={:.2f} ms ({:.0f}% util), "
                        "gpu={:.2f} ms ({:.0f}% util)\n",
                        snap.fps_instant, snap.fps_avg, snap.target_fps,
                        snap.cpu_frame_ms, snap.cpu_utilisation_pct,
                        snap.gpu_frame_ms, snap.gpu_utilisation_pct);
                    Log("%s", msg.c_str());
                } else {
                    Log("xr_telemetry: overlay was active but session too short for a "
                        "snapshot to finalise (need at least one refresh interval of frames)\n");
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

            // Stash the application name — we need it later both for
            // building CSV paths (auto AND hotkey modes derive the file
            // name from it) and for the log lines that announce mode
            // transitions.
            m_appName = appName;

            // Load the per-app settings file (bootstrap from the global
            // template if missing). bootstrapAndLoadSettings always
            // returns a usable struct, even on filesystem or parse
            // failure — defaults preserve the original always-on
            // behaviour.
            const auto parsed = bootstrapAndLoadSettings(
                openxr_api_layer::localAppData, appName);
            m_settings = parsed.settings;

            // Master kill switch. CLAUDE.md rule 9 (graceful degradation):
            // if BOTH features are disabled in settings, the layer has
            // nothing to do for this app — flip m_bypassApiLayer so every
            // per-frame override forwards through the base class. We
            // bypass on the AND of the two flags (not just log) so a
            // user can run overlay-only without the layer disappearing.
            if (!m_settings.log.enabled && !m_settings.overlay.enabled) {
                m_bypassApiLayer = true;
                Log("xr_telemetry: both log.enabled and overlay.enabled are false in "
                    "settings — layer running as pass-through for this session\n");
                return result;
            }

            // ---- log feature -------------------------------------------
            // Auto mode: open the CSV right now and let xrEndFrame append
            // rows until xrDestroySession closes it. Hotkey mode: keep
            // the CSV closed until the user presses the configured combo.
            // If log.enabled=false but overlay.enabled=true, we skip both
            // branches — the per-frame instrumentation still runs (to
            // feed the overlay aggregator) but never writes to disk.
            if (m_settings.log.enabled) {
                if (m_settings.log.mode == detail::LogMode::Auto) {
                    m_csvPath = buildSessionCsvPath(appName);
                    if (m_csv.start(m_csvPath)) {
                        m_recording = true;
                        Log(fmt::format("xr_telemetry: log mode=auto, writing per-frame CSV to {}\n",
                                         m_csvPath.string()));
                    } else {
                        ErrorLog(fmt::format("xr_telemetry: failed to open CSV at {} — "
                                              "log disabled for this session\n",
                                              m_csvPath.string()));
                    }
                } else {
                    Log(fmt::format("xr_telemetry: log mode=hotkey, press {} to start/stop "
                                     "recording (per-frame CSV opens on the next press)\n",
                                     detail::formatHotkey(m_settings.log.hotkey)));
                }
            } else {
                Log("xr_telemetry: log.enabled=false — no per-frame CSV will be written "
                    "this session (overlay still active if its own enabled flag is set)\n");
            }

            // ---- overlay feature ---------------------------------------
            // The aggregator is constructed up-front so PR2's renderer
            // can call snapshot() unconditionally. It needs the real
            // QPC frequency (cached in the constructor) and the user-
            // configured refresh cadence converted to nanoseconds.
            const int64_t overlayIntervalNs =
                1'000'000'000LL / std::max(1, m_settings.overlay.refresh_hz);
            m_overlay = detail::OverlayAggregator(overlayIntervalNs, m_qpcFrequency);

            if (m_settings.overlay.enabled) {
                if (m_settings.overlay.mode == detail::OverlayMode::Auto) {
                    m_overlayActive = true;
                    Log(fmt::format("xr_telemetry: overlay mode=auto, refreshing snapshot "
                                     "at {} Hz (PR1 plumbing only — rendering ships in PR2)\n",
                                     m_settings.overlay.refresh_hz));
                } else {
                    Log(fmt::format("xr_telemetry: overlay mode=hotkey, press {} to toggle "
                                     "(refreshes at {} Hz when active)\n",
                                     detail::formatHotkey(m_settings.overlay.hotkey),
                                     m_settings.overlay.refresh_hz));
                }
            }

            return result;
        }

        // ---- xrCreateSession ----------------------------------------------
        // Inspects createInfo->next for a graphics binding we support and
        // stands up the matching GPU timer:
        //
        //   D3D11 → D3D11GpuTimer (D3D11 timestamp queries on the app's
        //           immediate context)
        //   D3D12 → D3D12GpuTimer (native D3D12_QUERY_TYPE_TIMESTAMP on the
        //           app's command queue; no D3D11On12 bridge)
        //   any other (Vulkan / OpenGL / null) → no timer, gpu_time_ns
        //           reads as 0 in the CSV.
        //
        // We never mutate createInfo and we never fail the host's
        // xrCreateSession over GPU-timer init issues (CLAUDE.md rule 9:
        // degrade gracefully).
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrCreateSession
        XrResult xrCreateSession(XrInstance instance,
                                 const XrSessionCreateInfo* createInfo,
                                 XrSession* session) override {
            const XrResult result = OpenXrApi::xrCreateSession(instance, createInfo, session);
            // log.enabled=false → don't stand up the GPU timer either.
            // The session is created normally by the base call above;
            // we just skip our own instrumentation hooks.
            if (m_bypassApiLayer || XR_FAILED(result) || !createInfo) {
                return result;
            }
            if (const auto* d3d11 = findD3D11Binding(createInfo->next)) {
                auto timer = std::make_unique<D3D11GpuTimer>();
                if (timer->init(d3d11->device)) {
                    m_gpuTimer = std::move(timer);
                    Log("xr_telemetry: D3D11 GPU timer active\n");
                } else {
                    Log("xr_telemetry: D3D11 binding found but query creation failed; "
                        "gpu_time_ns will be 0 for this session\n");
                }
            } else if (const auto* d3d12 = findD3D12Binding(createInfo->next)) {
                auto timer = std::make_unique<D3D12GpuTimer>();
                if (timer->init(d3d12->device, d3d12->queue)) {
                    m_gpuTimer = std::move(timer);
                    Log("xr_telemetry: D3D12 GPU timer active (native query heap)\n");
                } else {
                    Log("xr_telemetry: D3D12 binding found but query / fence / command-list "
                        "creation failed; gpu_time_ns will be 0 for this session\n");
                }
            } else {
                Log("xr_telemetry: no D3D11 or D3D12 binding in xrCreateSession.next "
                    "(Vulkan / OpenGL / null); gpu_time_ns will be 0 for this session\n");
            }
            return result;
        }

        // ---- xrDestroySession ---------------------------------------------
        // Tears down the GPU timer so we don't leak query heaps / command
        // lists / D3D objects. The framework does NOT auto-handle
        // xrDestroySession (only xrCreateInstance, xrDestroyInstance,
        // xrGetInstanceProcAddr, and xrEnumerateInstanceExtensionProperties
        // are routed automatically by dispatch_generator.py). We have to
        // override it ourselves, like fov_crop does.
        // https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrDestroySession
        XrResult xrDestroySession(XrSession session) override {
            if (m_bypassApiLayer) {
                return OpenXrApi::xrDestroySession(session);
            }
            // Session-scoped cleanup only. Flush any FrameRecords still
            // waiting on a GPU result before we release the queries
            // (they'll land in the CSV with gpu_time_ns=0), then drop
            // the D3D timer so its query heap / command lists / fence
            // don't outlive the dying session's device.
            flushPendingFramesUnresolved();
            m_gpuTimer.reset();
            // DO NOT touch m_csv / m_recording / m_overlay / m_overlay
            // Active here. The OpenXR spec allows multiple xrCreateSession
            // / xrDestroySession cycles within a single xrInstance, and
            // OpenComposite uses exactly that pattern: a probe session
            // first (capability check, no frame loop), then the real
            // gameplay session. If we closed the CsvWriter at the probe's
            // xrDestroySession, the real session's frames would land in
            // a now-closed writer (m_csv.push() silently no-ops on
            // !m_started), and the user would see "no frames; csv
            // deleted" in the log — exactly the bug live-confirmed on
            // OpenComposite + LMU. The CsvWriter, m_recording, and the
            // overlay snapshot log all live for the xrInstance's full
            // lifetime; they're torn down by ~OpenXrLayer at
            // xrDestroyInstance instead.
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
            if (m_bypassApiLayer) {
                return OpenXrApi::xrWaitFrame(session, frameWaitInfo, frameState);
            }
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
            if (m_bypassApiLayer) {
                return OpenXrApi::xrBeginFrame(session, frameBeginInfo);
            }
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
                if (m_gpuTimer && m_tWaitOut.load(std::memory_order_relaxed) > 0) {
                    // peek m_frameIndex without incrementing — xrEndFrame
                    // does the fetch_add and uses the SAME value.
                    m_gpuTimer->beginFrame(m_frameIndex.load(std::memory_order_relaxed));
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
            if (m_bypassApiLayer) {
                return OpenXrApi::xrEndFrame(session, frameEndInfo);
            }

            // Hotkey mode: poll the configured combo once per frame, BEFORE
            // any timing capture. The edge detector returns true exactly
            // on the low→high transition, so a held key fires once even
            // if the game runs at 120 Hz. Toggling the recorder is cheap
            // — open/close start/stop the writer thread for that window.
            //
            // Edge cases handled by callees:
            //   - m_csv.start() returning false (read-only LOCALAPPDATA,
            //     disk full) leaves m_recording = false and the next
            //     press will try again. We log once per failure so a
            //     broken setup is visible in the support log.
            //   - m_csv.stop() during a pending GPU result: the
            //     resolved FrameRecord drains into a now-closed
            //     CsvWriter and push() silently no-ops. No leak.
            if (m_settings.log.enabled &&
                m_settings.log.mode == detail::LogMode::Hotkey) {
                if (m_hotkeyDetector.tick(pollHotkeyPressed(m_settings.log.hotkey))) {
                    if (!m_recording) {
                        m_csvPath = buildSessionCsvPath(m_appName);
                        if (m_csv.start(m_csvPath)) {
                            m_recording = true;
                            Log(fmt::format("xr_telemetry: hotkey pressed — "
                                             "recording started, writing to {}\n",
                                             m_csvPath.string()));
                        } else {
                            ErrorLog(fmt::format("xr_telemetry: hotkey pressed but "
                                                  "could not open {} — recording NOT started\n",
                                                  m_csvPath.string()));
                        }
                    } else {
                        const uint64_t written = m_csv.written();
                        // deleteIfEmpty=false: in hotkey mode, an empty
                        // recording is the user's explicit signal "did my
                        // binding fire?" — keep the header+footer file so
                        // they can confirm in the sessions/ folder.
                        m_csv.stop(/*deleteIfEmpty=*/false);
                        m_recording = false;
                        Log(fmt::format("xr_telemetry: hotkey pressed — "
                                         "recording stopped ({} frames written to {})\n",
                                         written, m_csvPath.string()));
                    }
                }
            }

            // Same rising-edge pattern for the overlay hotkey. Independent
            // from the log hotkey: a user can have both running in mode=
            // hotkey with separate combos. m_overlayHotkeyDetector keeps
            // its own latch so a press for one doesn't fire the other.
            //
            // No file I/O — overlay is purely in-memory state in PR1.
            // The Log() on toggle is on a user-initiated rising edge
            // (not every frame), so the per-frame DBWinMutex concern
            // documented above doesn't apply.
            if (m_settings.overlay.enabled &&
                m_settings.overlay.mode == detail::OverlayMode::Hotkey) {
                if (m_overlayHotkeyDetector.tick(
                        pollHotkeyPressed(m_settings.overlay.hotkey))) {
                    m_overlayActive = !m_overlayActive;
                    Log(fmt::format("xr_telemetry: hotkey pressed — overlay {}\n",
                                     m_overlayActive ? "ENABLED" : "DISABLED"));
                }
            }

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
            const auto resolved =
                m_gpuTimer ? m_gpuTimer->endFrameAndResolveOldest() : std::nullopt;

            const XrResult result = OpenXrApi::xrEndFrame(session, frameEndInfo);
            // end_frame_ns isolates the runtime + downstream layers' work
            // inside xrEndFrame (layer composition, projection correction,
            // compositor handoff). Useful for diagnosing runtime overhead —
            // young runtimes (e.g. Pimax OpenXR 0.1.0) can spend ~ms here
            // where mature compositors stay in the hundreds of µs.
            const int64_t tEndExit = QpcNow();
            const int64_t endFrameNs = detail::qpcToNs(tEndExit - tEnd, m_qpcFrequency);

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

            const int64_t waitBlockNs = detail::qpcToNs(tWaitOut - tWaitIn, m_qpcFrequency);
            const int64_t appCpuNs = detail::qpcToNs(tEnd - tWaitOut, m_qpcFrequency);
            // pre_begin_ns splits app_cpu_ns into the housekeeping window
            // (Wait→Begin) and the actual render submission (Begin→End).
            // Gated on tBegin > 0 because xrBeginFrame may be skipped on
            // some session-transition frames; report 0 then so the user
            // can filter.
            const int64_t preBeginNs =
                (tBegin > tWaitOut) ? detail::qpcToNs(tBegin - tWaitOut, m_qpcFrequency) : 0;

            // Full-cycle duration: end-to-end wall clock of the previous
            // frame. This is what fpsVR / OpenXR Toolkit / PresentMon
            // report as "app frame time" because it includes the
            // post-xrEndFrame work (sim, physics, AI, input polling) that
            // appCpuNs above cannot see — the app does that work BEFORE
            // calling xrWaitFrame for the next frame, so it falls
            // outside our wait→end window. Gated on tEndPrev > 0 (first
            // frame of session): 0 means "no previous frame to compare".
            const int64_t tEndPrev = m_tEndPrev.exchange(tEnd, std::memory_order_relaxed);
            const int64_t frameTotalNs = (tEndPrev > 0) ? detail::qpcToNs(tEnd - tEndPrev, m_qpcFrequency) : 0;

            // CPU headroom: fraction of the predicted display period that
            // is NOT spent on app CPU work. Uses the full-cycle metric
            // (frame_total - wait_block) so the number matches fpsVR /
            // OpenXR Toolkit — those subtract the compositor wait from
            // the cycle duration to get "all app CPU per cycle".
            //
            // Falls back to the wait→end window (appCpuNs) only on the
            // first frame where we don't have a previous cycle yet.
            //
            // The formula + period <= 0 sentinel live in detail::
            // (telemetry_internals.h) so they're independently unit-
            // tested.
            const int64_t appPerCycleNs =
                (frameTotalNs > 0) ? (frameTotalNs - waitBlockNs) : appCpuNs;
            const float headroomPct = detail::computeCpuHeadroomPct(appPerCycleNs, periodNs);
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
            // gpu_time_ns starts at 0 (the GPU hasn't finished this frame
            // yet). detail::computeGpuHeadroomPct with gpuTimeNs=0 yields
            // 100% — the standard "unmeasured" sentinel — and also handles
            // the periodNs == 0 transient with the same value. If a GPU
            // result lands later, patchAndDrainPending() recomputes the
            // pct against the resolved gpu_time_ns.
            const float gpuHeadroomPct = detail::computeGpuHeadroomPct(/*gpuTimeNs=*/0, periodNs);
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
            // or m_gpuTimer is null (no supported binding).
            //
            // Queue this frame's record and (if active) wait for GPU.
            // Pending deque grows by 1 each frame, shrinks by 1 each time
            // a result resolves. In steady state it stabilises at
            // ~kGpuRingSize entries.
            //
            // The drain sink fans the FULLY-RESOLVED FrameRecord out to
            // every consumer: CsvWriter when m_recording, OverlayAggregator
            // when m_overlayActive. Both gates are checked here (not
            // inside the sinks) so a closed CSV / disabled overlay stays
            // free of even the inner method-call overhead. The aggregator
            // wants the final gpu_time_ns, not the placeholder 0 — that's
            // exactly what patchAndDrainPending hands us once the GPU
            // result arrives.
            if (m_gpuTimer) {
                m_pendingFrames.push_back(rec);

                if (resolved.has_value()) {
                    patchAndDrainPending(resolved->frame_index, resolved->gpu_time_ns);
                }
            } else {
                // No GPU timer → no deferred path. Push immediately,
                // gpu_time_ns stays at 0 (which the aggregator clamps
                // back to 100% headroom via the helper formula).
                if (m_recording)     m_csv.push(rec);
                if (m_overlayActive) m_overlay.pushFrame(rec);
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
        // Thin wrappers around the pure templates in telemetry_internals.h —
        // the actual drain/patch/flush logic is unit-tested without an
        // OpenXR session in test_telemetry_math.cpp.
        //
        // The sink fans the resolved FrameRecord to BOTH consumers:
        //   - CsvWriter, gated on m_recording (mode=auto active OR
        //     mode=hotkey toggled on).
        //   - OverlayAggregator, gated on m_overlayActive.
        // Each gate avoids the empty-virtual-call cost when the
        // corresponding feature is dormant.
        void fanoutRecord(const FrameRecord& r) {
            if (m_recording)     m_csv.push(r);
            if (m_overlayActive) m_overlay.pushFrame(r);
        }

        void patchAndDrainPending(uint64_t resolvedFrameIndex, int64_t gpuTimeNs) {
            detail::patchAndDrainPending(
                m_pendingFrames, resolvedFrameIndex, gpuTimeNs,
                [this](const FrameRecord& r) { fanoutRecord(r); });
        }

        void flushPendingFramesUnresolved() {
            detail::flushPendingFramesUnresolved(
                m_pendingFrames, [this](const FrameRecord& r) { fanoutRecord(r); });
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
        // In mode=hotkey, m_csvPath is re-derived on each rising edge of
        // the hotkey so every recording window gets its own timestamped
        // file under sessions/.
        CsvWriter m_csv;
        std::filesystem::path m_csvPath;

        // Settings + per-app application name, loaded once at
        // xrCreateInstance. m_settings is read-only after init (we don't
        // hot-reload — see the README's "live_edit deliberately omitted"
        // note). m_appName drives both the CSV filename slug and the
        // hotkey log messages.
        detail::TelemetrySettings m_settings;
        std::string m_appName;

        // Hotkey-mode bookkeeping. m_hotkeyDetector rising-edge-filters
        // pollHotkeyPressed() so a held key fires the toggle exactly
        // once. m_recording tracks whether the CsvWriter is currently
        // open — true in auto-mode for the whole session, true between
        // press(N) and press(N+1) in hotkey-mode.
        detail::HotkeyEdgeDetector m_hotkeyDetector;
        bool m_recording = false;

        // Overlay state. Symmetric to the log state above. The aggregator
        // is constructed late (in xrCreateInstance, once we know the
        // refresh_hz from settings and have the cached m_qpcFrequency).
        // The renderer that will ship in PR2 reads its Snapshot via
        // m_overlay.snapshot() once per render tick.
        //
        // m_overlayActive mirrors m_recording: true in auto mode, toggled
        // by press in hotkey mode.
        detail::OverlayAggregator m_overlay;
        detail::HotkeyEdgeDetector m_overlayHotkeyDetector;
        bool m_overlayActive = false;

        // GPU timing — owned by unique_ptr so the concrete type (D3D11 or
        // D3D12) is picked at xrCreateSession based on the binding. Null
        // when the app uses Vulkan / OpenGL / no binding, in which case
        // m_pendingFrames stays empty and FrameRecords are pushed to the
        // CsvWriter immediately with gpu_time_ns = 0.
        //
        // Single-threaded access from the frame-loop thread: every call
        // into beginFrame / endFrameAndResolveOldest is from xrBeginFrame /
        // xrEndFrame, which the OpenXR spec serialises within a session.
        // The pending deque holds FrameRecords waiting on a GPU result to
        // land — typically kGpuRingSize entries in steady state, drains
        // when xrDestroySession is called.
        std::unique_ptr<IGpuTimer> m_gpuTimer;
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
