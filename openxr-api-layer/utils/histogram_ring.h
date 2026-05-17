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
// histogram_ring.h — fixed-capacity FIFO ring of recent samples, used by the
// overlay renderer to draw the mini histograms under frametime / GPU time.
//
// Pure value type, no allocations after construction, no threading. The
// renderer holds two of these (frame_total and gpu_time) and pushes one
// sample per xrEndFrame; on each paint it walks the ring oldest→newest to
// emit the bars. Capacity defaults to 50 samples (≈ 0.5 s at 90-120 Hz)
// which matches what fpsvr-style HUDs display.
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>

namespace openxr_api_layer::detail {

    template <std::size_t Capacity>
    class HistogramRing {
      public:
        static_assert(Capacity > 0, "HistogramRing capacity must be positive");

        // Push the newest sample. Old samples drop off the back once the
        // ring fills. Cheap (O(1), no allocation).
        void push(int64_t value) noexcept {
            m_data[m_writeIdx] = value;
            m_writeIdx = (m_writeIdx + 1) % Capacity;
            if (m_count < Capacity) ++m_count;
        }

        // Number of valid samples currently stored (0..Capacity).
        std::size_t size() const noexcept { return m_count; }
        bool        empty() const noexcept { return m_count == 0; }
        static constexpr std::size_t capacity() noexcept { return Capacity; }

        // Read samples in chronological order (oldest first). `i` must be
        // < size(). The renderer iterates 0..size()-1 to draw bars left
        // (oldest) → right (newest), matching fpsvr's convention where
        // the newest sample is on the right edge.
        int64_t at(std::size_t i) const noexcept {
            // Walk back from the write index by (size - i) entries. When
            // the ring isn't full yet, samples live at 0..count-1 and
            // m_writeIdx == count, so this collapses to data[i].
            const std::size_t origin = (m_writeIdx + Capacity - m_count) % Capacity;
            return m_data[(origin + i) % Capacity];
        }

        // The maximum sample currently in the ring. Used to normalise bar
        // heights when the renderer doesn't know an a-priori upper bound
        // (e.g. spiky GPU time on a CPU-bound frame). Returns 0 if empty.
        int64_t maxValue() const noexcept {
            if (m_count == 0) return 0;
            int64_t maxv = at(0);
            for (std::size_t i = 1; i < m_count; ++i) {
                const int64_t v = at(i);
                if (v > maxv) maxv = v;
            }
            return maxv;
        }

        // Drop all stored samples back to the empty state. Used by the
        // renderer when the session is destroyed and a fresh one is
        // about to start (OpenComposite probe-then-real pattern).
        void clear() noexcept {
            m_writeIdx = 0;
            m_count = 0;
        }

      private:
        std::array<int64_t, Capacity> m_data{};
        std::size_t m_writeIdx = 0;
        std::size_t m_count = 0;
    };

} // namespace openxr_api_layer::detail
