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
#include <cstring>
#include <zlib.h>

#include "log.h"

namespace libretrodroid {

namespace {

uint64_t nowMicros() {
    return (uint64_t) std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

size_t wordsFor(size_t bytes) {
    return (bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t);
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
        sparse.clear();
        sparse.shrink_to_fit();
        packed.clear();
        packed.shrink_to_fit();
    }
}

void Rewind::clear() {
    deltas.clear();
    used = 0;
    framesSinceCapture = 0;
    current.clear();
    stateSize = 0;
}

bool Rewind::capture(std::vector<uint64_t>& target, const Serialize& serialize) {
    target.resize(wordsFor(stateSize));
    target.back() = 0; // padding bytes past stateSize stay zero
    return serialize(reinterpret_cast<uint8_t*>(target.data()), stateSize);
}

void Rewind::onFrame(size_t size, const Serialize& serialize) {
    if (budget == 0 || size == 0) return;
    if (++framesSinceCapture < interval) return;
    framesSinceCapture = 0;

    uint64_t started = nowMicros();
    if (current.empty() || size != stateSize) {
        // First capture, or the core changed its state size: start over.
        clear();
        stateSize = size;
        if (!capture(current, serialize)) clear();
        lastCaptureMicros = nowMicros() - started;
        return;
    }

    if (!capture(scratch, serialize)) return;

    // Sparse XOR delta that turns the new state back into the previous one. Most of a state does
    // not change between captures, so only changed word runs are kept: a header word
    // (unchanged words << 32 | changed words) followed by the XOR of the changed words.
    sparse.clear();
    const size_t n = current.size();
    const uint64_t* oldWords = current.data();
    const uint64_t* newWords = scratch.data();
    size_t i = 0;
    while (i < n) {
        size_t changed = i;
        while (changed < n && oldWords[changed] == newWords[changed]) changed++;
        size_t end = changed;
        while (end < n && oldWords[end] != newWords[end]) end++;
        sparse.push_back(((uint64_t) (changed - i) << 32) | (uint64_t) (end - changed));
        for (size_t k = changed; k < end; k++) sparse.push_back(oldWords[k] ^ newWords[k]);
        i = end;
    }

    auto rawBytes = (uLong) (sparse.size() * sizeof(uint64_t));
    uLongf packedSize = compressBound(rawBytes);
    packed.resize(packedSize);
    if (compress2(packed.data(), &packedSize, reinterpret_cast<const Bytef*>(sparse.data()), rawBytes, 1) != Z_OK) {
        LOGE("Rewind: compression failed, clearing history");
        clear();
        return;
    }
    Delta delta;
    delta.rawBytes = rawBytes;
    delta.packed.assign(packed.begin(), packed.begin() + (long) packedSize);
    used += packedSize;
    deltas.push_back(std::move(delta));
    current.swap(scratch);

    while (used > budget && !deltas.empty()) {
        used -= deltas.front().packed.size();
        deltas.pop_front();
    }
    lastCaptureMicros = nowMicros() - started;
}

bool Rewind::stepBack(const Unserialize& unserialize) {
    if (budget == 0 || deltas.empty() || current.empty()) return false;

    const Delta& delta = deltas.back();
    sparse.resize(wordsFor(delta.rawBytes));
    uLongf size = (uLongf) delta.rawBytes;
    if (uncompress(reinterpret_cast<Bytef*>(sparse.data()), &size, delta.packed.data(), (uLong) delta.packed.size()) != Z_OK
        || size != delta.rawBytes) {
        LOGE("Rewind: corrupt delta, clearing history");
        clear();
        return false;
    }

    const size_t n = current.size();
    size_t position = 0;
    size_t cursor = 0;
    const size_t count = delta.rawBytes / sizeof(uint64_t);
    while (cursor < count) {
        uint64_t header = sparse[cursor++];
        position += (size_t) (header >> 32);
        auto changed = (size_t) (header & 0xFFFFFFFFu);
        if (position + changed > n || cursor + changed > count) {
            LOGE("Rewind: malformed delta, clearing history");
            clear();
            return false;
        }
        for (size_t k = 0; k < changed; k++) current[position + k] ^= sparse[cursor + k];
        position += changed;
        cursor += changed;
    }

    used -= delta.packed.size();
    deltas.pop_back();
    framesSinceCapture = 0;
    return unserialize(reinterpret_cast<const uint8_t*>(current.data()), stateSize);
}

}
