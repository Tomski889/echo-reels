// Protocol checks for native/shared/arcade_ipc.h (no game needed).
#include "../native/shared/arcade_ipc.h"
#include <atomic>
#include <cstdio>
#include <thread>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

int main() {
    static_assert(offsetof(arcade::Shared, frames) % 4096 == 0, "frames must be page aligned");
    // tools/fake_game.py mirrors these offsets.
    static_assert(offsetof(arcade::Shared, hostPid) == 32 && offsetof(arcade::Shared, latestFrame) == 36);
    static_assert(offsetof(arcade::Shared, frameSerial) == 40 && offsetof(arcade::Shared, hostHeartbeat) == 48);
    static_assert(offsetof(arcade::Shared, gamePid) == 152 && offsetof(arcade::Shared, pageVisible) == 156);
    static_assert(offsetof(arcade::Shared, readingFrame) == 160 && offsetof(arcade::Shared, gameHeartbeat) == 168);
    static_assert(offsetof(arcade::Shared, tabletScale) == 44 && offsetof(arcade::Shared, pageMode) == 164);
    static_assert(offsetof(arcade::Shared, touchWrite) == 176 && offsetof(arcade::Shared, touches) == 192);
    static_assert(offsetof(arcade::Shared, frames) == 8192 && sizeof(arcade::Shared) == 8192 + 3 * 2351104);
    HANDLE mapping = nullptr;
    auto s = arcade::open(&mapping);
    CHECK(s != nullptr);
    if (!s) return 1;
    CHECK(s->magic == arcade::MAGIC && s->width == arcade::WIDTH && s->height == arcade::HEIGHT);

    // Host never writes the newest frame or the one being read.
    for (int latest = 0; latest < 3; latest++)
        for (int reading = -1; reading < 3; reading++) {
            s->latestFrame = latest; s->readingFrame = reading;
            uint32_t w = arcade::writableFrame(s);
            CHECK(LONG(w) != latest && LONG(w) != reading);
        }

    // Concurrent writer/reader: every frame the reader copies is internally consistent.
    s->latestFrame = LONG(arcade::NO_FRAME); s->readingFrame = LONG(arcade::NO_FRAME);
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (uint32_t n = 1; !stop; n++) {
            uint32_t i = arcade::writableFrame(s);
            auto px = reinterpret_cast<uint32_t*>(s->frames[i]);
            for (uint32_t k = 0; k < arcade::FRAME_BYTES / 4; k += 97) px[k] = n;
            px[arcade::FRAME_BYTES / 4 - 1] = n;
            arcade::publishFrame(s, i);
            Sleep(1);  // real host: one frame per 16 ms
        }
    });
    int torn = 0, copies = 0;
    for (uint64_t end = GetTickCount64() + 1500; GetTickCount64() < end;) {
        LONG index = s->latestFrame;
        if (index == LONG(arcade::NO_FRAME)) continue;
        InterlockedExchange(&s->readingFrame, index);
        if (s->latestFrame != index) { InterlockedExchange(&s->readingFrame, LONG(arcade::NO_FRAME)); continue; }
        auto px = reinterpret_cast<uint32_t*>(s->frames[index]);
        uint32_t first = px[0];
        for (uint32_t k = 0; k < arcade::FRAME_BYTES / 4; k += 97) torn += px[k] != first;
        torn += px[arcade::FRAME_BYTES / 4 - 1] != first;
        copies++;
        InterlockedExchange(&s->readingFrame, LONG(arcade::NO_FRAME));
    }
    stop = true; writer.join();
    CHECK(copies > 100);
    CHECK(torn == 0);

    // Touch ring round trip.
    LONG start = s->touchWrite;
    arcade::pushTouch(s, 5, arcade::TouchDown);
    arcade::pushTouch(s, 5, arcade::TouchUp);
    auto& a = s->touches[uint32_t(start) % arcade::TOUCH_RING];
    auto& b = s->touches[uint32_t(start + 1) % arcade::TOUCH_RING];
    CHECK(a.seq == uint32_t(start) + 1 && a.cell == 5 && a.kind == arcade::TouchDown);
    CHECK(b.seq == uint32_t(start) + 2 && b.kind == arcade::TouchUp);
    std::printf(failures ? "ipc_test: %d failure(s)\n" : "ipc_test: all checks passed (%d consistent frame copies)\n", failures ? failures : copies);
    return failures ? 1 : 0;
}
