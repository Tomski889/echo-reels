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

// Stock tablet transform (r14_glb_global_mp, CTransformCR rows of actor
// 0x6c1f6ff04e070923, bytes +0x20..+0x48): identity rotation, position, scale 1.
constexpr float POS_X = -0.15f;  // = -half the 0.3 m canvas width: centres the tablet
constexpr uint32_t STOCK[10] = {0, 0, 0, 0x3f800000,               // rotation (0,0,0,1)
                                0xbe199999, 0x3f1ccccd, 0xbeff7cee,  // position (-0.15, 0.6125, -0.499)
                                0x3f800000, 0x3f800000, 0x3f800000}; // scale (1,1,1)

struct Hit { uint32_t* p; };
std::vector<Hit> hits;
std::atomic<float> desired{1.f};
std::atomic<bool> present{false}, rescan{true};

bool readable(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY);
}

// The tablet transform at any size s: stock rotation and y/z, scale (s,s,s), x = -0.15 s.
bool matches(const uint32_t* p) {
    if (memcmp(p, STOCK, 16) || memcmp(p + 5, STOCK + 5, 8) || p[7] != p[8] || p[8] != p[9]) return false;
    float s, x;
    memcpy(&s, p + 7, 4);
    memcpy(&x, p + 4, 4);
    return s >= .49f && s <= 4.01f && std::fabs(x - POS_X * s) < 1e-4f;
}

bool safeMatches(const uint32_t* p) {
    __try { return matches(p); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void scanRegion(uint32_t* base, size_t words) {
    __try {
        for (size_t i = 5; i + 5 < words; i++) {
            if (base[i] != STOCK[5]) continue;  // position y: the one value we never change
            uint32_t* p = base + i - 5;
            if (matches(p)) hits.push_back({p});
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
        if (mbi.State == MEM_COMMIT && readable(mbi.Protect) && mbi.RegionSize < (size_t(1) << 32)) {
            scanRegion(static_cast<uint32_t*>(mbi.BaseAddress), mbi.RegionSize / 4);
            scanned += mbi.RegionSize;
        }
        if (reinterpret_cast<uintptr_t>(a) >= 0x7ffffffe0000ull) break;
    }
    logf("tablet: scanned %zu MB in %llu ms, %zu transform copies", scanned >> 20, GetTickCount64() - t0, hits.size());
    for (auto& h : hits) {
        MEMORY_BASIC_INFORMATION m{};
        VirtualQuery(h.p, &m, sizeof(m));
        float ctx[8] = {};
        __try { memcpy(ctx, h.p - 8, 32); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        logf("tablet:   %p (%s) before: %g %g %g %g %g %g %g %g", static_cast<void*>(h.p),
             m.Type == MEM_IMAGE ? "image" : m.Type == MEM_MAPPED ? "mapped" : "heap",
             ctx[0], ctx[1], ctx[2], ctx[3], ctx[4], ctx[5], ctx[6], ctx[7]);
    }
}

bool write(Hit& h, float k) {
    __try {
        if (!matches(h.p)) return false;
        float x = POS_X * k;
        memcpy(h.p + 4, &x, 4);
        for (int i = 7; i < 10; i++) memcpy(h.p + i, &k, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void worker() {
    float applied = 1.f, gaveUpAt = 0;
    bool everResized = false;
    int failures = 0;
    for (;;) {
        Sleep(200);
        float want = desired;
        if (!present || (want == applied && !rescan)) continue;
        if (failures >= 3) {  // not found three times: wait for a new size or a new tablet
            if (want == gaveUpAt && !rescan) continue;
            failures = 0;
        }
        if (rescan) {
            // Level loads rebuild the tablet; old copies are gone. A size of 1 needs no scan.
            if (want == 1.f && !everResized) { rescan = false; continue; }
            Sleep(1500);  // let the lobby finish instantiating the tablet
            scan();
            rescan = false;
            applied = -1.f;
        }
        if (want == applied) continue;
        int ok = 0;
        for (auto& h : hits) ok += write(h, want);
        hits.erase(std::remove_if(hits.begin(), hits.end(), [](const Hit& h) { return !safeMatches(h.p); }), hits.end());
        logf("tablet: size %.2f applied to %d of %zu copies", want, ok, hits.size());
        applied = want;
        everResized |= ok > 0;
        if (ok) failures = 0;
        else if (++failures >= 3) { gaveUpAt = want; logf("tablet: transform not found; resizing paused until the size or tablet changes"); }
        else { rescan = true; Sleep(3000); }  // stale: the tablet was rebuilt, look again (slowly)
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

void tabletPresent(bool p) {
    if (p && !present) rescan = true;
    present = p;
}

}  // namespace tablet
