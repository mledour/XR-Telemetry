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
// cpu_usage.h — best-effort, system-wide per-core CPU utilisation polling.
//
// Surfaces the "CPUs" metric the overlay shows next to "CPU LOAD": the
// utilisation of the BUSIEST logical processor over the interval since the
// previous poll. This is the fpsVR "CPUs" reading — distinct from the
// frame-timing-derived "CPU LOAD" (= 100 − headroom) the aggregator already
// computes. A near-100 % "CPUs" while "CPU LOAD" looks tame is the classic
// single-thread-bound signature most VR titles hit.
//
// Source: NtQuerySystemInformation(SystemProcessorPerformanceInformation),
// the same per-core idle/kernel/user counters Task Manager and Process
// Explorer read. We diff two cumulative samples to get the busy fraction
// per core, then take the max.
//
// Why this is the EASY metric (vs. CPU temperature):
//   - CPU *usage* comes from a documented, user-mode NT call. No kernel
//     driver, no MSR reads, no elevation, no out-of-process helper — it
//     runs in-process inside the layer DLL.
//   - CPU *temperature*, by contrast, needs ring-0 access (LHM / PawnIO),
//     which is why it lives behind the separate out-of-process helper
//     design. This file deliberately does NOT touch temperature.
//
// Anti-cheat note (CLAUDE.md rule 3): NtQuerySystemInformation is resolved
// from the ALREADY-LOADED ntdll via GetModuleHandle — never LoadLibrary —
// so the layer adds no new module to the game process and trips no
// module-load hook. The call only reads global scheduler counters; it
// touches no MSRs and installs no driver. It is the same path Task Manager
// uses, so its presence in a game process is unremarkable.
//
// Threading: poll() is single-threaded and cheap (one syscall + a loop
// over ≤ 64 cores). Safe to call on the frame thread at the overlay
// aggregator's poll cadence (~1 Hz). Do NOT call concurrently from
// multiple threads — the previous-sample buffers aren't synchronised.
//
// Header stays free of <windows.h> (only <vector> + scalars) so the unit
// tests can include it for the CpuUsageSample POD on macOS / Linux — same
// portability contract as gpu_telemetry.h.
// =============================================================================

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace openxr_api_layer::detail {

    // POD result of one CpuUsageReader::poll() call.
    struct CpuUsageSample {
        // Busiest logical processor's utilisation, percent [0, 100], over
        // the interval since the previous poll. NaN means "no reading":
        // either the very first poll after init() (baseline only — a delta
        // needs two samples) or the source is unavailable
        // (NtQuerySystemInformation didn't resolve). The overlay's
        // isfinite() guard in formatOverlayDisplayValues renders NaN as
        // the "--" placeholder, keeping the cell at its fixed width.
        float cpus_max_pct = std::numeric_limits<float>::quiet_NaN();

        // True when this poll produced a real delta-based reading. An
        // immediate-consumer convenience (matches GpuTelemetrySample's
        // *_valid flags); downstream POD consumers rely only on the NaN
        // sentinel on cpus_max_pct.
        bool valid = false;
    };

    // Polls per-core CPU utilisation and reports the busiest core. One
    // instance per OpenXrLayer session. Holds the previous cumulative
    // per-core counters so each poll() reports the delta since the last.
    class CpuUsageReader {
      public:
        CpuUsageReader() = default;
        ~CpuUsageReader();

        // No copy/move — single instance per session in layer.cpp, and the
        // resolved-function-pointer + previous-sample state make the
        // copy/move semantics ambiguous. Disabled explicitly.
        CpuUsageReader(const CpuUsageReader&) = delete;
        CpuUsageReader& operator=(const CpuUsageReader&) = delete;
        CpuUsageReader(CpuUsageReader&&) = delete;
        CpuUsageReader& operator=(CpuUsageReader&&) = delete;

        // Best-effort init. Resolves NtQuerySystemInformation from the
        // loaded ntdll, sizes the per-core buffers, and takes a baseline
        // sample so the first user-facing poll() already has a delta to
        // diff against. Returns true if the function resolved (poll() can
        // then produce readings); false on the rare host where ntdll
        // doesn't expose the symbol — in which case poll() returns a NaN
        // sample and the layer keeps working with "--" in the cell.
        bool init() noexcept;

        // True once init() resolved the NT call. Callers gate polling on
        // this the same way they gate on GpuTelemetryReader::isReady().
        bool isReady() const noexcept { return m_ready; }

        // Single-shot poll. Reads the per-core counters, diffs them against
        // the previous sample, and returns the max busy% across all
        // logical processors. The first call after init()'s baseline
        // returns a real reading; on an un-init'd / unavailable reader it
        // returns the NaN sentinel. Never blocks meaningfully.
        CpuUsageSample poll() noexcept;

      private:
        // Resolved NtQuerySystemInformation. Stored as void* so this header
        // stays <windows.h>-free; the .cpp casts it to the real signature.
        // nullptr until init() succeeds.
        void* m_ntQuerySystemInformation = nullptr;

        // Logical processors sampled (≤ 64 — one processor group; see the
        // .cpp for the >64-core caveat). 0 until init().
        std::uint32_t m_cpuCount = 0;

        // Previous cumulative per-core idle and total (kernel+user) counts,
        // in 100-ns ticks, indexed by processor. Diffed each poll. Plain
        // integers so no Windows types leak into this header.
        std::vector<std::uint64_t> m_prevIdle;
        std::vector<std::uint64_t> m_prevTotal;

        // False until the first sample has populated m_prev* — guards the
        // first poll from reporting a bogus delta against zeroed buffers.
        bool m_haveBaseline = false;
        bool m_ready = false;
    };

} // namespace openxr_api_layer::detail
