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


#ifndef LIBRETRODROID_REWIND_H
#define LIBRETRODROID_REWIND_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace libretrodroid {

/**
 * Byte-budgeted rewind buffer. The newest state is kept whole; each older state is stored as the
 * sparse XOR delta (changed 64-bit word runs only, zlib level 1) to the state captured after it,
 * so stepping back costs one small decompression and an XOR of the changed words. The oldest
 * deltas are dropped when the budget is exceeded.
 *
 * Not thread-safe: used only from the emulation (GL) thread under the core lock.
 */
class Rewind {
public:
    using Serialize = std::function<bool(uint8_t* data, size_t size)>;
    using Unserialize = std::function<bool(const uint8_t* data, size_t size)>;

    /** budgetBytes 0 disables rewind and frees all memory. interval is in emulated frames (>= 1). */
    void configure(size_t budgetBytes, unsigned interval);
    bool isEnabled() const { return budget > 0; }
    void clear();

    /** Call once per emulated frame; captures every interval frames. stateSize 0 skips. */
    void onFrame(size_t stateSize, const Serialize& serialize);

    /** Loads the previous captured state. Returns false when nothing is left to rewind. */
    bool stepBack(const Unserialize& unserialize);

    size_t getEntryCount() const { return deltas.size(); }
    size_t getUsedBytes() const { return used; }
    uint64_t getLastCaptureMicros() const { return lastCaptureMicros; }

private:
    size_t budget = 0;
    unsigned interval = 1;
    unsigned framesSinceCapture = 0;

    struct Delta {
        size_t rawBytes = 0;
        std::vector<uint8_t> packed;
    };

    bool capture(std::vector<uint64_t>& target, const Serialize& serialize);

    size_t stateSize = 0;
    std::vector<uint64_t> current;
    std::vector<uint64_t> scratch;
    std::vector<uint64_t> sparse;
    std::vector<uint8_t> packed;
    std::deque<Delta> deltas;
    size_t used = 0;
    uint64_t lastCaptureMicros = 0;
};

}

#endif
