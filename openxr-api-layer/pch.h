// MIT License
//
// Copyright (c) 2026 <<AUTHOR_NAME>>
//
// Based on https://github.com/mbucchia/OpenXR-Layer-Template.
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
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

// pch.h pulls D3D11, D3D12 and D3D11On12 headers up front so any layer
// code below (utils/, layer.cpp) can use those types without further
// include gymnastics. None of these are *required* — a pure metadata-
// only layer that just rewrites FOVs / poses never touches D3D — but
// the vcxproj delay-loads d3d11.dll / d3dcompiler_47.dll so a layer
// that does NOT exercise the graphics path never causes those DLLs
// to be mapped into the host process. So you pay the cost only when
// your code actually calls into D3D.
//
// d3d11on12.h piggy-backs on d3d11.dll for the D3D11On12CreateDevice
// export, so D3D12-host support adds no extra DLL beyond what the
// native D3D11 path already pulls.

// Standard library.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <ctime>
#define _USE_MATH_DEFINES
#include <cmath>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>

using namespace std::chrono_literals;

// Windows header files.
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <wrl.h>
#include <wil/resource.h>
#include <traceloggingactivity.h>
#include <traceloggingprovider.h>

// D3D11 includes — types-only, no symbol references for any TU that
// does not actually use D3D. d3d11.dll / d3dcompiler_47.dll are
// delay-loaded at link time so they are not mapped into host
// processes that never exercise your graphics path.
//
// D3D12 + d3d11on12 are pulled in for layers that need to render
// auxiliary content on D3D12 hosts (MSFS, newer Forza, UE5 titles).
// You never call D3D12 entry points directly — you only consume the
// ID3D12Device / ID3D12CommandQueue the application hands you through
// XrGraphicsBindingD3D12KHR, then bridge them to ID3D11Device via
// D3D11On12CreateDevice (which is exported by d3d11.dll, already
// delay-loaded). So no extra DLL ends up loaded
// in non-D3D12 host processes.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3d12.h>
#include <d3d11on12.h>

using Microsoft::WRL::ComPtr;

// OpenXR + Windows-specific definitions. Both D3D11 and D3D12
// graphics-API switches are enabled so <openxr_platform.h> exposes
// XrGraphicsBindingD3D{11,12}KHR + XrSwapchainImageD3D{11,12}KHR. The
// switches only enable type definitions; nothing is linked.
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// OpenXR loader interfaces. Promoted to a public header in OpenXR 1.1;
// was <loader_interfaces.h> under src/common/ in 1.0.
#include <openxr/openxr_loader_negotiation.h>

// OpenXR/DirectX utilities.
#include <XrError.h>
#include <XrMath.h>
#include <XrSide.h>
#include <XrStereoView.h>
#include <XrToString.h>

// FMT formatter.
#include <fmt/format.h>
