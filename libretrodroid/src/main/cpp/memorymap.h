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

#ifndef LIBRETRODROID_MEMORYMAP_H
#define LIBRETRODROID_MEMORYMAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../libretro-common/include/libretro.h"

namespace libretrodroid {

/**
 * Bus-address view of core memory, built from RETRO_ENVIRONMENT_SET_MEMORY_MAPS.
 *
 * Semantics follow RetroArch so DualDex sees the same bytes as RetroArch's READ_CORE_MEMORY:
 * descriptors are preprocessed like runloop.c mmap_preprocess_descriptors (select/disconnect
 * derivation, including its early stop on a non-power-of-two length), and lookups follow command.c
 * command_memory_get_descriptor (first match in descriptor order, select matching, disconnect-bit
 * removal, 32-bit address arithmetic).
 * Not thread-safe on its own: callers serialize access with the core lock.
 */
class MemoryMap {
public:
    struct Region {
        uint64_t flags;
        uint8_t* ptr;
        size_t offset;
        size_t start;
        size_t select;
        size_t disconnect;
        size_t len;
    };

    void set(const retro_memory_map* map);
    void clear();

    bool hasDescriptors() const { return !regions.empty(); }
    const std::vector<Region>& getRegions() const { return regions; }

    /** False when RetroArch's preprocessing stopped early; later descriptors keep select 0. */
    bool isFullyPreprocessed() const { return preprocessed; }

    /**
     * Copies up to length bytes starting at the bus address into dest, continuing into the
     * descriptor that maps the next address like READ_CORE_MEMORY. Reads stop at the first
     * unmapped byte or a descriptor without data. Returns the number of bytes copied.
     */
    size_t read(uint32_t address, uint8_t* dest, size_t length) const;

    /** RetroArch's descriptor preprocessing, applied in place. Exposed for unit tests. */
    static bool preprocess(std::vector<Region>& regions);

    /** Returns the descriptor serving the address and the offset within it, or nullptr. */
    static const Region* find(const std::vector<Region>& regions, uint32_t address, size_t* offset);

private:
    std::vector<Region> regions;
    bool preprocessed = false;
};

}

#endif
