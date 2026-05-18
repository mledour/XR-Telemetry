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
// test_histogram_ring.cpp — fixed-capacity FIFO ring used by the overlay
// renderer's mini histograms. Pure value-type, easy to drive with
// synthetic samples.
// =============================================================================

#include <doctest/doctest.h>

#include "utils/histogram_ring.h"

using openxr_api_layer::detail::HistogramRing;

TEST_CASE("HistogramRing: empty after construction") {
    HistogramRing<8> r;
    CHECK(r.empty());
    CHECK(r.size() == 0);
    CHECK(r.maxValue() == 0);
}

TEST_CASE("HistogramRing: push fewer than capacity preserves order oldest→newest") {
    HistogramRing<8> r;
    r.push(10);
    r.push(20);
    r.push(30);
    REQUIRE(r.size() == 3);
    CHECK(r.at(0) == 10);
    CHECK(r.at(1) == 20);
    CHECK(r.at(2) == 30);
    CHECK(r.maxValue() == 30);
}

TEST_CASE("HistogramRing: full-capacity push, oldest still readable at index 0") {
    HistogramRing<4> r;
    r.push(1); r.push(2); r.push(3); r.push(4);
    REQUIRE(r.size() == 4);
    CHECK(r.at(0) == 1);
    CHECK(r.at(3) == 4);
    CHECK(r.maxValue() == 4);
}

TEST_CASE("HistogramRing: push beyond capacity drops the oldest, shifts the window") {
    HistogramRing<4> r;
    r.push(1); r.push(2); r.push(3); r.push(4);
    r.push(5);   // drops 1
    REQUIRE(r.size() == 4);
    CHECK(r.at(0) == 2);
    CHECK(r.at(3) == 5);
    r.push(6);   // drops 2
    CHECK(r.at(0) == 3);
    CHECK(r.at(3) == 6);
}

TEST_CASE("HistogramRing: maxValue tracks the highest sample currently stored") {
    HistogramRing<4> r;
    r.push(100); r.push(200); r.push(50); r.push(300);
    CHECK(r.maxValue() == 300);
    // Drop the 300 by pushing 4 more lower values.
    r.push(10); r.push(20); r.push(30); r.push(40);
    CHECK(r.maxValue() == 40);
}

TEST_CASE("HistogramRing: clear resets to empty") {
    HistogramRing<8> r;
    r.push(1); r.push(2); r.push(3);
    r.clear();
    CHECK(r.empty());
    CHECK(r.size() == 0);
    CHECK(r.maxValue() == 0);
    // Post-clear push behaves like a fresh ring.
    r.push(100);
    CHECK(r.size() == 1);
    CHECK(r.at(0) == 100);
}

TEST_CASE("HistogramRing: capacity == 1 degenerate case still works") {
    HistogramRing<1> r;
    r.push(5);
    CHECK(r.size() == 1);
    CHECK(r.at(0) == 5);
    r.push(10);   // drops 5
    CHECK(r.at(0) == 10);
}
