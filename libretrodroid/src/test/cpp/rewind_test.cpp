// Host test: rewind round-trips every captured state exactly and honours the byte budget.
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "rewind.h"

using libretrodroid::Rewind;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void mutate(std::vector<uint8_t>& s, std::mt19937& rng, int edits) {
    for (int e = 0; e < edits; e++) {
        size_t at = rng() % s.size();
        size_t len = 1 + rng() % 40;
        for (size_t k = at; k < s.size() && k < at + len; k++) s[k] = (uint8_t) rng();
    }
}

static void roundTrip(size_t size, unsigned interval, int frames, size_t budget) {
    std::mt19937 rng(1234 + (unsigned) size);
    std::vector<uint8_t> live(size);
    for (auto& b : live) b = (uint8_t) rng();
    std::vector<std::vector<uint8_t>> captured;
    Rewind r;
    r.configure(budget, interval);
    unsigned since = 0;
    for (int f = 0; f < frames; f++) {
        mutate(live, rng, f % 7 == 0 ? 200 : 3);
        r.onFrame(size, [&](uint8_t* d, size_t n) { std::memcpy(d, live.data(), n); return true; });
        if (++since >= interval) { since = 0; captured.push_back(live); }
    }
    // Newest capture is the current state; each step back yields the one before it.
    size_t steps = 0;
    std::vector<uint8_t> loaded;
    while (r.stepBack([&](const uint8_t* d, size_t n) { loaded.assign(d, d + n); return true; })) {
        steps++;
        size_t expectIndex = captured.size() - 1 - steps;
        CHECK(loaded == captured[expectIndex]);
        if (loaded != captured[expectIndex]) break;
    }
    if (budget >= (size_t) 1 << 30) CHECK(steps == captured.size() - 1);
    CHECK(steps > 0);
    CHECK(r.getUsedBytes() == 0);
    std::printf("size=%zu interval=%u frames=%d budget=%zu captures=%zu stepped=%zu\n",
                size, interval, frames, budget, captured.size(), steps);
}

int main() {
    roundTrip(4096, 1, 300, (size_t) 1 << 30);
    roundTrip(4093, 1, 300, (size_t) 1 << 30);     // not a multiple of 8: padding
    roundTrip(1 << 20, 4, 400, (size_t) 1 << 30);
    roundTrip(1 << 20, 1, 400, 200 * 1024);         // budget evicts the oldest deltas
    roundTrip(17, 2, 100, (size_t) 1 << 30);

    // Size change restarts history.
    Rewind r;
    r.configure(1 << 20, 1);
    std::vector<uint8_t> a(64, 1), b(128, 2);
    for (int i = 0; i < 5; i++) r.onFrame(a.size(), [&](uint8_t* d, size_t n) { std::memcpy(d, a.data(), n); return true; });
    CHECK(r.getEntryCount() == 4);
    r.onFrame(b.size(), [&](uint8_t* d, size_t n) { std::memcpy(d, b.data(), n); return true; });
    CHECK(r.getEntryCount() == 0);

    // Disabled ring never captures.
    Rewind off;
    off.configure(0, 1);
    off.onFrame(a.size(), [&](uint8_t* d, size_t n) { std::memcpy(d, a.data(), n); return true; });
    CHECK(!off.stepBack([](const uint8_t*, size_t) { return true; }));

    std::printf(failures == 0 ? "PASS\n" : "FAILURES %d\n", failures);
    return failures == 0 ? 0 : 1;
}
