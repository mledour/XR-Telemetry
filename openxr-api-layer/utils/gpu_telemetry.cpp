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
#include "gpu_telemetry.h"

#include <dxgi1_4.h>     // IDXGIAdapter3 + QueryVideoMemoryInfo (Win10+)
#include <windows.h>     // LoadLibraryA / GetProcAddress / FreeLibrary

// =============================================================================
// gpu_telemetry.cpp — implementation. See header for the public contract.
//
// The NvAPI side is the only meaningfully tricky bit: rather than link
// against nvapi64.lib (which would make the layer DLL fail to load on
// AMD/Intel hosts at process-start time), we resolve every entry point
// dynamically by:
//
//   1. LoadLibraryA("nvapi64.dll") — returns NULL on non-NVIDIA hosts
//      and that's how we self-disable.
//   2. GetProcAddress(h, "nvapi_QueryInterface") — single exported
//      symbol that returns function pointers indexed by an integer ID.
//   3. Call nvapi_QueryInterface for each function we want, passing
//      the documented NvAPI function ID. The IDs are stable across
//      NvAPI versions (NVIDIA never re-numbers; new entry points get
//      new IDs).
//
// The IDs we use here are documented in NVIDIA's public NvAPI SDK
// headers (under `Source/nvapi.h`, search for FUNCTIONID macros).
// They are publicly known constants — no NVIDIA-confidential
// material is reproduced.
// =============================================================================

namespace {

    // ----- NvAPI surface -----------------------------------------------
    //
    // NvAPI status enum — full enum lives in NvAPI SDK's nvapi.h; we
    // only ever check for OK == 0 here.
    using NvAPI_Status = int;
    static constexpr NvAPI_Status NVAPI_OK = 0;

    // Opaque handles. Cast back from the void* fields in
    // GpuTelemetryReader.
    using NvPhysicalGpuHandle = void*;

    // NvAPI exposes a single entry point — nvapi_QueryInterface(id) —
    // which returns the function pointer for the given ID. All other
    // NvAPI entry points are resolved through it. The ID values below
    // come from NvAPI SDK's public function-ID table.
    using NvAPI_QueryInterface_t = void* (*)(unsigned int id);

    // Function IDs. These are publicly documented in NvAPI SDK
    // headers (search for FUNCTIONID in nvapi.h).
    constexpr unsigned int kID_NvAPI_Initialize         = 0x0150E828u;
    constexpr unsigned int kID_NvAPI_Unload             = 0xD22BDD7Eu;
    constexpr unsigned int kID_NvAPI_EnumPhysicalGPUs   = 0xE5AC921Fu;
    constexpr unsigned int kID_NvAPI_GPU_GetThermalSettings = 0xE3640A56u;

    // Maximum GPUs NvAPI will report in EnumPhysicalGPUs. Defined as
    // NVAPI_MAX_PHYSICAL_GPUS = 64 in the SDK header.
    constexpr int kMaxNvPhysicalGpus = 64;

    // Function pointer typedefs. Calling convention is __cdecl on
    // x86_64 (no decoration needed).
    using NvAPI_Initialize_t = NvAPI_Status (*)();
    using NvAPI_Unload_t     = NvAPI_Status (*)();
    using NvAPI_EnumPhysicalGPUs_t = NvAPI_Status (*)(
        NvPhysicalGpuHandle gpus[kMaxNvPhysicalGpus],
        unsigned long* count);

    // Thermal-settings structures. The SDK defines NV_GPU_THERMAL_SETTINGS
    // with a version stamp in the first 4 bytes (so NVIDIA can extend the
    // struct without breaking old callers). We reproduce the v2 layout
    // we care about: up to 3 sensors, each carrying a target / source /
    // current_temp / min / max. The version word encodes
    // sizeof(struct) | (version_index << 16); v2 of THERMAL_SETTINGS
    // shipped in NvAPI ~R304 and hasn't changed since.
    //
    // Layout mirrors NV_GPU_THERMAL_SETTINGS_V2 byte-for-byte. The
    // struct is plain POD; no virtuals, no vtable.
    constexpr int kNvMaxThermalSensors = 3;
    struct NV_GPU_THERMAL_SETTINGS_V2_SENSOR {
        int controller;
        int defaultMinTemp;
        int defaultMaxTemp;
        int currentTemp;
        int target;
    };
    struct NV_GPU_THERMAL_SETTINGS_V2 {
        unsigned int version;
        unsigned int count;
        NV_GPU_THERMAL_SETTINGS_V2_SENSOR sensor[kNvMaxThermalSensors];
    };
    // MAKE_NVAPI_VERSION(struct, version_index) macro — packs the
    // struct size in the low 16 bits and the version index in the
    // high 16 bits. V2 of THERMAL_SETTINGS = version index 2.
    constexpr unsigned int kThermalSettingsVer =
        sizeof(NV_GPU_THERMAL_SETTINGS_V2) | (2u << 16);

    // Sentinel for "all sensors / any target". The thermal sensor
    // controller IDs (NVAPI_THERMAL_CONTROLLER_*) are GPU-internal —
    // the GPU package sensor is typically index 0, but we just take
    // the max across all reported sensors. Equivalent to fpsVR's "hot
    // spot" behaviour: shows the worst sensor, which is what the user
    // cares about.
    using NvAPI_GPU_GetThermalSettings_t = NvAPI_Status (*)(
        NvPhysicalGpuHandle gpu,
        int sensorIndex,
        NV_GPU_THERMAL_SETTINGS_V2* thermalSettings);
    constexpr int kNvThermalSensorAll = 15;  // NVAPI_THERMAL_TARGET_ALL

} // anonymous namespace

namespace openxr_api_layer::detail {

    GpuTelemetryReader::~GpuTelemetryReader() {
        // Release IDXGIAdapter3 refcount we took via QueryInterface in
        // init(). NvAPI gets unloaded via NvAPI_Unload before
        // FreeLibrary, so the driver-side handle table is cleaned up.
        if (m_adapter3) {
            static_cast<IDXGIAdapter3*>(m_adapter3)->Release();
            m_adapter3 = nullptr;
        }
        if (m_nvapiModule) {
            if (m_nvapiUnload) {
                reinterpret_cast<NvAPI_Unload_t>(m_nvapiUnload)();
            }
            ::FreeLibrary(static_cast<HMODULE>(m_nvapiModule));
            m_nvapiModule = nullptr;
        }
    }

    bool GpuTelemetryReader::init(IDXGIAdapter* adapter) noexcept {
        // ----- DXGI VRAM side -------------------------------------------
        //
        // QueryVideoMemoryInfo lives on IDXGIAdapter3 (added in Win10
        // RS1). If the caller's IDXGIAdapter doesn't QI to v3 (old
        // Win10 build, or a stub adapter from a test harness), we
        // disable VRAM polling cleanly — temp polling still works.
        if (adapter) {
            IDXGIAdapter3* a3 = nullptr;
            if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                                                   reinterpret_cast<void**>(&a3))) &&
                a3 != nullptr) {
                m_adapter3 = a3;
                m_vramReady = true;
            }
        }

        // ----- NvAPI side -----------------------------------------------
        //
        // The DLL is present on every NVIDIA driver install; loading
        // it on an AMD/Intel system returns NULL and we leave
        // m_tempReady=false. No log noise — that's the documented
        // contract, not an error.
        HMODULE h = ::LoadLibraryA("nvapi64.dll");
        if (h) {
            auto qi = reinterpret_cast<NvAPI_QueryInterface_t>(
                ::GetProcAddress(h, "nvapi_QueryInterface"));
            if (qi) {
                auto initFn = reinterpret_cast<NvAPI_Initialize_t>(
                    qi(kID_NvAPI_Initialize));
                auto unloadFn = reinterpret_cast<NvAPI_Unload_t>(
                    qi(kID_NvAPI_Unload));
                auto enumFn = reinterpret_cast<NvAPI_EnumPhysicalGPUs_t>(
                    qi(kID_NvAPI_EnumPhysicalGPUs));
                auto thermalFn = reinterpret_cast<NvAPI_GPU_GetThermalSettings_t>(
                    qi(kID_NvAPI_GPU_GetThermalSettings));

                // Every function we need must resolve, otherwise this
                // is a partial driver / NvAPI version mismatch and
                // poll() would be unsafe.
                if (initFn && unloadFn && enumFn && thermalFn &&
                    initFn() == NVAPI_OK) {
                    // Enumerate GPUs once at init — the handle is
                    // stable for the process lifetime.
                    NvPhysicalGpuHandle gpus[kMaxNvPhysicalGpus] = {};
                    unsigned long count = 0;
                    if (enumFn(gpus, &count) == NVAPI_OK && count > 0) {
                        // Single-GPU host is the overwhelming case in
                        // VR; if the user has an SLI rig we just
                        // sample GPU 0 (the primary). fpsVR makes the
                        // same simplifying choice.
                        m_nvapiGpuHandle = gpus[0];
                        m_nvapiInitialize = reinterpret_cast<void*>(initFn);
                        m_nvapiUnload = reinterpret_cast<void*>(unloadFn);
                        m_nvapiEnumPhysicalGPUs = reinterpret_cast<void*>(enumFn);
                        m_nvapiGetThermalSettings = reinterpret_cast<void*>(thermalFn);
                        m_nvapiModule = h;
                        m_tempReady = true;
                    }
                }
            }
            if (!m_tempReady) {
                // Either the QI returned nulls, Initialize failed, or
                // EnumPhysicalGPUs found nothing. Drop the module so
                // we don't keep an idle DLL mapped into the game
                // process for no reason.
                ::FreeLibrary(h);
            }
        }

        return isReady();
    }

    GpuTelemetrySample GpuTelemetryReader::poll() noexcept {
        GpuTelemetrySample s;

        // VRAM via DXGI.
        if (m_vramReady && m_adapter3) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
            HRESULT hr = static_cast<IDXGIAdapter3*>(m_adapter3)
                ->QueryVideoMemoryInfo(0,
                                        DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                                        &info);
            if (SUCCEEDED(hr)) {
                s.vram_used_bytes = info.CurrentUsage;
                s.vram_budget_bytes = info.Budget;
                s.vram_valid = true;
            }
        }

        // Temperature via NvAPI.
        if (m_tempReady && m_nvapiGpuHandle && m_nvapiGetThermalSettings) {
            NV_GPU_THERMAL_SETTINGS_V2 ts = {};
            ts.version = kThermalSettingsVer;
            auto thermalFn = reinterpret_cast<NvAPI_GPU_GetThermalSettings_t>(
                m_nvapiGetThermalSettings);
            if (thermalFn(m_nvapiGpuHandle, kNvThermalSensorAll, &ts) == NVAPI_OK &&
                ts.count > 0 && ts.count <= kNvMaxThermalSensors) {
                // Hot-spot semantics: take the max across the reported
                // sensors. On modern NVIDIA cards this is typically
                // the package sensor (sensor[0]) but the memory and
                // VRM sensors are sometimes higher under heavy load —
                // and the user wants to see the actual bottleneck.
                int maxC = ts.sensor[0].currentTemp;
                for (unsigned int i = 1; i < ts.count; ++i) {
                    if (ts.sensor[i].currentTemp > maxC) {
                        maxC = ts.sensor[i].currentTemp;
                    }
                }
                s.gpu_temp_c = static_cast<float>(maxC);
                s.temp_valid = true;
            }
        }

        return s;
    }

} // namespace openxr_api_layer::detail
