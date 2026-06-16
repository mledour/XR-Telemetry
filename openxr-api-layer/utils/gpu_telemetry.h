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
// gpu_telemetry.h — best-effort GPU temperature + VRAM polling.
//
// Two sources, both runtime-resolved so the layer keeps loading and the rest
// of telemetry keeps working on hosts where one or both APIs aren't
// available:
//
//   - DXGI (IDXGIAdapter3::QueryVideoMemoryInfo) for VRAM. Available since
//     Win10 RS1 on every D3D-capable adapter — NVIDIA, AMD, Intel, iGPU.
//     We hold the IDXGIAdapter externally (already cached in the session
//     setup path) and QI to the v3 interface; the only failure modes are
//     "not Win10+" (we treat as VRAM unavailable) or "QI returned null"
//     (driver too old — same treatment).
//
//   - NvAPI (nvapi64.dll) for GPU package temperature. NVIDIA-only — the
//     DLL ships with the NVIDIA display driver and is present on all
//     NVIDIA systems. Loaded via LoadLibrary so the layer keeps working
//     on AMD / Intel hosts (the load just fails and we fall back to
//     temp_valid=false). All NvAPI entry points are resolved through
//     `nvapi_QueryInterface` using the documented function IDs — we do
//     NOT link against nvapi.lib (the .lib would force a hard
//     LoadLibrary at process load, which fails on AMD/Intel).
//
// AMD coverage (ADL) and Intel Arc (IGCL) will land in a follow-up PR;
// today the temp field reads NaN on non-NVIDIA systems and the renderer
// already substitutes "--" via the isfinite() guard in
// formatOverlayDisplayValues.
//
// Anti-cheat note: NvAPI is a stock NVIDIA DLL signed by NVIDIA and
// loaded by virtually every game and benchmark tool — its presence in
// the module list of a game process is not flagged by EAC / BattlEye /
// VAC. DXGI is a Microsoft system API, same status. Neither path
// touches MSRs or installs a kernel driver, so CLAUDE.md rule 3 (signing
// + anti-cheat compatibility) is preserved.
//
// Threading: poll() is single-threaded and cheap — DXGI is sub-µs,
// NvAPI is sub-ms. Safe to call on the frame thread at the overlay
// aggregator's refresh cadence (10 Hz default). Do NOT call concurrently
// from multiple threads — the cached NvAPI handle isn't atomic.
// =============================================================================

#include <cmath>
#include <cstdint>
#include <limits>

// Forward decl — the .cpp pulls in <dxgi.h>. Keeps this header free of
// Windows headers so tests on macOS / Linux can include it for the
// GpuTelemetrySample POD.
struct IDXGIAdapter;

namespace openxr_api_layer::detail {

    // POD result of one GpuTelemetryReader::poll() call. NaN / 0 sentinels
    // mean "source unavailable on this poll" — and downstream consumers
    // (OverlayAggregator's snapshot, FrameRecord in the CSV) carry only
    // those sentinel-bearing scalars, NOT the *_valid flags. The flags
    // are an IMMEDIATE-CONSUMER convenience for code that wants to
    // distinguish "DXGI / NvAPI returned an error this tick" from
    // "they returned a clean 0/NaN" — but in practice no realistic GPU
    // reports exactly 0 bytes used, and NaN is structurally
    // unambiguous for the temperature, so the 0/NaN sentinels alone
    // are safe to propagate.
    struct GpuTelemetrySample {
        // GPU package temperature, °C. NaN if NvAPI is absent (AMD /
        // Intel), or if NvAPI returned an error. The renderer's
        // isfinite() guard in formatOverlayDisplayValues handles this
        // by substituting "--", keeping the bottom-row temp cell at
        // its fixed width.
        float gpu_temp_c = std::numeric_limits<float>::quiet_NaN();

        // Local VRAM used by THIS process (the OpenXR host). Comes from
        // DXGI_QUERY_VIDEO_MEMORY_INFO::CurrentUsage. 0 ⇒ DXGI didn't
        // answer (Win10 pre-RS1, stub adapter, or transient
        // QueryVideoMemoryInfo failure). No real GPU under a live
        // D3D11/D3D12 host reports exactly 0 bytes used — even idle
        // OpenXR runtimes hold ≥ 1 MB for compositor swapchains — so
        // 0 is a safe "no data" sentinel for downstream consumers.
        uint64_t vram_used_bytes = 0;

        // OS-allocated VRAM budget for the process. Comes from
        // DXGI_QUERY_VIDEO_MEMORY_INFO::Budget. 0 ⇒ unknown (same
        // failure modes as vram_used_bytes; same safety argument).
        uint64_t vram_budget_bytes = 0;

        // Per-source validity flags — set when poll() actually got an
        // OK status from NvAPI / DXGI on this tick. Not propagated
        // into downstream POD structs (snapshot, FrameRecord); the
        // 0/NaN sentinel on the scalar field is the sole signal those
        // consumers see. Reserved here for in-process callers that
        // care about "is this a fresh poll" vs. "still showing the
        // last good value".
        bool temp_valid = false;
        bool vram_valid = false;
    };

    // Polls GPU temperature + VRAM. Single-threaded; one instance per
    // OpenXrLayer session. Owns the NvAPI HMODULE (released in the
    // destructor) and a cached IDXGIAdapter3* (refcounted via QI from
    // the supplied adapter).
    class GpuTelemetryReader {
      public:
        GpuTelemetryReader() = default;
        ~GpuTelemetryReader();

        // No copy/move — the NvAPI handle ownership semantics make this
        // ambiguous, and we only ever construct one per session in
        // layer.cpp. Disabling explicitly keeps the contract obvious.
        GpuTelemetryReader(const GpuTelemetryReader&) = delete;
        GpuTelemetryReader& operator=(const GpuTelemetryReader&) = delete;
        GpuTelemetryReader(GpuTelemetryReader&&) = delete;
        GpuTelemetryReader& operator=(GpuTelemetryReader&&) = delete;

        // Best-effort initialisation. Sets up DXGI VRAM polling (if the
        // adapter QIs to IDXGIAdapter3) and NvAPI temperature polling
        // (if nvapi64.dll loads, NvAPI_Initialize succeeds, and an
        // NVIDIA GPU is enumerated). Returns true if AT LEAST ONE
        // source is ready; false only if both failed (in which case
        // poll() returns an empty sample but doesn't crash).
        //
        // `adapter` may be null — VRAM polling self-disables. Useful
        // for unit tests of the NvAPI side in isolation.
        //
        // The adapter pointer is borrowed (refcount taken via QI to
        // IDXGIAdapter3); the caller does NOT need to keep its
        // ComPtr<IDXGIAdapter> alive after init().
        bool init(IDXGIAdapter* adapter) noexcept;

        // True if init() ever succeeded on temperature OR VRAM.
        // Callers use this to decide whether to expose the GPU
        // telemetry fields in the overlay / CSV at all.
        bool isReady() const noexcept { return m_tempReady || m_vramReady; }

        // Single-shot poll. Never blocks more than a few hundred µs.
        // The returned sample's *_valid flags mirror m_tempReady /
        // m_vramReady at construction time, AND clear themselves if
        // the underlying API returned an error this tick (transient
        // failures — driver hiccup, etc.) so a consumer can treat the
        // sample as "fresh and trustworthy" without remembering the
        // init() outcome.
        GpuTelemetrySample poll() noexcept;

      private:
        // DXGI side. m_adapter3 is an IDXGIAdapter3*, refcount held.
        // Casting to void* keeps <dxgi.h> out of this header.
        void* m_adapter3 = nullptr;
        bool  m_vramReady = false;

        // NvAPI side. The DLL handle is held for as long as we
        // dispatch through it. Function pointers and the cached
        // physical GPU handle are NvAPI types we don't want to expose
        // here.
        void* m_nvapiModule = nullptr;
        // Resolved function pointers — cast to actual NvAPI typedefs
        // in the .cpp. Cleared if NvAPI_Initialize fails.
        void* m_nvapiInitialize = nullptr;
        void* m_nvapiUnload = nullptr;
        void* m_nvapiEnumPhysicalGPUs = nullptr;
        void* m_nvapiGetThermalSettings = nullptr;
        // First NVIDIA physical GPU enumerated. Held opaque (NvAPI
        // uses an opaque handle type). nullptr if NvAPI didn't find
        // any NVIDIA GPU on the system.
        void* m_nvapiGpuHandle = nullptr;
        bool  m_tempReady = false;
    };

} // namespace openxr_api_layer::detail
