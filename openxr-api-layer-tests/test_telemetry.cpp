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
#include <utils/name_utils.h>
#include <utils/overlay_layout.h>
#include <utils/overlay_renderer.h>
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

        // Write a per-app settings file at <tempDir>/<slug>_settings.json,
        // deriving the slug from appName via the same sanitizeForFilename the
        // layer's resolvePerAppConfigPath uses — so a test's pre-written file
        // and the name it later passes to startLayer can never drift apart.
        void writePerApp(const char* appName, const char* jsonBody) {
            const auto perAppPath = m_tempDir /
                (openxr_api_layer::sanitizeForFilename(appName) + "_settings.json");
            std::ofstream out(perAppPath);
            REQUIRE(out.is_open());
            out << jsonBody;
        }

        // Convenience wrapper over writePerApp: turns log writing on in AUTO
        // mode. The shipped template enables log in HOTKEY mode (armed but
        // dormant — no CSV until a keypress), so a test that needs a
        // deterministic CSV without simulating a hotkey pre-writes its own
        // per-app file — `{"log":{"enabled":true}}` enables it and the omitted
        // `mode` falls back to auto, so the CSV opens at session start.
        void enableLog(const char* appName) {
            writePerApp(appName, R"({"log":{"enabled":true}})");
        }

        // Like enableLog, but flips overlay.enabled (mode falls back to auto).
        // A per-app config with BOTH features disabled makes the layer a pure
        // pass-through (m_bypassApiLayer — see xrCreateInstance) and xrEndFrame
        // returns before the overlay block; enabling the overlay keeps
        // xrEndFrame live. The overlay-integration tests need that live
        // xrEndFrame; the renderer itself is supplied via
        // ForceOverlayActiveForTest, not built from a graphics binding.
        void enableOverlay(const char* appName) {
            writePerApp(appName, R"({"overlay":{"enabled":true}})");
        }

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
    // GPU telemetry columns appended in feat/gpu-telemetry. NaN /
    // 0 sentinels in CI (no NvAPI driver, no real DXGI adapter on
    // the GH Actions runner — the test fixture's mock_runtime fakes
    // the OpenXR surface but never wires a real GPU). The columns
    // are still emitted per-frame so the column count stays stable
    // across all FrameRecords.
    constexpr int kColGpuTempC = 12;
    constexpr int kColVramUsedBytes = 13;
    constexpr int kColVramBudgetBytes = 14;
    // CPU usage column appended in feat/overlay-cpus-max-core — the "CPUs
    // LOAD" reading. NaN in CI (the mock_runtime fixture never wires a real
    // CPU sampler / the first frames have no baseline delta), but still
    // emitted every frame so the column count stays stable.
    constexpr int kColCpusMaxPct = 15;
    constexpr int kColRenderNs = 16;
    constexpr int kColCount = 17;

    const std::string kExpectedHeader =
        "frame,timestamp_qpc,wait_block_ns,pre_begin_ns,app_cpu_ns,end_frame_ns,"
        "frame_total_ns,gpu_time_ns,period_ns,headroom_pct,gpu_headroom_pct,"
        "should_render,gpu_temp_c,vram_used_bytes,vram_budget_bytes,cpus_max_pct,"
        "render_ns";

    // Stand-in OverlayRenderer for the overlay-integration tests. Records the
    // calls the layer makes (renderAndCompose / pushFrameSample) and hands
    // back a quad layer so the layer's injection path runs — no GPU, no
    // swapchain. Injected via openxr_api_layer::ForceOverlayActiveForTest.
    class MockOverlayRenderer : public openxr_api_layer::detail::OverlayRenderer {
      public:
        MockOverlayRenderer() { m_quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD; }
        ~MockOverlayRenderer() override {
            if (destroyedFlag) *destroyedFlag = true;
        }

        bool isReady() const noexcept override { return true; }

        void pushFrameSample(int64_t cpu_per_cycle_ns, int64_t gpu_time_ns) override {
            ++pushFrameSampleCalls;
            lastCpuPerCycleNs = cpu_per_cycle_ns;
            lastGpuTimeNs = gpu_time_ns;
        }

        const XrCompositionLayerBaseHeader* renderAndCompose(
                XrSpace space,
                const XrPosef* anchorPose,
                const openxr_api_layer::detail::OverlaySnapshot& /*snap*/,
                const openxr_api_layer::detail::OverlayGeometry& geo) override {
            ++renderAndComposeCalls;
            lastSpace = space;
            lastHadAnchorPose = (anchorPose != nullptr);
            if (anchorPose) lastAnchorPose = *anchorPose;
            lastGeo = geo;
            return returnLayer
                ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_quad)
                : nullptr;
        }

        // Knobs + recorded calls. Read while the layer singleton (which owns
        // this object) is still alive — i.e. before the fixture's
        // ResetInstance(). For teardown tests, point destroyedFlag at a bool
        // that outlives the layer.
        bool    returnLayer = true;
        int     pushFrameSampleCalls = 0;
        int     renderAndComposeCalls = 0;
        int64_t lastCpuPerCycleNs = 0;
        int64_t lastGpuTimeNs = 0;
        XrSpace lastSpace = XR_NULL_HANDLE;
        bool    lastHadAnchorPose = false;
        XrPosef lastAnchorPose{};
        openxr_api_layer::detail::OverlayGeometry lastGeo{};
        bool*   destroyedFlag = nullptr;

      private:
        XrCompositionLayerQuad m_quad{};
    };

} // namespace

// ----------------------------------------------------------------------------
// 1. The CSV file is created at the expected location with the right header
//    after xrCreateInstance, even before any frames are rendered.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: xrCreateInstance creates a session CSV with the expected header") {
    TelemetryFixture fix;
    fix.enableLog("HelloXR");
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
    fix.enableLog("A<B>C:D\"E/F\\G|H?I*J K-_.");
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
    fix.enableLog("");
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
    fix.enableLog("ProbeApp");
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
    fix.enableLog("SingleFrameApp");
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
    // render_ns (Begin-exit → End) is non-negative (0 if begin was skipped).
    CHECK(std::stoll(cells[kColRenderNs]) >= 0);
}

// ----------------------------------------------------------------------------
// 6. frame_total_ns is 0 on the first frame and strictly positive on every
//    subsequent one. Same invariant goes for monotonicity of timestamp_qpc.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: frame_total_ns is 0 on frame 0 and positive thereafter") {
    TelemetryFixture fix;
    fix.enableLog("MultiFrameApp");
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

    fix.enableLog("WaitBlockApp");
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
    fix.enableLog("FirstInit");
    auto* api = fix.startLayer("FirstInit");

    // Without destroying, drive a second xrCreateInstance through the layer.
    // The framework's GetInstance returns the existing singleton (no
    // ResetInstance has fired), so this hits the same OpenXrLayer twice —
    // exactly the OC pattern. The layer's m_layerInitialized guard early-
    // returns on the second call without re-running the settings bootstrap
    // (which would otherwise re-key m_appName / m_settings onto "SecondInit"
    // and rebuild the overlay aggregator, discarding the session's
    // accumulated history). Defence in
    // depth: CsvWriter::start is idempotent too — even if the layer guard
    // ever regressed, the second start() is a no-op rather than
    // std::terminate via thread reassignment.
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

    fix.enableLog("SkippedFrameApp");
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

    // Force a real bypass (m_bypassApiLayer) by disabling BOTH features, so
    // this exercises the pass-through path rather than the armed-by-default
    // template. The call-count invariant below holds in BOTH bypass and
    // live-but-suspended hotkey mode, so it documents intent more than it
    // proves bypass — the no-CSV bypass guarantee itself is covered by the
    // dedicated log.enabled=false test.
    fix.writePerApp("PassThroughApp",
                    R"({"log":{"enabled":false},"overlay":{"enabled":false}})");

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
    fix.enableLog("NoGpuTimingApp");
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
    // Settings finds it and skips the template copy.
    fix.writePerApp("BypassTest", R"({"log":{"enabled":false}})");

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
// 9b. Suspend branch: overlay in HOTKEY mode but never activated (with log
//     off) is NOT a full bypass — overlay.enabled=true keeps the layer live so
//     it can still poll the activation hotkey — but with neither the CSV
//     (m_recording) nor the HUD (m_overlayActive) consuming frames, the
//     per-frame telemetry is suspended (collecting == false). The invariant
//     that MUST survive that suspension: xrEndFrame is still forwarded every
//     frame, or the app would stop submitting to the compositor. We can't
//     measure the CPU-cost drop directly, but we DO check a proxy for it: a
//     suspended frame early-returns before the m_frameIndex fetch_add, so the
//     counter must not advance (FrameIndexForTest). ForceOverlayActiveForTest
//     is deliberately NOT called, so m_overlayActive stays false.
// ----------------------------------------------------------------------------
TEST_CASE("telemetry: un-activated overlay hotkey suspends collection but still forwards frames") {
    TelemetryFixture fix;

    fix.writePerApp("SuspendTest", R"({"overlay":{"enabled":true,"mode":"hotkey"}})");

    auto* api = fix.startLayer("SuspendTest");

    constexpr int kFrames = 5;
    for (int i = 0; i < kFrames; ++i) {
        driveOneFrame(api);
    }

    // Proxy for "collection was suspended": a suspended frame early-returns
    // before the m_frameIndex fetch_add, so the counter must not have advanced
    // across kFrames. Deleting the collecting gate would route these frames
    // through the collecting path and bump the counter — failing this check.
    // (Read before ResetInstance tears the singleton down.)
    CHECK(openxr_api_layer::FrameIndexForTest() == 0);

    // The frame submission must still reach the runtime on every frame even
    // while our telemetry is suspended.
    CHECK(mock::state().waitFrameCallCount == kFrames);
    CHECK(mock::state().beginFrameCallCount == kFrames);
    CHECK(mock::state().endFrameCallCount == kFrames);

    openxr_api_layer::ResetInstance();

    // Log is off, so nothing should land on disk regardless of the overlay.
    const auto csv = findSessionCsv(fix.sessionsDir());
    CHECK(!csv.has_value());
}

// ----------------------------------------------------------------------------
// 10. Drift check: the in-binary `kBuiltInDefaultSettings` constexpr (the
//     fallback the layer ships when the installer never ran) MUST parse
//     to the same TelemetrySettings as installer/default_settings.json
//     (the file the installer drops). Without this guarantee a ZIP user
//     and an installer user get different first-run behaviour — exactly
//     the silent split this drift test exists to prevent. Comments and
//     whitespace are ignored (we
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
    CHECK(a.overlay.scale      == b.overlay.scale);
    CHECK(a.overlay.anchor     == b.overlay.anchor);
    CHECK(a.overlay.offset_x   == b.overlay.offset_x);
    CHECK(a.overlay.offset_y   == b.overlay.offset_y);
}

// ----------------------------------------------------------------------------
// Overlay integration. The CSV tests above stop at the m_overlayActive
// boundary because a real overlay needs a GPU device + an OpenXR swapchain the
// headless mock runtime can't supply. These drive the overlay path the layer
// runs inside xrEndFrame by injecting a MockOverlayRenderer via
// ForceOverlayActiveForTest, covering the fanout / layer-injection / teardown
// logic that real bugs once slipped through.
// ----------------------------------------------------------------------------

// Regression: the no-GPU-timer xrEndFrame branch used to dispatch records with
// a bare m_csv.push + m_overlay.pushFrame, skipping fanoutRecord — so
// pushFrameSample was never called and the HUD's CPU/GPU bar strips stayed
// empty for the whole session on a binding without a GPU timer.
TEST_CASE("overlay: no-GPU-timer frames still feed the renderer histogram") {
    TelemetryFixture fx;
    fx.enableOverlay("OverlayBarsTest");   // else the layer bypasses xrEndFrame
    auto* api = fx.startLayer("OverlayBarsTest");

    auto mock = std::make_unique<MockOverlayRenderer>();
    auto* mockPtr = mock.get();
    openxr_api_layer::ForceOverlayActiveForTest(
        std::move(mock), reinterpret_cast<XrSpace>(0xBEEF));

    // No xrCreateSession with a graphics binding ⇒ m_gpuTimer is null ⇒ this
    // exercises the no-timer fanout branch.
    driveOneFrame(api);
    driveOneFrame(api);

    CHECK(mockPtr->renderAndComposeCalls >= 1);
    CHECK(mockPtr->pushFrameSampleCalls >= 1);   // the regression — was 0
}

// An active overlay appends exactly one head-locked quad to xrEndFrame's layer
// list, on top of whatever the app submitted (here, zero layers).
TEST_CASE("overlay: active renderer injects its quad into xrEndFrame") {
    TelemetryFixture fx;
    fx.enableOverlay("OverlayInjectTest");
    auto* api = fx.startLayer("OverlayInjectTest");

    openxr_api_layer::ForceOverlayActiveForTest(
        std::make_unique<MockOverlayRenderer>(), reinterpret_cast<XrSpace>(0xBEEF));

    driveOneFrame(api);   // driveOneFrame submits layerCount = 0

    CHECK(mock::state().lastEndFrameLayerCount == 1);
    CHECK(mock::state().lastEndFrameQuadCount == 1);
}

// renderAndCompose returning nullptr (nothing to draw this frame) must NOT
// inject a layer — the app's submission passes through untouched.
TEST_CASE("overlay: renderAndCompose returning null injects nothing") {
    TelemetryFixture fx;
    fx.enableOverlay("OverlayNullTest");
    auto* api = fx.startLayer("OverlayNullTest");

    auto mock = std::make_unique<MockOverlayRenderer>();
    auto* mockPtr = mock.get();
    mock->returnLayer = false;
    openxr_api_layer::ForceOverlayActiveForTest(
        std::move(mock), reinterpret_cast<XrSpace>(0xBEEF));

    driveOneFrame(api);

    // renderAndCompose ran (not a bypass) but returned null → no injection.
    CHECK(mockPtr->renderAndComposeCalls >= 1);
    CHECK(mock::state().lastEndFrameLayerCount == 0);
    CHECK(mock::state().lastEndFrameQuadCount == 0);
}

// World-locked anchor: the layer locates the VIEW space in the LOCAL space
// once at activation, freezes the quad pose, and hands the renderer the LOCAL
// space + that pose. The frozen pose is the head pose ∘ the corner offset.
TEST_CASE("overlay: world anchor freezes the quad pose in the local space") {
    TelemetryFixture fx;
    fx.enableOverlay("OverlayWorldTest");
    auto* api = fx.startLayer("OverlayWorldTest");

    const auto viewSpace  = reinterpret_cast<XrSpace>(0xBEEF);
    const auto localSpace = reinterpret_cast<XrSpace>(0xCAFE);
    auto mock = std::make_unique<MockOverlayRenderer>();
    auto* mockPtr = mock.get();
    // Non-null localSpace flips the seam into world-anchor mode.
    openxr_api_layer::ForceOverlayActiveForTest(
        std::move(mock), viewSpace, localSpace);

    driveOneFrame(api);

    // It located VIEW relative to LOCAL (not the other way round).
    CHECK(mock::state().locateSpaceCallCount == 1);
    CHECK(mock::state().lastLocateSpace == viewSpace);
    CHECK(mock::state().lastLocateBaseSpace == localSpace);

    // The renderer was handed the LOCAL space + a frozen anchor pose.
    CHECK(mockPtr->renderAndComposeCalls >= 1);
    CHECK(mockPtr->lastSpace == localSpace);
    REQUIRE(mockPtr->lastHadAnchorPose);

    // The frozen pose equals composeAnchorPose(head, cornerOffset) for the
    // mock's default head pose (identity orientation at 1.6 m eye height).
    const auto geo = openxr_api_layer::detail::geometryForPosition("head_top_right", 1.0f);
    const auto head = openxr_api_layer::detail::toOverlayPose(mock::state().locateSpacePose);
    const auto expected = openxr_api_layer::detail::composeAnchorPose(
        head, {geo.pos_x, geo.pos_y, geo.pos_z});
    const auto& got = mockPtr->lastAnchorPose;
    CHECK(got.position.x == doctest::Approx(expected.position.x).epsilon(0.0001));
    CHECK(got.position.y == doctest::Approx(expected.position.y).epsilon(0.0001));
    CHECK(got.position.z == doctest::Approx(expected.position.z).epsilon(0.0001));
    CHECK(got.orientation.w == doctest::Approx(expected.orientation.w).epsilon(0.0001));

    // The quad was injected, attached to the world frame.
    CHECK(mock::state().lastEndFrameQuadCount == 1);
}

// World anchor freezes ONCE: subsequent frames reuse the frozen pose without
// re-locating, so the panel stays put in the play space as the head moves.
TEST_CASE("overlay: world anchor locates only once, then holds") {
    TelemetryFixture fx;
    fx.enableOverlay("OverlayWorldHoldTest");
    auto* api = fx.startLayer("OverlayWorldHoldTest");

    openxr_api_layer::ForceOverlayActiveForTest(
        std::make_unique<MockOverlayRenderer>(),
        reinterpret_cast<XrSpace>(0xBEEF), reinterpret_cast<XrSpace>(0xCAFE));

    driveOneFrame(api);
    driveOneFrame(api);
    driveOneFrame(api);

    // Anchored on frame 1; frames 2-3 reused the cached pose.
    CHECK(mock::state().locateSpaceCallCount == 1);
}

// Untracked head pose at activation: the layer must NOT freeze to a garbage
// pose. It holds off drawing and retries until tracking is valid.
TEST_CASE("overlay: world anchor waits for a tracked pose before freezing") {
    TelemetryFixture fx;
    fx.enableOverlay("OverlayWorldUntrackedTest");
    auto* api = fx.startLayer("OverlayWorldUntrackedTest");

    // Simulate lost tracking: locate succeeds but no location bits are set.
    mock::state().locateSpaceFlags = 0;

    auto mock = std::make_unique<MockOverlayRenderer>();
    auto* mockPtr = mock.get();
    openxr_api_layer::ForceOverlayActiveForTest(
        std::move(mock), reinterpret_cast<XrSpace>(0xBEEF),
        reinterpret_cast<XrSpace>(0xCAFE));

    driveOneFrame(api);

    // Pose wasn't valid → no freeze, no draw, no injection this frame.
    CHECK(mockPtr->renderAndComposeCalls == 0);
    CHECK(mock::state().lastEndFrameQuadCount == 0);

    // Tracking comes back → the next frame anchors and draws.
    mock::state().locateSpaceFlags =
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
        XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
        XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    driveOneFrame(api);

    CHECK(mockPtr->renderAndComposeCalls >= 1);
    CHECK(mockPtr->lastHadAnchorPose);
    CHECK(mock::state().lastEndFrameQuadCount == 1);
}

// A tracking BLIP keeps the *_VALID bits set (last-known / extrapolated pose)
// but clears the *_TRACKED bits. The layer must require TRACKED and refuse to
// freeze to that stale pose — the exact scenario the VALID-only gate missed.
TEST_CASE("overlay: world anchor refuses a valid-but-not-tracked pose") {
    TelemetryFixture fx;
    fx.enableOverlay("OverlayWorldBlipTest");
    auto* api = fx.startLayer("OverlayWorldBlipTest");

    // VALID set, TRACKED clear — a runtime's "here's the last pose I knew".
    mock::state().locateSpaceFlags =
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

    auto mock = std::make_unique<MockOverlayRenderer>();
    auto* mockPtr = mock.get();
    openxr_api_layer::ForceOverlayActiveForTest(
        std::move(mock), reinterpret_cast<XrSpace>(0xBEEF),
        reinterpret_cast<XrSpace>(0xCAFE));

    driveOneFrame(api);

    // It tried to locate but did NOT freeze or draw — the panel waits.
    CHECK(mock::state().locateSpaceCallCount == 1);
    CHECK(mockPtr->renderAndComposeCalls == 0);
    CHECK(mock::state().lastEndFrameQuadCount == 0);
}

// If a tracked pose never arrives (e.g. a 3DoF/seated runtime), the world
// anchor must not black-hole the HUD: after a timeout it logs once and
// degrades to head-locked so the overlay still appears. Driven fast by making
// each mock frame advance the predicted display time past the 2 s budget.
TEST_CASE("overlay: world anchor times out and degrades to head-locked") {
    TelemetryFixture fx;
    fx.enableOverlay("OverlayWorldTimeoutTest");
    auto* api = fx.startLayer("OverlayWorldTimeoutTest");

    // Never tracked, and 1.5 s of predicted-display-time per frame so three
    // frames cross the 2 s reanchor budget.
    mock::state().locateSpaceFlags = 0;
    mock::state().predictedDisplayPeriod = 1'500'000'000;   // 1.5 s/frame

    const auto viewSpace = reinterpret_cast<XrSpace>(0xBEEF);
    auto mock = std::make_unique<MockOverlayRenderer>();
    auto* mockPtr = mock.get();
    openxr_api_layer::ForceOverlayActiveForTest(
        std::move(mock), viewSpace, reinterpret_cast<XrSpace>(0xCAFE));

    driveOneFrame(api);   // t0: start the clock, no tracking → skip
    driveOneFrame(api);   // t0+1.5s: still inside budget → skip
    driveOneFrame(api);   // t0+3.0s: past 2 s budget → give up, draw head-locked

    // Gave up: drew head-locked (VIEW space, no anchor pose), quad injected,
    // and it stopped trying to locate after the give-up.
    CHECK(mockPtr->renderAndComposeCalls >= 1);
    CHECK(mockPtr->lastSpace == viewSpace);
    CHECK_FALSE(mockPtr->lastHadAnchorPose);
    CHECK(mock::state().lastEndFrameQuadCount == 1);
    CHECK(mock::state().locateSpaceCallCount == 3);   // tried each frame, then latched off

    // A fourth frame must not re-locate — the give-up is sticky.
    driveOneFrame(api);
    CHECK(mock::state().locateSpaceCallCount == 3);
}

// Head-locked path (the default, and where world-anchor degrades when LOCAL
// creation fails): never locates a space, attaches the quad to the VIEW space
// with no frozen pose. Locks the refactored renderAndCompose contract.
TEST_CASE("overlay: head-locked attaches to the view space with no anchor pose") {
    TelemetryFixture fx;
    fx.enableOverlay("OverlayHeadLockedTest");
    auto* api = fx.startLayer("OverlayHeadLockedTest");

    const auto viewSpace = reinterpret_cast<XrSpace>(0xBEEF);
    auto mock = std::make_unique<MockOverlayRenderer>();
    auto* mockPtr = mock.get();
    // No localSpace ⇒ head-locked (the stock anchor=head behaviour).
    openxr_api_layer::ForceOverlayActiveForTest(std::move(mock), viewSpace);

    driveOneFrame(api);

    CHECK(mock::state().locateSpaceCallCount == 0);
    CHECK(mockPtr->lastSpace == viewSpace);
    CHECK_FALSE(mockPtr->lastHadAnchorPose);
    CHECK(mock::state().lastEndFrameQuadCount == 1);
    // The layer resolved the geometry once and handed it through: default
    // settings → the bumped head_top_right corner (pos_x pushed past the old
    // 0.22) and the stock quad size.
    CHECK(mockPtr->lastGeo.pos_x > 0.22f);
    CHECK(mockPtr->lastGeo.pos_y > 0.14f);
    CHECK(mockPtr->lastGeo.width_m == doctest::Approx(0.28f).epsilon(0.001));
}

// Regression: ~OpenXrLayer (instance teardown WITHOUT a prior xrDestroySession)
// used to skip resetting the renderer / xrDestroySpace'ing the view space,
// leaking the XrSpace and tearing the swapchain down against a dying session.
TEST_CASE("overlay: instance teardown releases the view space (no xrDestroySession)") {
    bool rendererDestroyed = false;
    {
        TelemetryFixture fx;
        fx.enableOverlay("OverlayTeardownTest");
        auto* api = fx.startLayer("OverlayTeardownTest");

        auto mock = std::make_unique<MockOverlayRenderer>();
        mock->destroyedFlag = &rendererDestroyed;
        openxr_api_layer::ForceOverlayActiveForTest(
            std::move(mock), reinterpret_cast<XrSpace>(0xBEEF));

        driveOneFrame(api);
        // fx's destructor runs ResetInstance() → ~OpenXrLayer with NO prior
        // xrDestroySession — the path that used to leak m_viewSpace.
    }

    CHECK(rendererDestroyed);
    CHECK(mock::state().destroySpaceCallCount >= 1);
    CHECK(mock::state().lastDestroyedSpace == reinterpret_cast<XrSpace>(0xBEEF));
}
