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

#include "memorymap.h"

#include <algorithm>
#include <cstring>

namespace libretrodroid {

void MemoryMap::set(const retro_memory_map* map) {
    regions.clear();
    if (map == nullptr || map->descriptors == nullptr) return;

    for (unsigned i = 0; i < map->num_descriptors; i++) {
        const retro_memory_descriptor& d = map->descriptors[i];
        if (d.ptr == nullptr || d.len == 0) continue;
        regions.push_back(Region {
            d.flags,
            static_cast<uint8_t*>(d.ptr),
            d.offset,
            d.start,
            d.len
        });
    }
}

void MemoryMap::clear() {
    regions.clear();
}

const MemoryMap::Region* MemoryMap::find(size_t address) const {
    for (const Region& region : regions) {
        if (address >= region.start && address - region.start < region.len) {
            return &region;
        }
    }
    return nullptr;
}

size_t MemoryMap::read(size_t address, uint8_t* dest, size_t length) const {
    size_t copied = 0;
    while (copied < length) {
        const Region* region = find(address + copied);
        if (region == nullptr) break;

        size_t within = address + copied - region->start;
        size_t chunk = std::min(length - copied, region->len - within);
        std::memcpy(dest + copied, region->ptr + region->offset + within, chunk);
        copied += chunk;
    }
    return copied;
}

}
