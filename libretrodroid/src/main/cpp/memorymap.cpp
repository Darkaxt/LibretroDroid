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

namespace {

// Bit helpers ported from RetroArch runloop.c (mmap_add_bits_down, mmap_inflate, mmap_reduce,
// mmap_highest_bit).
size_t addBitsDown(size_t n) {
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
#if SIZE_MAX > 0xFFFFFFFFUL
    n |= n >> 32;
#endif
    return n;
}

size_t inflate(size_t addr, size_t mask) {
    while (mask) {
        size_t tmp = (mask - 1) & ~mask;
        addr = ((addr & ~tmp) << 1) | (addr & tmp);
        mask = mask & (mask - 1);
    }
    return addr;
}

size_t reduce(size_t addr, size_t mask) {
    while (mask) {
        size_t tmp = (mask - 1) & ~mask;
        addr = (addr & tmp) | ((addr >> 1) & ~tmp);
        mask = (mask & (mask - 1)) >> 1;
    }
    return addr;
}

size_t highestBit(size_t n) {
    n = addBitsDown(n);
    return n ^ (n >> 1);
}

}

bool MemoryMap::preprocess(std::vector<Region>& regions) {
    size_t topAddr = 1;
    for (const Region& d : regions) {
        if (d.select != 0) {
            topAddr |= d.select;
        } else {
            topAddr |= d.start + d.len - 1;
        }
    }
    topAddr = addBitsDown(topAddr);

    for (Region& d : regions) {
        if (d.select == 0) {
            if (d.len == 0) return false;
            if ((d.len & (d.len - 1)) != 0) return false;
            d.select = topAddr & ~inflate(addBitsDown(d.len - 1), d.disconnect);
        }

        if (d.len == 0) {
            d.len = addBitsDown(reduce(topAddr & ~d.select, d.disconnect)) + 1;
        }

        if (d.start & ~d.select) return false;

        size_t highestReachable = inflate(d.len - 1, d.disconnect);
        while (highestBit(topAddr & ~d.select & ~d.disconnect) > highestBit(highestReachable)) {
            d.disconnect |= highestBit(topAddr & ~d.select & ~d.disconnect);
        }
    }
    return true;
}

void MemoryMap::set(const retro_memory_map* map) {
    regions.clear();
    preprocessed = false;
    if (map == nullptr || map->descriptors == nullptr) return;

    // RetroArch keeps every descriptor, including ones without data: they still shadow later
    // descriptors during lookup.
    for (unsigned i = 0; i < map->num_descriptors; i++) {
        const retro_memory_descriptor& d = map->descriptors[i];
        regions.push_back(Region {
            d.flags,
            static_cast<uint8_t*>(d.ptr),
            d.offset,
            d.start,
            d.select,
            d.disconnect,
            d.len
        });
    }
    preprocessed = preprocess(regions);
}

void MemoryMap::clear() {
    regions.clear();
    preprocessed = false;
}

const MemoryMap::Region* MemoryMap::find(const std::vector<Region>& regions, uint32_t address, size_t* offset) {
    for (const Region& d : regions) {
        if (d.select == 0) {
            if (address >= d.start && address < d.start + d.len) {
                *offset = address - d.start;
                return &d;
            }
        } else if (((d.start ^ address) & d.select) == 0) {
            // 32-bit arithmetic, as in RetroArch's command_memory_get_descriptor.
            unsigned descOffset = address - (unsigned) d.start;
            unsigned mask = (unsigned) d.disconnect;
            while (mask) {
                const unsigned tmp = (mask - 1) & ~mask;
                descOffset = (descOffset & tmp) | ((descOffset >> 1) & ~tmp);
                mask = (mask & (mask - 1)) >> 1;
            }
            *offset = descOffset;
            if (descOffset < d.len) return &d;
        }
    }
    return nullptr;
}

size_t MemoryMap::read(uint32_t address, uint8_t* dest, size_t length) const {
    size_t copied = 0;
    while (copied < length) {
        size_t offset = 0;
        const Region* d = find(regions, (uint32_t) (address + copied), &offset);
        if (d == nullptr || d->ptr == nullptr) break;

        size_t chunk = std::min(length - copied, d->len - offset);
        if (chunk == 0) break;
        std::memcpy(dest + copied, d->ptr + d->offset + offset, chunk);
        copied += chunk;
    }
    return copied;
}

}
