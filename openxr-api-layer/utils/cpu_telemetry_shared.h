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
// cpu_telemetry_shared.h — the WIRE CONTRACT for the CPU-temperature
// shared-memory channel.
//
// This is the single source of truth shared (conceptually) between:
//   * the in-game READER — CpuTelemetryReader in cpu_telemetry.{h,cpp}, which
//     only ever MAPS this block read-only and never touches a kernel driver,
//   * the out-of-process WRITER — a small signed helper built on
//     LibreHardwareMonitorLib (which itself reads CPU temperature through
//     PawnIO >= 2.2.0). The helper is a SEPARATE deliverable; this layer
//     ships only the reader.
//
// Why a separate process at all: reading an Intel/AMD CPU die temperature
// requires ring-0 access (MSR / SMN), i.e. a kernel driver. We refuse to
// open a kernel-driver handle from inside the game process (anti-cheat
// behavioural heuristics scan the protected process's handles + modules,
// and CLAUDE.md rule 9 forbids anything that could crash the host). So the
// privileged read lives in the helper; the game process only ever reads
// this plain shared-memory block — exactly the pattern HWiNFO / FanControl /
// LibreHardwareMonitor follow. See gpu_telemetry.h for the sibling
// "stock signed DLL, no MSR" argument on the GPU side.
//
// ── Memory layout (x86-64, little-endian) ────────────────────────────────
//   offset  size  field
//   0       4     magic            == kCpuTelemetryMagic ('XRTC')
//   4       4     schema_version   == kCpuTelemetrySchemaVersion
//   8       4     seq              seqlock counter (odd while writing)
//   12      4     reserved         padding → 8-byte align timestamp_qpc
//   16      8     timestamp_qpc    QueryPerformanceCounter() at last write
//   24      4     cpu_temp_c       package/Tctl °C, NaN if helper has none
//   28      4     cpu_temp_max     optional Tjmax/limit °C, NaN if unknown
//   ── total 32 bytes, no implicit padding ──
//
// ── Writer protocol (the helper MUST follow this) ────────────────────────
//   1. On startup: CreateFileMapping(name=kCpuTelemetryMappingName,
//      size >= sizeof(CpuTelemetryShared)) with a security descriptor that
//      grants FILE_MAP_READ to "ALL APPLICATION PACKAGES" (and All Restricted
//      Packages) so sandboxed OpenXR identities (WebXR in Chrome, OpenXR Tools
//      for WMR) can map it — see CLAUDE.md best-practice rule 5.
//   2. Write magic + schema_version once.
//   3. Per update (~1-2 Hz):
//        seq = seq + 1;            // now ODD — "write in progress"
//        <release fence>
//        timestamp_qpc = QPC();    // system-wide on Windows → comparable
//        cpu_temp_c    = reading;  // NaN if no live reading this tick
//        cpu_temp_max  = limit;    // or NaN
//        <release fence>
//        seq = seq + 1;            // now EVEN — "write complete"
//   The reader detects a torn read by seeing an odd seq, or seq changing
//   between its two acquire-loads, and simply skips that tick (NaN).
//
// QPC note: QueryPerformanceCounter is consistent across processes on the
// same machine, so the reader can compare its own QPC against the writer's
// timestamp_qpc to detect a dead/frozen helper (staleness → NaN).
//
// Stays self-contained — only <cstdint> / <cstddef>. No Windows headers, so
// the test binary picks it up on macOS / Linux.
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace openxr_api_layer::detail {

    // Block identifier — the four ASCII bytes 'X','R','T','C' laid out in
    // memory order, read back as a little-endian uint32. Lets the reader
    // reject a mapping that happens to exist under the same name but was
    // created by something else (or a future incompatible writer).
    inline constexpr uint32_t kCpuTelemetryMagic = 0x43545258u; // 'X' 'R' 'T' 'C'

    // Bump on any incompatible layout change. The reader rejects a mapping
    // whose schema_version it doesn't recognise (→ "--"), so an old layer
    // never misreads a newer helper's block.
    inline constexpr uint32_t kCpuTelemetrySchemaVersion = 1u;

    // Named section in the per-session ("Local\") namespace. Writer and
    // reader normally share the same interactive session, so Local\ is the
    // right scope; cross-identity access is granted by the writer's ACL, not
    // by widening the namespace to Global\ (which would need SeCreateGlobal).
    inline constexpr char kCpuTelemetryMappingName[] = "Local\\XrTelemetryCpu";

    // The helper publishes at ~1-2 Hz; anything older than this means it
    // crashed, was killed, or its view froze (we keep our handle open, so a
    // dead helper leaves the last sample frozen — staleness is how we turn
    // that back into "--"). 2 s leaves comfortable margin over a 1 Hz writer.
    inline constexpr int64_t kCpuTelemetryStalenessNs = 2'000'000'000LL;

    // The shared block itself. Plain POD; the explicit `reserved` word keeps
    // the 64-bit timestamp 8-byte aligned with NO implicit padding, so the
    // .NET writer's StructLayout(Sequential) marshals to the identical bytes.
    struct CpuTelemetryShared {
        uint32_t magic;
        uint32_t schema_version;
        uint32_t seq;
        uint32_t reserved;
        int64_t  timestamp_qpc;
        float    cpu_temp_c;
        float    cpu_temp_max;
    };

    // Layout guards — if any of these fire, the .NET writer's marshalling
    // would silently disagree with the reader. Keep them in lock-step with
    // the "Memory layout" table above.
    static_assert(sizeof(CpuTelemetryShared) == 32,
                  "CpuTelemetryShared must be exactly 32 bytes — the wire "
                  "contract the helper marshals to");
    static_assert(offsetof(CpuTelemetryShared, seq) == 8, "seq @ 8");
    static_assert(offsetof(CpuTelemetryShared, timestamp_qpc) == 16, "timestamp_qpc @ 16");
    static_assert(offsetof(CpuTelemetryShared, cpu_temp_c) == 24, "cpu_temp_c @ 24");
    static_assert(offsetof(CpuTelemetryShared, cpu_temp_max) == 28, "cpu_temp_max @ 28");

} // namespace openxr_api_layer::detail
