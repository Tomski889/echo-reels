#include "tablet_scale.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// The tablet canvas (CCanvasUICR row of actor 0x6c1f6ff04e070923, canvas
// 0x30b4d30bbcb8d444) is 0.3 x 0.3 m at 150 px/m. The engine copies that record
// into the canvas component; resizing = changing those world sizes. (Scaling the
// tablet actor's local transforms does nothing: its pose is set directly each frame.)
constexpr float STOCK_SIZE = 0.3f, PPM = 150.f;
constexpr uint64_t TABLET_CANVAS = 0x30b4d30bbcb8d444;

std::vector<uint32_t*> hits;  // -> width, height, pixels-per-metre
std::atomic<float> desired{1.f}, applied{1.f};
std::atomic<bool> present{false}, rescan{true};
std::atomic<int> failures{0};
float written = STOCK_SIZE;  // world width we last wrote (recognises our own edits on a rescan)

bool scannable(const MEMORY_BASIC_INFORMATION& m) {
    if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE) return false;
    if (m.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE | PAGE_NOCACHE)) return false;
    return (m.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && m.RegionSize < (size_t(1) << 31);
}

float f32(const uint32_t* p) { float f; memcpy(&f, p, 4); return f; }

bool isSize(const uint32_t* p) {
    if (p[0] != p[1] || f32(p + 2) != PPM) return false;
    float w = f32(p);
    return w == STOCK_SIZE || w == written;
}

template <class F> bool guarded(F f) {
    __try { return f(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void scanRegion(uint32_t* base, size_t words) {
    const uint32_t ppm = 0x43160000;  // 150.0f
    __try {
        for (size_t i = 2; i < words; i++) {
            if (base[i] != ppm) continue;
            uint32_t* p = base + i - 2;
            if (isSize(p)) hits.push_back(p);
            if (hits.size() >= 64) return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void scan() {
    hits.clear();
    ULONGLONG t0 = GetTickCount64();
    size_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (auto* a = reinterpret_cast<unsigned char*>(0x10000); VirtualQuery(a, &mbi, sizeof(mbi)); a = static_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize) {
        if (scannable(mbi)) { scanRegion(static_cast<uint32_t*>(mbi.BaseAddress), mbi.RegionSize / 4); scanned += mbi.RegionSize; }
        if (reinterpret_cast<uintptr_t>(a) >= 0x7ffffffe0000ull || GetTickCount64() - t0 > 60000) break;
    }
    logf("tablet: scanned %zu MB in %llu ms: %zu canvas size copies", scanned >> 20, GetTickCount64() - t0, hits.size());
    for (auto* p : hits) {
        uint64_t before[4] = {};
        guarded([&] { memcpy(before, p - 8, 32); return true; });
        bool record = before[2] == TABLET_CANVAS;  // the level record: canvas name right before the size
        logf("tablet:   %p %s  before: %016llx %016llx %016llx %016llx", static_cast<void*>(p),
             record ? "tablet canvas record" : "copy", before[0], before[1], before[2], before[3]);
    }
}

bool writeSize(uint32_t* p, float w) {
    return guarded([p, w] {
        if (!isSize(p)) return false;
        memcpy(p, &w, 4);
        memcpy(p + 1, &w, 4);
        return true;
    });
}

void worker() {
    for (;;) {
        Sleep(100);
        float want = desired;
        if (!present) continue;
        if (rescan) {
            if (want == 1.f && written == STOCK_SIZE) { rescan = false; continue; }  // nothing to do
            if (failures >= 3) { rescan = false; continue; }
            Sleep(1500);  // let the lobby finish creating the tablet
            scan();
            rescan = false;
            applied = -1.f;
        }
        if (want == applied) continue;
        float w = STOCK_SIZE * want;
        int ok = 0;
        for (auto* p : hits) ok += writeSize(p, w);
        if (ok) written = w;
        hits.erase(std::remove_if(hits.begin(), hits.end(), [](uint32_t* p) { return !guarded([p] { return isSize(p); }); }), hits.end());
        logf("tablet: size %.2f -> %d canvas copies", want, ok);
        applied = ok ? want : 1.f;
        if (ok) failures = 0;
        else if (++failures >= 3) logf("tablet: canvas size not found; resizing paused until the tablet reloads");
        else { rescan = true; Sleep(3000); }
    }
}

}  // namespace

namespace tablet {

void start(const std::wstring& pluginDir) {
    wchar_t buf[32];
    GetPrivateProfileStringW(L"tablet", L"scale", L"1.0", buf, 32, (pluginDir + L"\\EchoArcade\\echo_tweaks.ini").c_str());
    request(float(_wtof(buf)));
    logf("tablet: saved size %.2f", desired.load());
    std::thread(worker).detach();
}

void request(float scale) {
    if (!(scale > 0)) scale = 1.f;
    desired = std::clamp(std::round(scale * 100) / 100, 0.5f, 4.f);
}

float appliedScale() {
    float a = applied;
    return a > 0 ? a : 1.f;
}

void tabletPresent(bool p) {
    if (p && !present) { rescan = true; failures = 0; }
    if (!p) applied = 1.f;
    present = p;
}

}  // namespace tablet
