// Checks tablet_scale.cpp finds the tablet canvas size (0.3 x 0.3 m at 150 px/m) on
// the heap, resizes it and reports the applied scale.
#include "../native/runtime/tablet_scale.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    const float stock[3] = {0.3f, 0.3f, 150.f};
    std::vector<float> heap(1 << 20, 7.f);
    float* size = heap.data() + 4099;
    memcpy(size, stock, sizeof(stock));

    tablet::start(L"C:\\nonexistent");  // no ini: size 1.0
    tablet::tabletPresent(true);
    tablet::request(2.f);
    for (int i = 0; i < 100; i++) {
        if (size[0] != 0.3f) {
            Sleep(200);
            bool ok = size[0] == 0.6f && size[1] == 0.6f && size[2] == 150.f && tablet::appliedScale() == 2.f;
            printf("tablet_scan_test: %s (%.2f x %.2f m, applied %.2f, after %d00 ms)\n", ok ? "all checks passed" : "FAILED",
                   size[0], size[1], tablet::appliedScale(), i);
            return ok ? 0 : 1;
        }
        Sleep(100);
    }
    printf("tablet_scan_test: FAILED (canvas size not changed within 10 s)\n");
    return 1;
}
