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

// Host unit test for MemoryMap: compares it against a verbatim oracle of RetroArch's
// descriptor preprocessing (runloop.c) and lookup (command.c), RetroArch master
// a9850c6f6678b89221ba9b7907ecc59a7bb5bbf5 (GPL-3.0-or-later), on core-shaped maps.
//
// Build and run (host g++):
//   g++ -std=c++17 -O2 -Wall -I../../main/cpp memorymap_test.cpp ../../main/cpp/memorymap.cpp -o memorymap_test
//   ./memorymap_test

#include "memorymap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using libretrodroid::MemoryMap;

// ---- RetroArch oracle (verbatim logic, renamed) -----------------------------------------------

namespace ra {

struct rarch_memory_descriptor_t { retro_memory_descriptor core; };

static size_t mmap_add_bits_down(size_t n) {
    n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16;
#if SIZE_MAX > 0xFFFFFFFFUL
    n |= n >> 32;
#endif
    return n;
}

static size_t mmap_inflate(size_t addr, size_t mask) {
    while (mask) {
        size_t tmp = (mask - 1) & ~mask;
        addr = ((addr & ~tmp) << 1) | (addr & tmp);
        mask = mask & (mask - 1);
    }
    return addr;
}

static size_t mmap_reduce(size_t addr, size_t mask) {
    while (mask) {
        size_t tmp = (mask - 1) & ~mask;
        addr = (addr & tmp) | ((addr >> 1) & ~tmp);
        mask = (mask & (mask - 1)) >> 1;
    }
    return addr;
}

static size_t mmap_highest_bit(size_t n) {
    n = mmap_add_bits_down(n);
    return n ^ (n >> 1);
}

static bool mmap_preprocess_descriptors(rarch_memory_descriptor_t *first, unsigned count) {
    size_t top_addr = 1;
    rarch_memory_descriptor_t *desc = NULL;
    const rarch_memory_descriptor_t *end = first + count;
    size_t highest_reachable = 0;

    for (desc = first; desc < end; desc++) {
        if (desc->core.select != 0)
            top_addr |= desc->core.select;
        else
            top_addr |= desc->core.start + desc->core.len - 1;
    }

    top_addr = mmap_add_bits_down(top_addr);

    for (desc = first; desc < end; desc++) {
        if (desc->core.select == 0) {
            if (desc->core.len == 0)
                return false;
            if ((desc->core.len & (desc->core.len - 1)) != 0)
                return false;
            desc->core.select = top_addr
                & ~mmap_inflate(mmap_add_bits_down(desc->core.len - 1), desc->core.disconnect);
        }

        if (desc->core.len == 0)
            desc->core.len = mmap_add_bits_down(
                mmap_reduce(top_addr & ~desc->core.select, desc->core.disconnect)) + 1;

        if (desc->core.start & ~desc->core.select)
            return false;

        highest_reachable = mmap_inflate(desc->core.len - 1, desc->core.disconnect);

        while (mmap_highest_bit(top_addr & ~desc->core.select & ~desc->core.disconnect) >
               mmap_highest_bit(highest_reachable))
            desc->core.disconnect |= mmap_highest_bit(top_addr
                & ~desc->core.select & ~desc->core.disconnect);
    }

    return true;
}

static const rarch_memory_descriptor_t* command_memory_get_descriptor(
        const rarch_memory_descriptor_t* first, unsigned count, unsigned address, size_t* offset) {
    const rarch_memory_descriptor_t* desc = first;
    const rarch_memory_descriptor_t* end = desc + count;

    for (; desc < end; desc++) {
        if (desc->core.select == 0) {
            if (address >= desc->core.start && address < desc->core.start + desc->core.len) {
                *offset = address - desc->core.start;
                return desc;
            }
        } else {
            if (((desc->core.start ^ address) & desc->core.select) == 0) {
                unsigned desc_offset = address - (unsigned)desc->core.start;
                unsigned mask = (unsigned)desc->core.disconnect;
                while (mask) {
                    const unsigned tmp = (mask - 1) & ~mask;
                    desc_offset = (desc_offset & tmp) | ((desc_offset >> 1) & ~tmp);
                    mask = (mask & (mask - 1)) >> 1;
                }
                *offset = desc_offset;
                if (desc_offset < desc->core.len)
                    return desc;
            }
        }
    }
    return NULL;
}

}

// ---- Harness -----------------------------------------------------------------------------------

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

struct Case {
    std::string name;
    std::vector<retro_memory_descriptor> descriptors;
    std::vector<std::vector<uint8_t>> buffers; // keep descriptor storage alive
};

static uint8_t* fill(Case& c, size_t size, uint8_t seed) {
    c.buffers.emplace_back(size);
    std::vector<uint8_t>& b = c.buffers.back();
    for (size_t i = 0; i < size; i++) b[i] = (uint8_t) (seed + i * 7 + (i >> 8));
    return b.data();
}

static retro_memory_descriptor desc(uint64_t flags, void* ptr, size_t offset, size_t start,
                                    size_t select, size_t disconnect, size_t len) {
    retro_memory_descriptor d{};
    d.flags = flags; d.ptr = ptr; d.offset = offset; d.start = start;
    d.select = select; d.disconnect = disconnect; d.len = len; d.addrspace = nullptr;
    return d;
}

static void compareCase(Case& c, const std::vector<uint32_t>& addresses) {
    // Oracle.
    std::vector<ra::rarch_memory_descriptor_t> oracle(c.descriptors.size());
    for (size_t i = 0; i < c.descriptors.size(); i++) oracle[i].core = c.descriptors[i];
    bool oracleOk = ra::mmap_preprocess_descriptors(oracle.data(), (unsigned) oracle.size());

    // Implementation under test.
    retro_memory_map map{ c.descriptors.data(), (unsigned) c.descriptors.size() };
    MemoryMap mm;
    mm.set(&map);
    const auto& regions = mm.getRegions();

    // Test-data sanity: every descriptor with data must fit its buffer after preprocessing,
    // otherwise the comparison would read out of bounds.
    for (const auto& r : regions) {
        if (r.ptr == nullptr) continue;
        bool fits = false;
        for (const auto& b : c.buffers) {
            if (r.ptr >= b.data() && r.ptr < b.data() + b.size()) {
                fits = (size_t) (r.ptr - b.data()) + r.offset + r.len <= b.size();
            }
        }
        if (!fits) {
            std::printf("BAD TEST DATA %s: descriptor at 0x%zx len 0x%zx exceeds its buffer\n", c.name.c_str(), r.start, r.len);
            std::exit(2);
        }
    }

    CHECK(mm.isFullyPreprocessed() == oracleOk, "%s: preprocess result %d vs RA %d",
          c.name.c_str(), mm.isFullyPreprocessed(), oracleOk);
    CHECK(regions.size() == oracle.size(), "%s: %zu regions vs RA %zu", c.name.c_str(),
          regions.size(), oracle.size());
    for (size_t i = 0; i < regions.size() && i < oracle.size(); i++) {
        CHECK(regions[i].select == oracle[i].core.select && regions[i].disconnect == oracle[i].core.disconnect
              && regions[i].len == oracle[i].core.len,
              "%s: descriptor %zu select/disconnect/len %zx/%zx/%zx vs RA %zx/%zx/%zx", c.name.c_str(), i,
              regions[i].select, regions[i].disconnect, regions[i].len,
              oracle[i].core.select, oracle[i].core.disconnect, oracle[i].core.len);
    }

    int mismatches = 0;
    for (uint32_t address : addresses) {
        size_t raOffset = 0, ourOffset = 0;
        const auto* r = ra::command_memory_get_descriptor(oracle.data(), (unsigned) oracle.size(), address, &raOffset);
        const auto* o = MemoryMap::find(regions, address, &ourOffset);
        long raIndex = r ? (long) (r - oracle.data()) : -1;
        long ourIndex = o ? (long) (o - regions.data()) : -1;
        bool same = raIndex == ourIndex && (raIndex < 0 || raOffset == ourOffset);
        if (!same && mismatches++ < 5) {
            CHECK(false, "%s: address 0x%08X -> desc %ld off 0x%zx vs RA desc %ld off 0x%zx", c.name.c_str(),
                  address, ourIndex, ourOffset, raIndex, raOffset);
        }

        // READ_CORE_MEMORY first byte: RA returns ptr + offset + desc offset when ptr is set.
        uint8_t got = 0;
        size_t n = mm.read(address, &got, 1);
        if (r && r->core.ptr) {
            uint8_t expected = ((const uint8_t*) r->core.ptr)[r->core.offset + raOffset];
            if ((n != 1 || got != expected) && mismatches++ < 5) {
                CHECK(false, "%s: read 0x%08X got %zu/%02X expected %02X", c.name.c_str(), address, n, got, expected);
            }
        } else if (n != 0 && mismatches++ < 5) {
            CHECK(false, "%s: read 0x%08X returned %zu bytes for an address RA cannot serve", c.name.c_str(), address, n);
        }
    }
    checks++;
    if (mismatches) failures++;
    std::printf("%-28s preprocess=%s descriptors=%zu addresses=%zu mismatches=%d\n", c.name.c_str(),
                oracleOk ? "ok" : "stopped", regions.size(), addresses.size(), mismatches);
}

static std::vector<uint32_t> denseAddresses(uint32_t limit) {
    std::vector<uint32_t> a;
    a.reserve(limit);
    for (uint32_t i = 0; i < limit; i++) a.push_back(i);
    return a;
}

static std::vector<uint32_t> sampledAddresses(const std::vector<retro_memory_descriptor>& ds) {
    std::vector<uint32_t> a;
    for (const auto& d : ds) {
        size_t span = d.len ? d.len : 0x10000;
        for (long delta = -0x200; delta <= 0x200; delta++) {
            a.push_back((uint32_t) (d.start + delta));
            a.push_back((uint32_t) (d.start + span + delta));
        }
        for (size_t mirror = 1; mirror < 8; mirror++) a.push_back((uint32_t) (d.start + span * mirror + 3));
    }
    std::mt19937 rng(1234);
    for (int i = 0; i < 2000000; i++) a.push_back((uint32_t) rng());
    return a;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // 1. mGBA as published (GBA bus). VRAM 0x18000 is not a power of two with select 0, so RA's
    //    preprocessing stops there and later descriptors keep explicit-range matching.
    {
        Case c{"mgba-gba", {}, {}};
        c.descriptors = {
            desc(RETRO_MEMDESC_SYSTEM_RAM, fill(c, 0x8000, 1), 0, 0x03000000, 0, 0, 0x8000),
            desc(RETRO_MEMDESC_SYSTEM_RAM, fill(c, 0x40000, 2), 0, 0x02000000, 0, 0, 0x40000),
            desc(RETRO_MEMDESC_SAVE_RAM, fill(c, 0x20000, 3), 0, 0x0E000000, 0, 0, 0x20000),
            desc(RETRO_MEMDESC_CONST, fill(c, 0x1000000, 4), 0, 0x08000000, 0, 0, 0x1000000),
            desc(RETRO_MEMDESC_CONST, c.buffers.back().data(), 0, 0x0A000000, 0, 0, 0x1000000),
            desc(RETRO_MEMDESC_CONST, c.buffers.back().data(), 0, 0x0C000000, 0, 0, 0x1000000),
            desc(RETRO_MEMDESC_CONST, fill(c, 0x4000, 5), 0, 0x00000000, 0, 0, 0x4000),
            desc(RETRO_MEMDESC_VIDEO_RAM, fill(c, 0x18000, 6), 0, 0x06000000, 0, 0, 0x18000),
            desc(0, fill(c, 0x400, 7), 0, 0x05000000, 0, 0, 0x400),
            desc(0, fill(c, 0x400, 8), 0, 0x07000000, 0, 0, 0x400),
            desc(0, fill(c, 0x400, 9), 0, 0x04000000, 0, 0, 0x400),
        };
        compareCase(c, sampledAddresses(c.descriptors));

        // Linear behaviour the stage-1 DualDex checks relied on stays the same.
        retro_memory_map map{ c.descriptors.data(), (unsigned) c.descriptors.size() };
        MemoryMap mm; mm.set(&map);
        uint8_t buf[16];
        CHECK(mm.read(0x02000010, buf, 16) == 16 && std::memcmp(buf, c.buffers[1].data() + 0x10, 16) == 0,
              "mgba EWRAM linear read");
        CHECK(mm.read(0x0203FFF8, buf, 16) == 8, "mgba EWRAM read stops at the end of the mapped range");
        CHECK(mm.read(0x03007FF0, buf, 16) == 16, "mgba IWRAM tail read");
    }

    // 2. libretro.h's SNES example: WRAM at 7E-7F with a low-RAM mirror in banks 00-3F/80-BF,
    //    SRAM with a disconnect bit, LoROM with A15 disconnected, select-only mirrors.
    {
        Case c{"snes-lorom", {}, {}};
        uint8_t* wram = fill(c, 0x20000, 11);
        uint8_t* sram = fill(c, 0x20000, 12);
        uint8_t* rom = fill(c, 0x100000, 13);
        c.descriptors = {
            desc(RETRO_MEMDESC_SYSTEM_RAM, wram, 0, 0x7E0000, 0xFE0000, 0, 0x20000),
            desc(RETRO_MEMDESC_SYSTEM_RAM, wram, 0, 0x000000, 0x40E000, 0, 0x2000),
            desc(RETRO_MEMDESC_SAVE_RAM, sram, 0, 0x700000, 0xFE8000, 0, 0),
            desc(RETRO_MEMDESC_CONST, rom, 0, 0x008000, 0x408000, 0x8000, 0x100000),
            desc(RETRO_MEMDESC_CONST, rom, 0, 0x808000, 0xC08000, 0x8000, 0x100000),
        };
        compareCase(c, denseAddresses(0x1000000));

        // Hand-computed expectations, independent of the oracle.
        retro_memory_map map{ c.descriptors.data(), (unsigned) c.descriptors.size() };
        MemoryMap mm; mm.set(&map);
        uint8_t b = 0;
        CHECK(mm.read(0x001234, &b, 1) == 1 && b == wram[0x1234], "snes low-RAM mirror bank 00");
        CHECK(mm.read(0x801234, &b, 1) == 1 && b == wram[0x1234], "snes low-RAM mirror bank 80");
        CHECK(mm.read(0x3F1234, &b, 1) == 1 && b == wram[0x1234], "snes low-RAM mirror bank 3F");
        CHECK(mm.read(0x7F1234, &b, 1) == 1 && b == wram[0x11234], "snes WRAM bank 7F");
        CHECK(mm.read(0x018000, &b, 1) == 1 && b == rom[0x8000], "snes LoROM bank 01 with A15 disconnected");
    }

    // 3. Power-of-two descriptors with select 0 (derived select) and an explicit disconnect.
    {
        Case c{"derived-select", {}, {}};
        c.descriptors = {
            desc(RETRO_MEMDESC_SYSTEM_RAM, fill(c, 0x800, 21), 0, 0x0000, 0, 0x1800, 0x800),
            desc(0, fill(c, 0x2000, 22), 0, 0x8000, 0, 0, 0x2000),
            desc(0, fill(c, 0x4000, 23), 0x100, 0xC000, 0, 0, 0x2000),
            desc(0, nullptr, 0, 0x6000, 0, 0, 0x2000),
        };
        compareCase(c, denseAddresses(0x20000));
    }

    // 4. GB-shaped map (Gambatte style): echo RAM length 0x1E00 stops preprocessing.
    {
        Case c{"gb-echo", {}, {}};
        uint8_t* wram = fill(c, 0x2000, 31);
        c.descriptors = {
            desc(RETRO_MEMDESC_SYSTEM_RAM, fill(c, 0x7F, 32), 0, 0xFF80, 0, 0, 0x7F),
            desc(RETRO_MEMDESC_SYSTEM_RAM, wram, 0, 0xC000, 0, 0, 0x2000),
            desc(RETRO_MEMDESC_SYSTEM_RAM, wram, 0, 0xE000, 0, 0, 0x1E00),
            desc(RETRO_MEMDESC_VIDEO_RAM, fill(c, 0x2000, 33), 0, 0x8000, 0, 0, 0x2000),
            desc(RETRO_MEMDESC_CONST, fill(c, 0x8000, 34), 0, 0x0000, 0, 0, 0x8000),
        };
        compareCase(c, denseAddresses(0x20000));
    }

    // 5. Invalid start (start bits outside select): RA stops at that descriptor.
    {
        Case c{"invalid-start", {}, {}};
        c.descriptors = {
            desc(0, fill(c, 0x1000, 41), 0, 0x1000, 0xF000, 0, 0x1000),
            desc(0, fill(c, 0x1000, 42), 0, 0x2010, 0xF000, 0, 0x1000),
            desc(0, fill(c, 0x1000, 43), 0, 0x3000, 0, 0, 0x1000),
        };
        compareCase(c, denseAddresses(0x10000));
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
