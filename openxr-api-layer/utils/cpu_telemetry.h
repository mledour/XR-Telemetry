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
// cpu_telemetry.h — best-effort CPU package temperature, read from a
// shared-memory block published by an out-of-process helper.
//
// Mirrors the shape of gpu_telemetry.h (POD sample + a thin reader with
// init()/poll()/isReady()) so the layer.cpp call sites stay symmetric. The
// crucial difference is the SOURCE:
//
//   - GPU temperature comes from a stock signed vendor DLL (NvAPI) loaded
//     INSIDE the game process — anti-cheat-clean, no kernel driver.
//   - CPU die temperature has no such user-mode path; it needs ring-0
//     (MSR / SMN). So we DO NOT read it in-process. Instead a separate
//     signed helper (built on LibreHardwareMonitorLib + PawnIO >= 2.2.0)
//     does the privileged read and publishes it; THIS reader only maps the
//     resulting plain shared-memory block read-only. The game process never
//     opens a kernel-driver handle. See cpu_telemetry_shared.h for the full
//     rationale and the wire contract.
//
// When no helper is running (the default — the layer ships no helper), the
// mapping is absent, poll() returns NaN, and the overlay's fmtTempInt() guard
// renders "--" in the CPU TEMP cell. Zero behavioural change for users who
// don't install the helper.
//
// Anti-cheat note: OpenFileMapping + MapViewOfFile on a Local\ named section
// is an ordinary kernel-object lookup, not file I/O and not a driver handle.
// It adds nothing to the game process's module list and touches no MSR — the
// same "no surprise driver" posture as the GPU side.
//
// Threading: poll() is single-threaded and cheap (a seqlock read of 32 bytes
// + a QueryPerformanceCounter). Call it on the frame thread at the overlay
// aggregator's cadence, exactly like GpuTelemetryReader::poll().
//
// Stays free of <windows.h> — the reader's handle/view are held as void* so
// this header (and the pure evaluate helper below) compile in the test binary
// on macOS / Linux.
// =============================================================================

#include "cpu_telemetry_shared.h"

#include "../telemetry_internals.h"  // qpcToNs

#include <cmath>
#include <cstdint>
#include <limits>

namespace openxr_api_layer::detail {

    // POD result of one CpuTelemetryReader::poll(). NaN means "no usable
    // reading this tick" — helper absent, mapping torn, stale, or the helper
    // explicitly published NaN (it has a driver but no live sensor value).
    // The renderer's isfinite() guard in formatOverlayDisplayValues turns
    // that into "--", keeping the CPU TEMP cell at its fixed width.
    struct CpuTelemetrySample {
        float cpu_temp_c = std::numeric_limits<float>::quiet_NaN();
        // True only when poll() got a fresh, untorn, non-stale, finite
        // reading this tick. Like GpuTelemetrySample::temp_valid, this is an
        // immediate-consumer convenience; downstream POD structs carry only
        // the NaN sentinel on cpu_temp_c.
        bool  temp_valid = false;
    };

    // Pure decision function: given the values a seqlock read pulled out of
    // the shared block (two acquire-loads of `seq` bracketing the payload),
    // decide whether they constitute a usable temperature.
    //
    // Factored out of the Windows reader so every branch — bad magic, schema
    // mismatch, torn read (odd seq / seq changed mid-read), stale timestamp,
    // backwards clock, non-finite temp — is unit-testable without a real
    // mapping or a real clock. CpuTelemetryReader::poll() performs the
    // volatile reads and hands the results here.
    //
    // Staleness is checked in QPC ticks converted to ns via qpcToNs (the
    // same helper the rest of the layer uses), so the writer's timestamp_qpc
    // and the reader's nowQpc must come from the same machine's QPC (they do
    // — QPC is system-wide on Windows). qpcFrequency <= 0 or stalenessNs <= 0
    // disables the staleness gate (used by tests that don't model a clock).
    inline CpuTelemetrySample evaluateCpuTelemetryRead(uint32_t magic,
                                                       uint32_t schema_version,
                                                       uint32_t seq_before,
                                                       uint32_t seq_after,
                                                       int64_t timestamp_qpc,
                                                       float cpu_temp_c,
                                                       int64_t nowQpc,
                                                       int64_t qpcFrequency,
                                                       int64_t stalenessNs) noexcept {
        CpuTelemetrySample s;  // NaN / invalid by default

        // Wrong block, or a writer whose layout we don't understand.
        if (magic != kCpuTelemetryMagic) return s;
        if (schema_version != kCpuTelemetrySchemaVersion) return s;

        // Seqlock: an odd seq means the writer was mid-update when we started;
        // a changed seq means it updated WHILE we were reading the payload.
        // Either way the payload bytes may be torn — skip this tick.
        if ((seq_before & 1u) != 0u) return s;
        if (seq_before != seq_after) return s;

        // Staleness: a frozen helper leaves the last sample in memory (we keep
        // the section alive via our own handle), so a stale timestamp is the
        // only signal that the helper died. A timestamp in the future
        // (nowQpc < timestamp_qpc) is a bogus write / clock glitch — reject.
        if (qpcFrequency > 0 && stalenessNs > 0) {
            const int64_t ageQpc = nowQpc - timestamp_qpc;
            if (ageQpc < 0) return s;
            if (qpcToNs(ageQpc, qpcFrequency) > stalenessNs) return s;
        }

        // The helper may legitimately publish NaN (driver present but no live
        // reading this tick). Treat that as "no data".
        if (!std::isfinite(cpu_temp_c)) return s;

        s.cpu_temp_c = cpu_temp_c;
        s.temp_valid = true;
        return s;
    }

    // Maps the helper's shared-memory block and reads CPU package temperature
    // from it. Single-threaded; one instance per OpenXrLayer session. Owns the
    // file-mapping HANDLE and the mapped view (both held as void* so this
    // header stays <windows.h>-free).
    class CpuTelemetryReader {
      public:
        CpuTelemetryReader() = default;
        ~CpuTelemetryReader();

        // No copy/move — owns OS handles, and we construct exactly one per
        // session in layer.cpp. Same contract as GpuTelemetryReader.
        CpuTelemetryReader(const CpuTelemetryReader&) = delete;
        CpuTelemetryReader& operator=(const CpuTelemetryReader&) = delete;
        CpuTelemetryReader(CpuTelemetryReader&&) = delete;
        CpuTelemetryReader& operator=(CpuTelemetryReader&&) = delete;

        // Best-effort init. Caches the QPC frequency for the staleness math
        // and attempts a first connect to the helper's mapping. Returns true
        // if the mapping was present at init time; false otherwise — but a
        // false return is NOT terminal: poll() lazily retries the connect, so
        // a helper started AFTER the game still lights up the CPU TEMP cell.
        // The return value is for the diagnostic log line only.
        bool init() noexcept;

        // True if currently mapped to the helper's block. Best-effort /
        // advisory: poll() works (and keeps trying to connect) regardless, so
        // callers should gate polling on the reader merely existing, not on
        // isReady(). Exposed for the init() log line and tests.
        bool isReady() const noexcept { return m_view != nullptr; }

        // Single-shot poll. Lazily (re)connects if not yet mapped, then does a
        // bounded seqlock read. Returns NaN (temp_valid=false) when the helper
        // is absent, the read tore, the sample is stale, or the temp is
        // non-finite. Never blocks.
        CpuTelemetrySample poll() noexcept;

      private:
        // OpenFileMapping + MapViewOfFile, read-only. Leaves the members null
        // (and the reader simply "not connected") if the helper isn't running.
        void connect() noexcept;

        void* m_mapping = nullptr;   // HANDLE from OpenFileMappingA
        void* m_view    = nullptr;   // const CpuTelemetryShared* from MapViewOfFile
        int64_t m_qpcFrequency = 0;  // QueryPerformanceFrequency, cached at init
    };

    // Bounded retries for the seqlock read before giving up for this tick. A
    // 1-2 Hz writer holds the odd-seq window for microseconds, so even one
    // retry is almost always enough; 4 is generous head-room without ever
    // spinning meaningfully on the frame thread.
    inline constexpr int kCpuSeqlockReadAttempts = 4;

} // namespace openxr_api_layer::detail
