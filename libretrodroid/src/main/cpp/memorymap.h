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
 * Stage-1 scope: each descriptor maps its primary range [start, start + len) linearly onto
 * ptr + offset. RetroArch's select/disconnect mirror preprocessing is tracked as deferral D-MEM-1.
 * Not thread-safe on its own: callers serialize access with the core lock.
 */
class MemoryMap {
public:
    struct Region {
        uint64_t flags;
        uint8_t* ptr;
        size_t offset;
        size_t start;
        size_t len;
    };

    void set(const retro_memory_map* map);
    void clear();

    bool hasDescriptors() const { return !regions.empty(); }
    const std::vector<Region>& getRegions() const { return regions; }

    /**
     * Copies up to length bytes starting at the bus address into dest. Reads stop at the first
     * unmapped byte. Returns the number of bytes copied (0 when the start address is unmapped).
     */
    size_t read(size_t address, uint8_t* dest, size_t length) const;

private:
    const Region* find(size_t address) const;

    std::vector<Region> regions;
};

}

#endif
