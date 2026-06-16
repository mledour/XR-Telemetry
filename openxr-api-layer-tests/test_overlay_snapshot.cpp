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
// test_overlay_snapshot.cpp — visual-regression snapshot of the overlay.
//
// Renders the HUD with hard-coded mock data (the same numbers from the
// design mockup) through the GPU pipeline — the actual shipping path:
// chrome shapes + glyph atlas + instanced bars, on a WARP D3D11 device
// rendering into a BGRA8 texture. The texture is read back via a
// staging copy and encoded to overlay_snapshot.png in the test binary's
// working directory. CI uploads the PNG as a build artifact so a
// reviewer can eyeball the rendering without spinning up a HMD.
//
// WARP (software rasteriser) is deterministic run-to-run and present on
// GitHub's GPU-less runners, so the exact-pixel golden comparison stays
// stable across CI runs. The golden was regenerated from WARP output
// when the snapshot moved off the legacy D2D path.
//
// Why a doctest TEST_CASE rather than a standalone EXE: the existing
// tests project already pulls in overlay_renderer.cpp (along with the
// glyph-atlas / chrome-shape renderers + DirectWrite), so a single
// binary covers BOTH the unit tests AND the snapshot generation. No
// new vcxproj, no new solution entry, no order-of-build worries.
//
// The atlas builder's bundled-Rajdhani path uses FindResource against
// the running module's resource table. The test EXE has its OWN copy
// of the Rajdhani resources via openxr-api-layer-tests.rc (see "Snapshot
// tests" in docs/DEVELOPMENT.md), so the test bakes the same atlas as
// the in-headset HUD. It renders through the same GPU pipeline too
// (WARP here vs the user's GPU in-headset), so the snapshot matches
// the real output modulo rasteriser AA. The shared
// bundled_fonts.rc.inc include guarantees the test and the production
// DLL embed byte-identical font tables.
// =============================================================================

#include <doctest/doctest.h>

// Windows + D3D11 / WIC stack. The snapshot now renders through the
// GPU pipeline (the actual shipping path) rather than D2D, so we need
// d3d11 here; WIC stays for PNG encode + golden comparison.
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <d3d11.h>
#include <wincodec.h>

#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "utils/histogram_ring.h"
#include "utils/overlay_aggregator.h"
#include "utils/overlay_renderer.h"

using Microsoft::WRL::ComPtr;

namespace {

    // Mock OverlaySnapshot matching the design mockup numbers.
    openxr_api_layer::detail::OverlaySnapshot makeMockSnapshot() {
        openxr_api_layer::detail::OverlaySnapshot s;
        s.valid       = true;
        s.version     = 1;
        s.fps_instant = 142.0f;
        s.fps_avg     = 138.0f;
        s.fps_p95     = 124.0f;
        s.fps_p99     = 108.0f;
        s.fps_p99_9   =  98.0f;
        s.target_fps  = 90.0f;   // 90 Hz — the most common VR refresh. Drives
                                 // the budget (11.1 ms), the budget line, and
                                 // the histograms' left-hand ms axis (→ 0/5/10).
                                 // Matches the original 90 Hz design mockup; an
                                 // fpsVR-style HUD shows render-rate FPS (142),
                                 // which can sit above the display rate.
        s.gpu_frame_ms = 6.7f;
        s.cpu_frame_ms   = 7.4f;   // CPU FRAMETIME total (per-cycle)
        s.cpu_render_ms  = 2.7f;   // Render (Begin-exit → End)
        s.cpu_utilisation_pct = 78.0f;
        s.gpu_utilisation_pct = 92.0f;
        s.cpus_max_pct = 98.0f;  // busiest core — "CPUs LOAD" cell
        s.gpu_temp_c   = 67.0f;
        s.vram_used_bytes   = 6'080'000'000ULL;  // ≈ 5.66 GB
        s.vram_budget_bytes = 8'000'000'000ULL;  // 8 GB → 76 %
        return s;
    }

    // Rising-ramp histogram with two stutter spikes. A few empty slots
    // at the LEADING (oldest) end only — they render as the 2-px "no-data"
    // dash, so the golden still covers that placeholder path. The trailing
    // (newest) slots are always filled: in a live session the most recent
    // frames are never missing, so the bars run flush to the right edge
    // (only the oldest end empties, during ring warm-up).
    void fillSyntheticRing(openxr_api_layer::detail::HistogramRing<
                              openxr_api_layer::detail::kOverlayHistoRingSize>& ring,
                            float base_ms, float amp_ms, float stutter_ms,
                            int spike_a, int spike_b) {
        constexpr int N = static_cast<int>(
            openxr_api_layer::detail::kOverlayHistoRingSize);
        constexpr int kEmptyHead = 15;  // oldest slots: warm-up / dash test
        constexpr int kEmptyTail = 0;   // newest frames always present →
                                        // bars reach the right edge
        for (int i = 0; i < N; ++i) {
            int64_t ns = 0;
            if (i < kEmptyHead || i >= N - kEmptyTail) {
                ns = 0;  // placeholder dash
            } else if (i == spike_a || i == spike_b) {
                ns = static_cast<int64_t>(stutter_ms * 1.0e6f);
            } else {
                const float t = static_cast<float>(i - kEmptyHead) /
                                 static_cast<float>(N - kEmptyHead - kEmptyTail);
                const float wave = 0.5f *
                    (1.0f + std::sin((t - 0.5f) * 3.14159265f));  // 0..1, monotonic
                const float ms = base_ms + amp_ms * (wave - 0.5f) * 2.0f;
                ns = static_cast<int64_t>(std::max(0.0f, ms) * 1.0e6f);
            }
            ring.push(ns);
        }
    }

    // RAII helper for CoInitializeEx so the test scope can't leak the
    // apartment if a REQUIRE() short-circuits early.
    struct ComScope {
        HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(hr)) ::CoUninitialize(); }
    };

    // Resolve the golden snapshot path relative to THIS .cpp file's
    // build-time location. Using __FILE__ rather than the test EXE's
    // CWD makes the test robust to whichever working directory the
    // test runner picks (CI runs from the workspace root, VS Test
    // Explorer from bin\x64\<cfg>\, manual local from anywhere).
    //
    // The repo layout is:
    //   <repo>/
    //     openxr-api-layer-tests/test_overlay_snapshot.cpp  ← __FILE__
    //     screenshots/overlay_snapshot.png                  ← golden
    //
    // So the golden lives at __FILE__/../../screenshots/...
    std::filesystem::path goldenPngPath() {
        namespace fs = std::filesystem;
        return fs::path(__FILE__).parent_path().parent_path()
            / "screenshots" / "overlay_snapshot.png";
    }

    // Decode a PNG (any pixel format) into a 32bpp BGRA byte buffer.
    // BGRA — not PBGRA — to dodge the alpha-premultiplied-vs-straight
    // mismatch that WIC's PNG decoder produces depending on whether
    // the encoder side wrote a tRNS chunk. Both the golden and the
    // freshly-rendered image are decoded through THIS same path, so the
    // convention cancels in the byte-for-byte compare regardless of a
    // pixel's alpha — including the rounded frame/panel corners, whose
    // anti-aliased edges carry fractional alpha (not just 0x00 / 0xFF),
    // and the fully-transparent area outside the rounded frame.
    bool decodePngToBgra(IWICImagingFactory* wic, const wchar_t* path,
                         std::vector<BYTE>& outPixels,
                         UINT& outW, UINT& outH) {
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wic->CreateDecoderFromFilename(
                path, nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand,
                decoder.GetAddressOf()))) {
            return false;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(wic->CreateFormatConverter(conv.GetAddressOf()))) return false;
        if (FAILED(conv->Initialize(
                frame.Get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeMedianCut))) {
            return false;
        }
        if (FAILED(conv->GetSize(&outW, &outH))) return false;
        const UINT stride = outW * 4;
        outPixels.assign(static_cast<std::size_t>(stride) * outH, 0);
        return SUCCEEDED(conv->CopyPixels(
            nullptr, stride,
            static_cast<UINT>(outPixels.size()), outPixels.data()));
    }

    // Pixel-diff report. Counts pixels (NOT bytes) where any of B/G/R/A
    // differ, and records the first such pixel for the failure message.
    struct PixelDiff {
        std::size_t differingPixels = 0;
        int firstX = -1;
        int firstY = -1;
    };

    PixelDiff comparePixels(const std::vector<BYTE>& a,
                             const std::vector<BYTE>& b,
                             UINT w, UINT h) {
        PixelDiff d{};
        const std::size_t pixelCount = static_cast<std::size_t>(w) * h;
        for (std::size_t i = 0; i < pixelCount; ++i) {
            const std::size_t off = i * 4;
            if (a[off]   != b[off]   ||
                a[off+1] != b[off+1] ||
                a[off+2] != b[off+2] ||
                a[off+3] != b[off+3]) {
                if (d.differingPixels == 0) {
                    d.firstX = static_cast<int>(i % w);
                    d.firstY = static_cast<int>(i / w);
                }
                ++d.differingPixels;
            }
        }
        return d;
    }

} // namespace

TEST_CASE("overlay snapshot — render mock to PNG (visual-regression artifact)") {
    ComScope com;
    REQUIRE((com.hr == S_OK || com.hr == S_FALSE));

    // WIC factory.
    ComPtr<IWICImagingFactory> wic;
    REQUIRE(SUCCEEDED(::CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(wic.GetAddressOf()))));

    // Physical (supersampled) overlay dimensions — the exact size
    // renderOverlayToTextureD3D11 renders at. Queried from the renderer so the
    // snapshot tracks kOverlaySupersample automatically: no manual lockstep
    // with kTexW / kTexH or the supersample factor (bump the factor and the
    // target + golden size follow). The golden PNG must be regenerated when
    // these change — that's the deliberate, reviewed visual baseline.
    UINT W = 0, H = 0;
    openxr_api_layer::detail::overlaySnapshotTargetSize(W, H);

    // WARP D3D11 device. WARP (software rasteriser) is deterministic
    // run-to-run and available on GitHub's GPU-less CI runners, while
    // fully supporting the overlay's SM 4.0 shaders. BGRA_SUPPORT isn't
    // required by renderOverlayToTextureD3D11 anymore (the GPU shaders
    // render BGRA8 regardless), but it's harmless to keep and matches
    // what a real app's D3D11 device usually requests.
    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> ctx;
    {
        const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got{};
        REQUIRE(SUCCEEDED(::D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            want, 1, D3D11_SDK_VERSION,
            device.GetAddressOf(), &got, ctx.GetAddressOf())));
    }

    // Target the GPU pipeline paints into: BGRA8_UNORM render target,
    // plus a CPU-readable staging copy we map to pull pixels out.
    auto makeTex = [&](D3D11_USAGE usage, UINT bind, UINT cpu,
                        ComPtr<ID3D11Texture2D>& out) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = usage; d.BindFlags = bind; d.CPUAccessFlags = cpu;
        return SUCCEEDED(device->CreateTexture2D(&d, nullptr, out.GetAddressOf()));
    };
    ComPtr<ID3D11Texture2D> target, staging;
    REQUIRE(makeTex(D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, target));
    REQUIRE(makeTex(D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, staging));

    // Mock snapshot + synthetic histograms.
    using namespace openxr_api_layer::detail;
    const auto snap = makeMockSnapshot();
    HistogramRing<kOverlayHistoRingSize> cpuRing;
    HistogramRing<kOverlayHistoRingSize> gpuRing;
    fillSyntheticRing(cpuRing, /*base=*/7.4f, /*amp=*/1.5f,
                       /*stutter=*/12.0f, /*spike_a=*/60, /*spike_b=*/85);
    fillSyntheticRing(gpuRing, /*base=*/6.7f, /*amp=*/1.8f,
                       /*stutter=*/11.0f, /*spike_a=*/55, /*spike_b=*/90);

    // Render through the SAME GPU pipeline the in-headset D3D11 path
    // uses (chrome shapes + glyph atlas + instanced bars). errOut
    // captures which step failed for a useful doctest message.
    std::string err;
    const bool ok = openxr_api_layer::detail::renderOverlayToTextureD3D11(
        device.Get(), ctx.Get(), target.Get(), snap, cpuRing, gpuRing, &err);
    INFO("renderOverlayToTextureD3D11 step: " << err);
    REQUIRE(ok);

    // Pull the rendered pixels back: copy the GPU target into the
    // staging texture, map it, and pack into a tight W*4 BGRA buffer
    // (the mapped RowPitch may exceed W*4, so copy row by row), then
    // wrap as a WIC bitmap for the existing PNG encode + golden path.
    ctx->CopyResource(staging.Get(), target.Get());
    std::vector<BYTE> readback(static_cast<std::size_t>(W) * H * 4);
    {
        D3D11_MAPPED_SUBRESOURCE map{};
        REQUIRE(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
        const BYTE* src = static_cast<const BYTE*>(map.pData);
        for (UINT y = 0; y < H; ++y) {
            std::memcpy(readback.data() + static_cast<std::size_t>(y) * W * 4,
                        src + static_cast<std::size_t>(y) * map.RowPitch,
                        static_cast<std::size_t>(W) * 4);
        }
        ctx->Unmap(staging.Get(), 0);
    }
    ComPtr<IWICBitmap> bitmap;
    REQUIRE(SUCCEEDED(wic->CreateBitmapFromMemory(
        W, H, GUID_WICPixelFormat32bppPBGRA, W * 4,
        static_cast<UINT>(readback.size()), readback.data(),
        bitmap.GetAddressOf())));

    // Encode to PNG. Filename relative to the test EXE's CWD.
    //
    // The encoder + stream are scoped inside a { } block so they
    // release their refcounts (and the underlying file handle) on
    // exit. WIC's IWICStream wraps a Windows HANDLE opened with
    // GENERIC_WRITE — the next decoder we'll create against the
    // same path needs GENERIC_READ + FILE_SHARE_READ. Without the
    // scope, CreateDecoderFromFilename returns 0x80070020 (sharing
    // violation) and decodePngToBgra fails — which is exactly what
    // the first golden-comparison CI run hit.
    constexpr const wchar_t* kOut = L"overlay_snapshot.png";
    {
        ComPtr<IWICStream> stream;
        REQUIRE(SUCCEEDED(wic->CreateStream(stream.GetAddressOf())));
        REQUIRE(SUCCEEDED(stream->InitializeFromFilename(kOut, GENERIC_WRITE)));

        ComPtr<IWICBitmapEncoder> encoder;
        REQUIRE(SUCCEEDED(wic->CreateEncoder(
            GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())));
        REQUIRE(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)));

        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2>         encoderOptions;
        REQUIRE(SUCCEEDED(encoder->CreateNewFrame(
            frame.GetAddressOf(), encoderOptions.GetAddressOf())));
        REQUIRE(SUCCEEDED(frame->Initialize(encoderOptions.Get())));
        REQUIRE(SUCCEEDED(frame->SetSize(W, H)));

        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppPBGRA;
        REQUIRE(SUCCEEDED(frame->SetPixelFormat(&pixelFormat)));
        REQUIRE(SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)));
        REQUIRE(SUCCEEDED(frame->Commit()));
        REQUIRE(SUCCEEDED(encoder->Commit()));
    }
    // Stream, encoder, frame, options all released here — the file
    // handle is closed and the PNG is now safe to re-open for read.

    // -------- Visual-regression check vs golden ---------------------------
    //
    // The freshly-rendered HUD MUST match the committed golden at
    // screenshots/overlay_snapshot.png pixel-for-pixel. WIC software
    // rendering is deterministic, the bundled font is identical
    // across runners, and the mock snapshot is hard-coded — so any
    // diff is a real regression in the renderer.
    //
    // Both halves are decoded from PNG on disk: the golden directly,
    // the fresh from the overlay_snapshot.png we just wrote above.
    // That way both buffers go through the SAME PNG encode +
    // un-premultiply + decode pipeline, so the comparison only
    // surfaces real rendering differences — not encode-precision
    // noise from PBGRA→BGRA conversion paths.
    //
    // On mismatch, doctest prints how many pixels differ and the
    // (x,y) of the first one. The freshly-rendered PNG is left on
    // disk at overlay_snapshot.png for local-dev inspection. To
    // accept an intentional change, regenerate the golden via the
    // (forthcoming) manual workflow.
    {
        const std::filesystem::path goldenPath = goldenPngPath();
        INFO("golden expected at: " << goldenPath.string());
        REQUIRE_MESSAGE(std::filesystem::exists(goldenPath),
                         "golden snapshot missing — run the update-snapshot "
                         "workflow to seed screenshots/overlay_snapshot.png");

        std::vector<BYTE> goldenPx, freshPx;
        UINT gw = 0, gh = 0, fw = 0, fh = 0;
        REQUIRE(decodePngToBgra(wic.Get(), goldenPath.wstring().c_str(),
                                 goldenPx, gw, gh));
        REQUIRE(decodePngToBgra(wic.Get(), kOut, freshPx, fw, fh));

        CAPTURE(gw); CAPTURE(gh); CAPTURE(fw); CAPTURE(fh);
        REQUIRE(gw == fw);
        REQUIRE(gh == fh);
        REQUIRE(goldenPx.size() == freshPx.size());

        const PixelDiff diff = comparePixels(goldenPx, freshPx, fw, fh);
        if (diff.differingPixels != 0) {
            MESSAGE("snapshot diverges from golden: "
                    << diff.differingPixels << " pixels differ; first at "
                    << "(x=" << diff.firstX << ", y=" << diff.firstY << "); "
                    << "fresh render kept at overlay_snapshot.png");
        }
        CHECK(diff.differingPixels == 0);
    }
}
