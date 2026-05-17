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

// =============================================================================
// test_telemetry.cpp — integration tests for the frame-timing CSV writer.
//
// The CsvWriter, FrameRecord and buildSessionCsvPath live in an anonymous
// namespace inside layer.cpp, so they're not directly accessible from this
// translation unit. We exercise them via the layer's public OpenXrApi
// interface (the same one the OpenXR loader drives) and assert against
// the side effects we CAN see from a test:
//
//   - the on-disk CSV file in our test temp dir
//   - the recorded calls / state of the mock runtime
//   - the file's contents (header line, row count, footer)
//
// Pattern for every test:
//   1. mock::reset() — clean mock state
//   2. point openxr_api_layer::localAppData at a fresh temp dir
//   3. drive the layer through Create → (Wait/Begin/End)* → Destroy
//   4. assert on the resulting file(s) under <temp>/sessions/
//   5. ResetInstance() to free the OpenXrLayer singleton between tests
// =============================================================================

#include "pch.h"

#include <doctest/doctest.h>

#include "mock_runtime.h"

#include <layer.h>
#include <log.h>
#include <utils/default_settings_template.h>
#include <utils/hotkey.h>
#include <utils/settings.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

    // RAII test fixture: per-test temp dir under the system temp, wired into
    // openxr_api_layer::localAppData so the CsvWriter writes its sessions/
    // folder there. Destructor removes the dir and resets the layer singleton.
    class TelemetryFixture {
      public:
        TelemetryFixture() {
            // Defensive: if a previous test died mid-way without running its
            // destructor cleanly (or doctest invokes us out of order), make
            // sure the layer's g_instance is unset before we wire up a fresh
            // mock. ResetInstance is a no-op when g_instance is already null.
            openxr_api_layer::ResetInstance();

            // Random suffix so parallel test runs don't collide. We never go
            // through the OS's CreateDirectoryW in xrCreateInstance (only
            // CsvWriter does, via std::filesystem::create_directories on the
            // sessions/ subdir), so we have to create the layer-level dir
            // ourselves up front.
            std::random_device rd;
            std::ostringstream name;
            name << "xr_telemetry_test_" << std::hex << rd() << "_" << std::hex << rd();
            m_tempDir = std::filesystem::temp_directory_path() / name.str();
            std::filesystem::create_directories(m_tempDir);

            m_savedLocalAppData = openxr_api_layer::localAppData;
            openxr_api_layer::localAppData = m_tempDir;

            mock::reset();
        }

        ~TelemetryFixture() {
            // Tear down the layer singleton FIRST so its destructor (which
            // joins the CsvWriter thread and flushes the file) runs while
            // the temp dir still exists.
            openxr_api_layer::ResetInstance();

            openxr_api_layer::localAppData = m_savedLocalAppData;

            std::error_code ec;
            std::filesystem::remove_all(m_tempDir, ec);
        }

        std::filesystem::path sessionsDir() const {
            return m_tempDir / "sessions";
        }

        // Exposed so tests can pre-write a settings file at
        // `<tempDir>/<app>_settings.json` BEFORE startLayer, to exercise
        // the bypass / hotkey branches without depending on the layer's
        // bootstrap path.
        const std::filesystem::path& tempDir() const { return m_tempDir; }

        // Common setup: wire the singleton to the mock runtime and call
        // xrCreateInstance. Resolves m_xrWaitFrame / m_xrBeginFrame /
        // m_xrEndFrame / m_xrDestroyInstance so the OpenXrApi virtual
        // base methods can chain downstream into the mock.
        openxr_api_layer::OpenXrApi* startLayer(const char* appName) {
            auto* api = openxr_api_layer::GetInstance();
            api->SetGetInstanceProcAddr(&mock::xrGetInstanceProcAddr,
                                        reinterpret_cast<XrInstance>(0x1));

            XrApplicationInfo appInfo{};
            std::strncpy(appInfo.applicationName, appName, XR_MAX_APPLICATION_NAME_SIZE - 1);
            appInfo.applicationVersion = 1;
            std::strncpy(appInfo.engineName, "test", XR_MAX_ENGINE_NAME_SIZE - 1);
            appInfo.engineVersion = 1;
            appInfo.apiVersion = XR_API_VERSION_1_0;
            XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
            info.applicationInfo = appInfo;

            REQUIRE(api->xrCreateInstance(&info) == XR_SUCCESS);

            // Populate m_xrWaitFrame / m_xrBeginFrame / m_xrEndFrame /
            // m_xrDestroyInstance via the layer's own xrGetInstanceProcAddr.
            // In production this is what the loader does when the app
            // queries each entry point; we have to do it explicitly here
            // because no loader sits in front of us.
            PFN_xrVoidFunction sink = nullptr;
            for (const char* n : {"xrWaitFrame", "xrBeginFrame", "xrEndFrame", "xrDestroyInstance"}) {
                REQUIRE(api->xrGetInstanceProcAddr(
                            reinterpret_cast<XrInstance>(0x1), n, &sink) == XR_SUCCESS);
            }
            return api;
        }

      private:
        std::filesystem::path m_tempDir;
        std::filesystem::path m_savedLocalAppData;
    };

    // Find the single CSV file the layer should have created under
    // sessions/. Returns std::nullopt if no file exists; FAILS the test
    // (via REQUIRE) if more than one — we expect tests to produce exactly
    // one or zero CSVs.
    std::optional<std::filesystem::path> findSessionCsv(const std::filesystem::path& sessionsDir) {
        if (!std::filesystem::exists(sessionsDir)) {
            return std::nullopt;
        }
        std::vector<std::filesystem::path> csvs;
        for (const auto& e : std::filesystem::directory_iterator(sessionsDir)) {
            if (e.is_regular_file() && e.path().extension() == ".csv") {
                csvs.push_back(e.path());
            }
        }
        if (csvs.empty()) return std::nullopt;
        REQUIRE_MESSAGE(csvs.size() == 1, "expected exactly one CSV in sessions/");
        return csvs.front();
    }

    // Read all lines of a file. We assume the writer has flushed and closed
    // by the time we read (test fixtures take care of that via ResetInstance
    // in their destructor, but we sometimes need to read mid-test, after
    // explicit xrDestroyInstance).
    std::vector<std::string> readLines(const std::filesystem::path& path) {
        std::vector<std::string> lines;
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            lines.push_back(line);
        }
        return lines;
    }

    // Split a CSV line on commas. The footer (# session_end ...) is the only
    // free-form line; everything else is well-formed positional CSV.
    std::vector<std::string> split(const std::string& line, char sep = ',') {
        std::vector<std::string> out;
        std::string cell;
        for (char c : line) {
            if (c == sep) {
                out.push_back(std::move(cell));
                cell.clear();
            } else {
                cell.push_back(c);
            }
        }
        out.push_back(std::move(cell));
        return out;
    }

    // Drive one full frame cycle through the layer: wait → begin → end. The
    // mock's xrWaitFrame can sleep (set state.waitFrameSleepMicros) to give
    // the layer a non-zero wait_block_ns to measure.
    //
    // The trailing sleep is non-obvious. The CsvWriter's writer thread is
    // single-consumer; it holds m_mtx for the few µs it takes to drain the
    // deque into a batch. The producer (push() from xrEndFrame) uses
    // try_to_lock, so if the test thread races a push against the writer's
    // drain window, the push is dropped (counted in droppedTryLock). In a
    // real VR app, the 11 ms inter-frame gap makes this race vanishingly
    // unlikely; tests that hammer driveOneFrame back-to-back used to drop
    // ~1 record per 5 frames on the GitHub-hosted Windows runner. 500 µs
    // is well above the writer's drain time on any realistic machine
    // (including a noisy CI VM) and well below the 11 ms a real app
    // would space frames by, so it both deflakes the tests and stays
    // representative of production.
    void driveOneFrame(openxr_api_layer::OpenXrApi* api) {
        const auto session = reinterpret_cast<XrSession>(0x1234);
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        REQUIRE(api->xrWaitFrame(session, &waitInfo, &frameState) == XR_SUCCESS);

        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        REQUIRE(api->xrBeginFrame(session, &beginInfo) == XR_SUCCESS);

        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        endInfo.layers = nullptr;
        REQUIRE(api->xrEndFrame(session, &endInfo) == XR_SUCCESS);

        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    // Column indices in the CSV header. Kept in one place so adding a column
    // is a one-line change here.
    constexpr int kColFrame = 0;
    constexpr int kColTimestampQpc = 1;
    constexpr int kColWaitBlockNs = 2;
    constexpr int kColPreBeginNs = 3;
    constexpr int kColAppCpuNs = 4;
    constexpr int kColEndFrameNs = 5;
    constexpr int kColFrameTotalNs = 6;
    constexpr int kColGpuTimeNs = 7;
    constexpr int kColPeriodNs = 8;
    constexpr int kColHeadroomPct = 9;
    constexpr int kColGpuHeadroomPct = 10;
    constexpr int kColShouldRender = 11;
    constexpr int kColCount = 12;

    const std::string kExpectedHeader =
        "frame,timestamp_qpc,wait_block_ns,pre_begin_ns,app_cpu_ns,end_frame_ns,"
        "frame_total_ns,gpu_time_ns,period_ns,headroom_pct,gpu_headroom_pct,"
        "should_render";

} // namespace

// ----------------------------------------------------------------------------
// 1. The CSV file is created at the expected location with the right header
//    after xrCreateInstance, even before any frames are rendered.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: xrCreateInstance creates a session CSV with the expected header") {
    TelemetryFixture fix;
    fix.startLayer("HelloXR");

    const auto csv = findSessionCsv(fix.sessionsDir());
    REQUIRE(csv.has_value());

    // The file path should encode the app name (sanitised) and a UTC
    // timestamp ending in Z. Spot-check both pieces of structure.
    const auto fname = csv->filename().string();
    CHECK_MESSAGE(fname.find("HelloXR") != std::string::npos,
                  "filename should contain the app name; got: " << fname);
    CHECK_MESSAGE(fname.find("Z_") != std::string::npos,
                  "filename should have the Z (UTC) timestamp suffix; got: " << fname);

    // The header is written and flushed eagerly in CsvWriter::start, so we
    // can read it back even though no frames have been pushed yet.
    const auto lines = readLines(*csv);
    REQUIRE(lines.size() >= 1);
    CHECK(lines[0] == kExpectedHeader);
}

// ----------------------------------------------------------------------------
// 2. App name sanitisation: special chars are replaced with '_' in the file
//    name. Tests the buildSessionCsvPath logic via the file it produces.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: app names with special characters are sanitised in the CSV filename") {
    TelemetryFixture fix;
    // The OpenXR spec caps applicationName at 128 chars; pick a name with
    // every character class the sanitiser handles plus one valid alnum
    // sentinel on each side so we can spot the boundary.
    fix.startLayer("A<B>C:D\"E/F\\G|H?I*J K-_."); // mixed: alnum, dashes, dots, junk

    const auto csv = findSessionCsv(fix.sessionsDir());
    REQUIRE(csv.has_value());
    const auto fname = csv->filename().string();

    // The sanitiser keeps [A-Za-z0-9._-] and replaces everything else with '_'.
    // Concretely: every '<', '>', ':', '"', '/', '\\', '|', '?', '*', ' ' should
    // be '_' in the filename, while letters/digits, '-', '_' and '.' survive.
    CHECK(fname.find('<') == std::string::npos);
    CHECK(fname.find('>') == std::string::npos);
    CHECK(fname.find(':') == std::string::npos);
    CHECK(fname.find('"') == std::string::npos);
    CHECK(fname.find('/') == std::string::npos);
    CHECK(fname.find('\\') == std::string::npos);
    CHECK(fname.find('|') == std::string::npos);
    CHECK(fname.find('?') == std::string::npos);
    CHECK(fname.find('*') == std::string::npos);
    CHECK(fname.find(' ') == std::string::npos);
    // ...and the alphanumeric letters all show up.
    for (char c : {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J'}) {
        CHECK_MESSAGE(fname.find(c) != std::string::npos,
                      "expected letter '" << c << "' to survive in " << fname);
    }
}

// ----------------------------------------------------------------------------
// 3. An empty app name falls back to "unknown" in the filename, rather than
//    producing "<timestamp>Z_.csv" or failing.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: empty app name falls back to 'unknown'") {
    TelemetryFixture fix;
    fix.startLayer("");
    const auto csv = findSessionCsv(fix.sessionsDir());
    REQUIRE(csv.has_value());
    CHECK(csv->filename().string().find("unknown") != std::string::npos);
}

// ----------------------------------------------------------------------------
// 4. A "probe" XrInstance — Create + Destroy without ever rendering a frame —
//    leaves no junk in sessions/. This is the OpenComposite / OXR Toolkit
//    capability-probe pattern; the empty CSV is deleted by CsvWriter::stop
//    when m_written == 0.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: probe XrInstance (no frames) deletes the empty CSV on destroy") {
    TelemetryFixture fix;
    auto* api = fix.startLayer("ProbeApp");

    // File exists right after Create (header was written).
    REQUIRE(findSessionCsv(fix.sessionsDir()).has_value());

    // Destroy without driving any frame. The framework's auto-generated
    // xrDestroyInstance calls ResetInstance() which destroys our OpenXrLayer,
    // triggering CsvWriter::stop. Because m_written == 0 the file is
    // removed (probe cleanup branch).
    REQUIRE(api->xrDestroyInstance(reinterpret_cast<XrInstance>(0x1)) == XR_SUCCESS);

    CHECK_FALSE(findSessionCsv(fix.sessionsDir()).has_value());
}

// ----------------------------------------------------------------------------
// 5. After rendering one full frame and tearing down the layer, the CSV ends
//    up with exactly one data row plus the # session_end footer.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: one frame produces one CSV data row + footer") {
    TelemetryFixture fix;
    auto* api = fix.startLayer("SingleFrameApp");

    driveOneFrame(api);

    // Force a clean tear-down so the CsvWriter joins its thread and writes
    // the footer before we read the file. ResetInstance is also invoked by
    // the fixture's destructor, but we want to read mid-test here.
    openxr_api_layer::ResetInstance();

    const auto csv = findSessionCsv(fix.sessionsDir());
    REQUIRE(csv.has_value());
    const auto lines = readLines(*csv);

    // Expect: header (1), data row (1), footer "# session_end ..." (1)
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == kExpectedHeader);
    CHECK(lines[2].rfind("# session_end", 0) == 0);
    CHECK(lines[2].find("written=1") != std::string::npos);

    // Data row column count + a couple of value sanity checks.
    const auto cells = split(lines[1]);
    REQUIRE(cells.size() == kColCount);
    CHECK(cells[kColFrame] == "0");                          // first frame
    CHECK(std::stoll(cells[kColTimestampQpc]) > 0);
    CHECK(std::stoll(cells[kColPeriodNs]) == 11'111'111);    // mock default
    CHECK(cells[kColShouldRender] == "1");                   // shouldRender = XR_TRUE
    // First frame has no previous tEnd → frame_total_ns is reported as 0.
    CHECK(std::stoll(cells[kColFrameTotalNs]) == 0);
}

// ----------------------------------------------------------------------------
// 6. frame_total_ns is 0 on the first frame and strictly positive on every
//    subsequent one. Same invariant goes for monotonicity of timestamp_qpc.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: frame_total_ns is 0 on frame 0 and positive thereafter") {
    TelemetryFixture fix;
    auto* api = fix.startLayer("MultiFrameApp");

    constexpr int kFrames = 5;
    for (int i = 0; i < kFrames; ++i) {
        driveOneFrame(api);
    }

    openxr_api_layer::ResetInstance();

    const auto csv = findSessionCsv(fix.sessionsDir());
    REQUIRE(csv.has_value());
    const auto lines = readLines(*csv);
    REQUIRE(lines.size() == kFrames + 2);  // header + N rows + footer

    int64_t prevQpc = -1;
    for (int i = 0; i < kFrames; ++i) {
        const auto cells = split(lines[1 + i]);
        REQUIRE(cells.size() == kColCount);
        CHECK(cells[kColFrame] == std::to_string(i));

        const auto qpc = std::stoll(cells[kColTimestampQpc]);
        CHECK(qpc > prevQpc);   // monotonic
        prevQpc = qpc;

        const auto frameTotal = std::stoll(cells[kColFrameTotalNs]);
        if (i == 0) {
            CHECK_MESSAGE(frameTotal == 0,
                          "first frame has no previous cycle to compare; expected 0, got "
                          << frameTotal);
        } else {
            CHECK_MESSAGE(frameTotal > 0,
                          "frame " << i << " should have a positive frame_total_ns; got "
                          << frameTotal);
        }
    }
}

// ----------------------------------------------------------------------------
// 7. The full per-row decomposition adds up: wait_block + pre_begin + app_cpu
//    are non-negative and the layer counts forward correctly.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: per-frame timing columns are non-negative and self-consistent") {
    TelemetryFixture fix;
    // Force the mock's xrWaitFrame to sleep ~1 ms so wait_block_ns is large
    // enough to dominate measurement noise and we can assert > 0.
    mock::state().waitFrameSleepMicros = 1000;

    auto* api = fix.startLayer("WaitBlockApp");
    constexpr int kFrames = 3;
    for (int i = 0; i < kFrames; ++i) {
        driveOneFrame(api);
    }
    openxr_api_layer::ResetInstance();

    const auto csv = findSessionCsv(fix.sessionsDir());
    REQUIRE(csv.has_value());
    const auto lines = readLines(*csv);
    REQUIRE(lines.size() >= kFrames + 1);

    for (int i = 0; i < kFrames; ++i) {
        const auto cells = split(lines[1 + i]);
        REQUIRE(cells.size() == kColCount);

        const auto waitBlock = std::stoll(cells[kColWaitBlockNs]);
        const auto preBegin = std::stoll(cells[kColPreBeginNs]);
        const auto appCpu = std::stoll(cells[kColAppCpuNs]);
        const auto endFrame = std::stoll(cells[kColEndFrameNs]);

        // All durations are non-negative.
        CHECK(waitBlock >= 0);
        CHECK(preBegin >= 0);
        CHECK(appCpu >= 0);
        CHECK(endFrame >= 0);

        // We slept ~1 ms inside xrWaitFrame; allow plenty of slack for slow
        // CI / scheduler jitter — we mostly want to confirm the layer
        // actually measured a non-trivial block, not that the clock is
        // perfectly accurate.
        CHECK_MESSAGE(waitBlock >= 500'000,
                      "expected wait_block_ns >= 500 µs after mock sleep, got " << waitBlock);
    }
}

// ----------------------------------------------------------------------------
// 8. Calling xrCreateInstance twice on the same singleton (without an
//    intervening xrDestroyInstance) does NOT crash. Reproduces the
//    OpenComposite / OXR-Toolkit probe-then-real init flow that caused
//    std::terminate() in PR #1 before CsvWriter::start was made idempotent.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: double xrCreateInstance on the same singleton survives") {
    TelemetryFixture fix;
    auto* api = fix.startLayer("FirstInit");

    // Without destroying, drive a second xrCreateInstance through the layer.
    // The framework's GetInstance returns the existing singleton (no
    // ResetInstance has fired), so this hits the same OpenXrLayer twice —
    // exactly the OC pattern. CsvWriter::start's idempotent guard makes the
    // second call a no-op rather than std::terminate via thread reassignment.
    XrApplicationInfo appInfo{};
    std::strcpy(appInfo.applicationName, "SecondInit");
    appInfo.applicationVersion = 1;
    std::strcpy(appInfo.engineName, "test");
    appInfo.apiVersion = XR_API_VERSION_1_0;
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    info.applicationInfo = appInfo;
    CHECK(api->xrCreateInstance(&info) == XR_SUCCESS);

    // Still produces exactly one CSV (the first init's). The second start
    // is a no-op, no new file appears.
    driveOneFrame(api);
    openxr_api_layer::ResetInstance();
    const auto csv = findSessionCsv(fix.sessionsDir());
    REQUIRE(csv.has_value());
    CHECK(csv->filename().string().find("FirstInit") != std::string::npos);
}

// ----------------------------------------------------------------------------
// 9. should_render = XR_FALSE round-trips into the CSV as 0.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: should_render XR_FALSE is recorded as 0 in the CSV") {
    TelemetryFixture fix;
    mock::state().shouldRender = XR_FALSE;

    auto* api = fix.startLayer("SkippedFrameApp");
    driveOneFrame(api);
    openxr_api_layer::ResetInstance();

    const auto csv = findSessionCsv(fix.sessionsDir());
    REQUIRE(csv.has_value());
    const auto lines = readLines(*csv);
    REQUIRE(lines.size() >= 2);
    const auto cells = split(lines[1]);
    REQUIRE(cells.size() == kColCount);
    CHECK(cells[kColShouldRender] == "0");
}

// ----------------------------------------------------------------------------
// 10. The mock runtime observes the expected number of forwarded calls — no
//    frame is silently dropped by the layer, no extra is forwarded.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: layer is pure pass-through w.r.t. downstream call counts") {
    TelemetryFixture fix;
    auto* api = fix.startLayer("PassThroughApp");

    constexpr int kFrames = 7;
    for (int i = 0; i < kFrames; ++i) {
        driveOneFrame(api);
    }
    openxr_api_layer::ResetInstance();

    CHECK(mock::state().waitFrameCallCount == kFrames);
    CHECK(mock::state().beginFrameCallCount == kFrames);
    CHECK(mock::state().endFrameCallCount == kFrames);
}

// ----------------------------------------------------------------------------
// 11. GPU columns degrade gracefully when no D3D11 device is available.
//    The mock runtime mocks xrCreateSession but the integration tests don't
//    drive it (no graphics binding), so GpuTimer::init is never called and
//    isActive() stays false. gpu_time_ns reads as 0 (no measurement);
//    gpu_headroom_pct follows the formula with gpu_time_ns=0, yielding
//    100% — matching fpsVR / OpenXR Toolkit's "GPU unmeasured = 100%
//    headroom default" convention.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: gpu_time_ns is 0 and gpu_headroom_pct is 100% when no D3D11 binding is set up") {
    TelemetryFixture fix;
    auto* api = fix.startLayer("NoGpuTimingApp");

    constexpr int kFrames = 3;
    for (int i = 0; i < kFrames; ++i) {
        driveOneFrame(api);
    }
    openxr_api_layer::ResetInstance();

    const auto csv = findSessionCsv(fix.sessionsDir());
    REQUIRE(csv.has_value());
    const auto lines = readLines(*csv);
    REQUIRE(lines.size() == kFrames + 2);  // header + N + footer

    for (int i = 0; i < kFrames; ++i) {
        const auto cells = split(lines[1 + i]);
        REQUIRE(cells.size() == kColCount);
        CHECK(cells[kColGpuTimeNs] == "0");
        CHECK(cells[kColGpuHeadroomPct] == "100.00");
    }
}

// ----------------------------------------------------------------------------
// 9. Bypass branch: a per-app settings file with `log.enabled = false`
//    must turn the layer into a pure pass-through — no CSV, no frame
//    records, even after multiple xrEndFrame calls. Covers the runtime
//    wiring that the unit tests in test_settings.cpp cannot reach (they
//    test the parser; this test checks the parser's output actually
//    drives m_bypassApiLayer).
//
// Note: hotkey mode (log AND overlay) is unit-tested end-to-end in
// test_hotkey.cpp via the HotkeyEdgeDetector + parseHotkey helpers.
// Driving it through the layer here would require mocking GetAsync
// KeyState, which is a Win32 syscall — too invasive for the testing
// surface area. The integration coverage here intentionally stops at
// the parser → m_settings → m_bypassApiLayer / m_recording /
// m_overlayActive boundary.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: log.enabled=false in per-app settings disables CSV writing") {
    TelemetryFixture fix;

    // Pre-write the per-app file BEFORE startLayer so bootstrapAndLoad
    // Settings finds it and skips the template copy. The slug rule
    // matches sanitizeForFilename: "BypassTest" → "bypasstest".
    const auto perAppPath = fix.tempDir() / "bypasstest_settings.json";
    {
        std::ofstream out(perAppPath);
        REQUIRE(out.is_open());
        out << R"({"log":{"enabled":false}})";
    }

    auto* api = fix.startLayer("BypassTest");

    // Drive a handful of frames — every override should early-return
    // through the base class without touching CsvWriter or the GPU
    // timer.
    constexpr int kFrames = 5;
    for (int i = 0; i < kFrames; ++i) {
        driveOneFrame(api);
    }

    // Force CsvWriter teardown (matches what xrDestroyInstance triggers
    // in production) so any header-only CSV would be on disk by now.
    openxr_api_layer::ResetInstance();

    // The bypass branch never opens a writer, so the sessions/ folder
    // either doesn't exist or is empty. findSessionCsv returns nullopt
    // in both cases.
    const auto csv = findSessionCsv(fix.sessionsDir());
    CHECK_MESSAGE(!csv.has_value(),
                  "expected no CSV under sessions/ when log.enabled=false; "
                  "found " << (csv ? csv->string() : std::string("<none>")));
}

// ----------------------------------------------------------------------------
// 10. Drift check: the in-binary `kBuiltInDefaultSettings` constexpr (the
//     fallback the layer ships when the installer never ran) MUST parse
//     to the same TelemetrySettings as installer/default_settings.json
//     (the file the installer drops). Without this guarantee a ZIP user
//     and an installer user get different first-run behaviour — exactly
//     the silent split the README's "Robustness contract" section
//     promises NOT to ship. Comments and whitespace are ignored (we
//     compare the PARSED structs, not the byte stream).
//
//     Resolves the installer file at runtime by navigating up from the
//     test binary path: <repo>\bin\<Platform>\<Config>\test.exe → 4
//     parent_path() calls reach the repo root, then the well-known
//     installer/ subdir.
// ----------------------------------------------------------------------------
TEST_CASE("template constexpr matches installer/default_settings.json (no schema drift)") {
    using openxr_api_layer::detail::parseSettings;
    using openxr_api_layer::detail::formatHotkey;
    using openxr_api_layer::detail::kBuiltInDefaultSettings;

    char exePath[MAX_PATH] = {};
    REQUIRE(::GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0);
    const auto exeFile = std::filesystem::path(exePath);
    // .exe → bin/<Platform>/<Config>/ → bin/<Platform>/ → bin/ → repo root
    const auto repoRoot = exeFile.parent_path()
                              .parent_path()
                              .parent_path()
                              .parent_path();
    const auto installerFile = repoRoot / "installer" / "default_settings.json";
    REQUIRE_MESSAGE(std::filesystem::exists(installerFile),
                    "Could not locate installer/default_settings.json relative to the "
                    "test binary. Test binary at: " << exeFile.string()
                    << ". Looked at: " << installerFile.string()
                    << ". If the build layout changed, update the parent_path() chain "
                    "above to match.");

    std::ifstream in(installerFile);
    REQUIRE(in.is_open());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string installerJson = ss.str();

    const auto installer = parseSettings(installerJson);
    const auto runtime = parseSettings(kBuiltInDefaultSettings);
    REQUIRE_MESSAGE(installer.error.empty(),
                    "installer/default_settings.json failed to parse: " << installer.error);
    REQUIRE_MESSAGE(runtime.error.empty(),
                    "in-binary kBuiltInDefaultSettings failed to parse: " << runtime.error);

    const auto& a = installer.settings;
    const auto& b = runtime.settings;
    CHECK(a.log.enabled == b.log.enabled);
    CHECK(a.log.mode    == b.log.mode);
    CHECK(formatHotkey(a.log.hotkey) == formatHotkey(b.log.hotkey));
    CHECK(a.overlay.enabled    == b.overlay.enabled);
    CHECK(a.overlay.mode       == b.overlay.mode);
    CHECK(formatHotkey(a.overlay.hotkey) == formatHotkey(b.overlay.hotkey));
    CHECK(a.overlay.refresh_hz == b.overlay.refresh_hz);
    CHECK(a.overlay.position   == b.overlay.position);
}
