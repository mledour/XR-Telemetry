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
#include "cpu_telemetry.h"

#include <windows.h>   // OpenFileMappingA / MapViewOfFile / QueryPerformanceCounter

#include <atomic>      // atomic_thread_fence — seqlock acquire barriers

// =============================================================================
// cpu_telemetry.cpp — the thin Windows shell over the pure logic in the
// header. All it does is map the helper's named section read-only and pull a
// seqlock snapshot out of it; evaluateCpuTelemetryRead() in the header makes
// the actual accept/reject decision.
//
// Lifetime of the mapping: we hold our own handle + view for the whole
// session. That keeps the section object alive even if the helper exits, so
// reading the view never faults — a dead helper just leaves its last sample
// frozen, which the staleness check converts back to "no data". And because
// the named section stays alive, a RESTARTED helper re-opens the SAME section
// (CreateFileMapping on an existing name returns the existing object) and its
// writes resume flowing to our view with no reconnect needed on our side.
// =============================================================================

namespace openxr_api_layer::detail {

    CpuTelemetryReader::~CpuTelemetryReader() {
        if (m_view) {
            ::UnmapViewOfFile(m_view);
            m_view = nullptr;
        }
        if (m_mapping) {
            ::CloseHandle(static_cast<HANDLE>(m_mapping));
            m_mapping = nullptr;
        }
    }

    void CpuTelemetryReader::connect() noexcept {
        // Open-only: we never CREATE the section. If the helper isn't running
        // the name doesn't exist and OpenFileMapping returns NULL — that's the
        // "helper absent" path, not an error (no log noise, matches the GPU
        // reader's silent self-disable on non-NVIDIA hosts).
        HANDLE h = ::OpenFileMappingA(FILE_MAP_READ, FALSE, kCpuTelemetryMappingName);
        if (!h) {
            return;
        }
        void* view = ::MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(CpuTelemetryShared));
        if (!view) {
            ::CloseHandle(h);
            return;
        }
        m_mapping = h;
        m_view = view;
    }

    bool CpuTelemetryReader::init() noexcept {
        LARGE_INTEGER freq{};
        // QueryPerformanceFrequency cannot fail on anything Windows XP+; the
        // guard is belt-and-braces. 0 frequency disables the staleness gate in
        // evaluateCpuTelemetryRead (we'd rather show a possibly-stale temp than
        // permanently blank it because we couldn't read the clock).
        m_qpcFrequency = ::QueryPerformanceFrequency(&freq) ? freq.QuadPart : 0;
        connect();
        return isReady();
    }

    CpuTelemetrySample CpuTelemetryReader::poll() noexcept {
        // Lazy (re)connect so a helper launched after the game still lights up
        // the cell. Cheap kernel-object lookup; the caller already throttles
        // poll() to the overlay cadence (<= 10 Hz), so an absent helper costs
        // at most ~10 OpenFileMapping lookups/second.
        if (!m_view) {
            connect();
            if (!m_view) {
                return {};
            }
        }

        const volatile CpuTelemetryShared* p =
            static_cast<const volatile CpuTelemetryShared*>(m_view);

        LARGE_INTEGER now{};
        ::QueryPerformanceCounter(&now);

        // Seqlock read. On x86-TSO, volatile reads aren't reordered among
        // themselves and the acquire fences pair the payload reads strictly
        // between the two `seq` loads — so a writer that bumps seq mid-read is
        // always caught by the odd-seq / seq-changed checks in
        // evaluateCpuTelemetryRead. A bounded retry handles the (microsecond)
        // window where we happened to sample the writer mid-update.
        for (int attempt = 0; attempt < kCpuSeqlockReadAttempts; ++attempt) {
            const uint32_t seqBefore = p->seq;
            std::atomic_thread_fence(std::memory_order_acquire);

            const uint32_t magic    = p->magic;
            const uint32_t schema   = p->schema_version;
            const int64_t  ts       = p->timestamp_qpc;
            const float    tempC    = p->cpu_temp_c;

            std::atomic_thread_fence(std::memory_order_acquire);
            const uint32_t seqAfter = p->seq;

            if ((seqBefore & 1u) == 0u && seqBefore == seqAfter) {
                return evaluateCpuTelemetryRead(magic, schema, seqBefore, seqAfter,
                                                ts, tempC, now.QuadPart,
                                                m_qpcFrequency,
                                                kCpuTelemetryStalenessNs);
            }
        }

        // Writer kept us in a torn window for every attempt — unlikely at
        // 1-2 Hz. Treat as no data this tick; the next poll will catch up.
        return {};
    }

} // namespace openxr_api_layer::detail
