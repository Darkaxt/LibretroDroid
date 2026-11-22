/*
 *     Copyright (C) 2026  Darkaxt
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#include "rewind.h"

#include <chrono>
#include <zlib.h>

#include "log.h"

namespace libretrodroid {

namespace {

uint64_t nowMicros() {
    return (uint64_t) std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void xorInto(uint8_t* target, const uint8_t* other, size_t size) {
    size_t words = size / sizeof(uint64_t);
    auto* t = reinterpret_cast<uint64_t*>(target);
    auto* o = reinterpret_cast<const uint64_t*>(other);
    for (size_t i = 0; i < words; i++) t[i] ^= o[i];
    for (size_t i = words * sizeof(uint64_t); i < size; i++) target[i] ^= other[i];
}

}

void Rewind::configure(size_t budgetBytes, unsigned newInterval) {
    budget = budgetBytes;
    interval = newInterval == 0 ? 1 : newInterval;
    clear();
    if (budget == 0) {
        current.clear();
        current.shrink_to_fit();
        scratch.clear();
        scratch.shrink_to_fit();
        packed.clear();
        packed.shrink_to_fit();
    }
}

void Rewind::clear() {
    deltas.clear();
    used = 0;
    framesSinceCapture = 0;
    current.clear();
}

void Rewind::onFrame(size_t stateSize, const Serialize& serialize) {
    if (budget == 0 || stateSize == 0) return;
    if (++framesSinceCapture < interval) return;
    framesSinceCapture = 0;

    uint64_t started = nowMicros();
    if (current.size() != stateSize) {
        // First capture, or the core changed its state size: start over.
        clear();
        current.resize(stateSize);
        if (!serialize(current.data(), stateSize)) current.clear();
        lastCaptureMicros = nowMicros() - started;
        return;
    }

    scratch.resize(stateSize);
    if (!serialize(scratch.data(), stateSize)) return;

    // Delta that turns the new state back into the previous one.
    xorInto(current.data(), scratch.data(), stateSize);
    uLongf packedSize = compressBound((uLong) stateSize);
    packed.resize(packedSize);
    if (compress2(packed.data(), &packedSize, current.data(), (uLong) stateSize, 1) != Z_OK) {
        LOGE("Rewind: compression failed, clearing history");
        clear();
        return;
    }
    deltas.emplace_back(packed.begin(), packed.begin() + (long) packedSize);
    used += packedSize;
    current.swap(scratch);

    while (used > budget && !deltas.empty()) {
        used -= deltas.front().size();
        deltas.pop_front();
    }
    lastCaptureMicros = nowMicros() - started;
}

bool Rewind::stepBack(const Unserialize& unserialize) {
    if (budget == 0 || deltas.empty() || current.empty()) return false;

    const std::vector<uint8_t>& delta = deltas.back();
    scratch.resize(current.size());
    uLongf size = (uLongf) scratch.size();
    if (uncompress(scratch.data(), &size, delta.data(), (uLong) delta.size()) != Z_OK || size != scratch.size()) {
        LOGE("Rewind: corrupt delta, clearing history");
        clear();
        return false;
    }
    xorInto(scratch.data(), current.data(), scratch.size());
    used -= delta.size();
    deltas.pop_back();
    current.swap(scratch);
    framesSinceCapture = 0;
    return unserialize(current.data(), current.size());
}

}
