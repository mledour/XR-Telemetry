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

#include "pch.h"
#include "cpu_usage.h"

#include <windows.h>

#include <algorithm>

// =============================================================================
// cpu_usage.cpp — implementation. See header for the public contract and the
// anti-cheat / threading rationale.
//
// We read per-core scheduler counters through
// NtQuerySystemInformation(SystemProcessorPerformanceInformation). That class
// and its result struct are NOT in the public Win32 SDK headers (they live in
// the DDK / ntddk), so we declare the minimal surface we use below. The class
// value (8) and the struct layout are stable, long-documented in the Windows
// Internals literature, and are exactly what Task Manager / Process Explorer
// consume — there is no public Win32 equivalent that yields per-core idle
// time (GetSystemTimes is whole-system only, PDH pulls in pdh.dll).
// =============================================================================

namespace {

    // One entry per logical processor. KernelTime INCLUDES IdleTime, so the
    // busy time of a core over an interval is (ΔKernel + ΔUser) − ΔIdle and
    // the total is (ΔKernel + ΔUser). Times are in 100-ns ticks.
    typedef struct _SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
        LARGE_INTEGER IdleTime;
        LARGE_INTEGER KernelTime;
        LARGE_INTEGER UserTime;
        LARGE_INTEGER DpcTime;
        LARGE_INTEGER InterruptTime;
        ULONG         InterruptCount;
    } SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION;

    // SYSTEM_INFORMATION_CLASS::SystemProcessorPerformanceInformation.
    constexpr ULONG kSystemProcessorPerformanceInformation = 8;

    // NtQuerySystemInformation(SystemInformationClass, buffer, len, retLen).
    // Returns an NTSTATUS (LONG); negative == error.
    using NtQuerySystemInformation_t =
        LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);

    // A single processor group holds at most 64 logical processors, and the
    // SystemProcessorPerformanceInformation class reports the calling
    // process's group. We cap the buffer at 64: on a >64-thread machine the
    // "busiest core" reading covers group 0 only, which is an acceptable
    // approximation for a glanceable HUD metric (and matches how most
    // single-group monitoring tools behave).
    constexpr std::uint32_t kMaxProcessors = 64;

} // namespace

namespace openxr_api_layer::detail {

    CpuUsageReader::~CpuUsageReader() = default;

    bool CpuUsageReader::init() noexcept {
        // ntdll is mapped into every Win32 process before our code runs, so
        // GetModuleHandle resolves it without adding a module to the process
        // — this is the anti-cheat-safe path (no LoadLibrary, no new entry
        // in the loaded-module list). See the header banner.
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) {
            return false;
        }
        m_ntQuerySystemInformation = reinterpret_cast<void*>(
            GetProcAddress(ntdll, "NtQuerySystemInformation"));
        if (!m_ntQuerySystemInformation) {
            return false;
        }

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        m_cpuCount = si.dwNumberOfProcessors;
        if (m_cpuCount == 0) {
            // Defensive: a 0-processor answer is impossible in practice,
            // but a zero-length query below would be meaningless.
            m_ntQuerySystemInformation = nullptr;
            return false;
        }
        if (m_cpuCount > kMaxProcessors) {
            // More logical processors than one group holds — we sample
            // group 0 only. Flag it so layer.cpp can log the limitation
            // once (the busiest core may live in an unsampled group).
            m_cpuCount = kMaxProcessors;
            m_groupTruncated = true;
        }

        try {
            m_prevIdle.assign(m_cpuCount, 0);
            m_prevTotal.assign(m_cpuCount, 0);
        } catch (...) {
            // .assign() can throw bad_alloc; in a noexcept function that
            // would call std::terminate and kill the host game — against
            // the layer's "degrade, never crash" rule. Bail to the
            // disabled state instead (near-impossible for two ~512-byte
            // vectors, but the rule is absolute).
            m_ntQuerySystemInformation = nullptr;
            return false;
        }
        m_ready = true;

        // Prime the baseline now so the first real poll() has a previous
        // sample to diff against. NOTE: this does NOT guarantee a non-"--"
        // first frame — the per-core counters only advance at the system
        // tick (~15.6 ms) and the first real poll fires within a few ms of
        // this baseline (layer.cpp's m_lastCpuUsagePollNs == 0 path), so
        // that first reading is usually still NaN ("--") until a tick
        // boundary passes (~1 s of polls). Priming is cheap insurance for
        // slower poll cadences / the case where a tick happens to land
        // between baseline and first poll.
        primeBaseline();
        return true;
    }

    void CpuUsageReader::primeBaseline() noexcept {
        // First sample establishes m_prev* / m_haveBaseline only; its NaN
        // return is intentionally discarded.
        (void)poll();
    }

    CpuUsageSample CpuUsageReader::poll() noexcept {
        CpuUsageSample out;  // NaN ("no reading") by default
        // m_ready is only set true after m_ntQuerySystemInformation is
        // confirmed non-null, so the readiness check alone is sufficient.
        if (!m_ready) {
            return out;
        }

        auto fn = reinterpret_cast<NtQuerySystemInformation_t>(
            m_ntQuerySystemInformation);

        SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION info[kMaxProcessors]{};
        const ULONG bytes = static_cast<ULONG>(
            sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) * m_cpuCount);
        ULONG returned = 0;
        const LONG status = fn(kSystemProcessorPerformanceInformation,
                               info, bytes, &returned);
        if (status < 0) {
            // Transient NT error this tick — leave the cached value alone
            // upstream by reporting "no reading".
            return out;
        }

        const std::uint32_t n = std::min<std::uint32_t>(
            m_cpuCount,
            static_cast<std::uint32_t>(
                returned / sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)));
        if (n == 0) {
            return out;
        }

        float maxPct = 0.0f;
        bool any = false;
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint64_t idle =
                static_cast<std::uint64_t>(info[i].IdleTime.QuadPart);
            // KernelTime already includes IdleTime, so total busy+idle for
            // the core is KernelTime + UserTime.
            const std::uint64_t total =
                static_cast<std::uint64_t>(info[i].KernelTime.QuadPart) +
                static_cast<std::uint64_t>(info[i].UserTime.QuadPart);

            if (m_haveBaseline) {
                // Cumulative counters are monotonic, so the unsigned deltas
                // are non-negative in every realistic case (true 64-bit
                // 100-ns wrap is ~58 000 years away).
                const std::uint64_t idleDelta  = idle - m_prevIdle[i];
                const std::uint64_t totalDelta = total - m_prevTotal[i];
                if (totalDelta > 0) {
                    const double busyFrac =
                        static_cast<double>(totalDelta - idleDelta) /
                        static_cast<double>(totalDelta);
                    const float pct = static_cast<float>(busyFrac * 100.0);
                    if (pct > maxPct) {
                        maxPct = pct;
                    }
                    any = true;
                }
            }

            m_prevIdle[i]  = idle;
            m_prevTotal[i] = total;
        }
        m_haveBaseline = true;

        if (any) {
            out.cpus_max_pct = std::clamp(maxPct, 0.0f, 100.0f);
        }
        return out;
    }

} // namespace openxr_api_layer::detail
