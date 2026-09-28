// Checks tablet_scale.cpp finds a stock tablet transform on the heap and resizes it.
#include "../native/runtime/tablet_scale.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    const uint32_t stock[10] = {0, 0, 0, 0x3f800000, 0xbe199999, 0x3f1ccccd, 0xbeff7cee, 0x3f800000, 0x3f800000, 0x3f800000};
    std::vector<uint32_t> heap(1 << 20, 0x12345678);
    uint32_t* t = heap.data() + 4099;  // like a CTransformCR row, 4-byte aligned
    memcpy(t, stock, sizeof(stock));

    tablet::start(L"C:\\nonexistent");  // no ini: size 1.0
    tablet::tabletPresent(true);
    tablet::request(2.f);
    for (int i = 0; i < 100; i++) {
        float s, x;
        memcpy(&s, t + 7, 4);
        memcpy(&x, t + 4, 4);
        if (s == 2.f) {
            bool ok = x == -0.3f && !memcmp(t, stock, 16) && t[8] == t[7] && t[9] == t[7];
            printf("tablet_scan_test: %s (scale %.2f, x %.3f, after %d00 ms)\n", ok ? "all checks passed" : "FAILED", s, x, i);
            return ok ? 0 : 1;
        }
        Sleep(100);
    }
    printf("tablet_scan_test: FAILED (transform not resized within 10 s)\n");
    return 1;
}
