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
constexpr int ENTRY = 12;  // the transform system's local transforms: 48-byte entries

std::vector<uint32_t*> copies;  // stock-valued templates (resource rows, spawn data)
std::vector<uint32_t*> live;    // entries the game animates each frame, next to the copies
std::atomic<float> desired{1.f};
std::atomic<bool> present{false}, rescan{true};
std::atomic<int> failures{0};

// Ordinary heap only. GPU upload heaps are mapped write-combined: reading them from
// the CPU is so slow that the first version of this scan took minutes.
bool scannable(const MEMORY_BASIC_INFORMATION& m) {
    if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE) return false;
    if (m.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE | PAGE_NOCACHE)) return false;
    return (m.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && m.RegionSize < (size_t(1) << 31);
}

// A stock tablet copy at any size s: stock rotation and y/z, scale (s,s,s), x = -0.15 s.
bool isCopy(const uint32_t* p) {
    if (memcmp(p, STOCK, 16) || memcmp(p + 5, STOCK + 5, 8) || p[7] != p[8] || p[8] != p[9]) return false;
    float s, x;
    memcpy(&s, p + 7, 4);
    memcpy(&x, p + 4, 4);
    return s >= .49f && s <= 4.01f && std::fabs(x - POS_X * s) < 1e-4f;
}

// An animated tablet transform: the tablet's depth (-0.499), a unit rotation, nearby
// x/y and a uniform scale.
bool isLive(const uint32_t* e) {
    if (e[6] != STOCK[6] || e[7] != e[8] || e[8] != e[9]) return false;
    float q[4], pos[2], s;
    memcpy(q, e, 16);
    memcpy(pos, e + 4, 8);
    memcpy(&s, e + 7, 4);
    float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return std::fabs(n - 1) < .01f && std::fabs(pos[0]) < 1.5f && std::fabs(pos[1]) < 2.5f && s >= .49f && s <= 4.01f;
}

template <class F> bool guarded(F f) {
    __try { return f(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void scanRegion(uint32_t* base, size_t words) {
    __try {
        for (size_t i = 5; i + 5 < words; i++) {
            if (base[i] != STOCK[5]) continue;  // position y: the one value we never change
            uint32_t* p = base + i - 5;
            if (isCopy(p)) copies.push_back(p);
            if (copies.size() >= 64) return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool liveCandidate(uint32_t* e) { return isLive(e) && !isCopy(e); }

void findLive() {
    live.clear();
    for (auto* p : copies)
        for (int i = -16; i <= 16; i++) {
            uint32_t* e = p + i * ENTRY;
            if (i && guarded([e] { return liveCandidate(e); }) && std::find(live.begin(), live.end(), e) == live.end())
                live.push_back(e);
        }
    for (auto* e : live) {
        float v[10] = {};
        guarded([&] { memcpy(v, e, 40); return true; });
        logf("tablet:   live %p rot %.3f %.3f %.3f %.3f pos %.4f %.4f %.4f scale %.3f", static_cast<void*>(e),
             v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    }
}

void scan() {
    copies.clear();
    ULONGLONG t0 = GetTickCount64();
    size_t scanned = 0, regions = 0;
    logf("tablet: looking for the tablet transform in memory");
    MEMORY_BASIC_INFORMATION mbi;
    for (auto* a = reinterpret_cast<unsigned char*>(0x10000); VirtualQuery(a, &mbi, sizeof(mbi)); a = static_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize) {
        if (scannable(mbi)) {
            scanRegion(static_cast<uint32_t*>(mbi.BaseAddress), mbi.RegionSize / 4);
            scanned += mbi.RegionSize;
            regions++;
        }
        if (reinterpret_cast<uintptr_t>(a) >= 0x7ffffffe0000ull || GetTickCount64() - t0 > 60000) break;
    }
    findLive();
    logf("tablet: scanned %zu MB (%zu regions) in %llu ms: %zu stock copies, %zu animated", scanned >> 20, regions,
         GetTickCount64() - t0, copies.size(), live.size());
}

bool writeCopy(uint32_t* p, float k) {
    return guarded([p, k] {
        if (!isCopy(p)) return false;
        float x = POS_X * k;
        memcpy(p + 4, &x, 4);
        for (int i = 7; i < 10; i++) memcpy(p + i, &k, 4);
        return true;
    });
}

bool writeLive(uint32_t* e, float k) {  // scale only: the game owns its position
    return guarded([e, k] {
        if (!isLive(e)) return false;
        for (int i = 7; i < 10; i++) memcpy(e + i, &k, 4);
        return true;
    });
}

float scaleOf(uint32_t* e) {
    float s = 0;
    guarded([&] { memcpy(&s, e + 7, 4); return true; });
    return s;
}

void worker() {
    float applied = 1.f;
    bool everResized = false, reportedRevert = false;
    for (;;) {
        Sleep(50);
        float want = desired;
        if (!present) continue;
        if (rescan) {
            // Level loads rebuild the tablet; old entries are gone. A size of 1 needs no scan.
            if (want == 1.f && !everResized) { rescan = false; continue; }
            if (failures >= 3) { rescan = false; continue; }
            Sleep(1500);  // let the lobby finish instantiating the tablet
            scan();
            rescan = false;
            applied = -1.f;
            reportedRevert = false;
        }
        // The game may rewrite its animated transforms; keep ours applied (20x a second).
        for (auto* e : live)
            if (scaleOf(e) != want) {
                if (applied == want && !reportedRevert) { logf("tablet: the game reset an animated scale; re-applying"); reportedRevert = true; }
                writeLive(e, want);
            }
        if (want == applied) continue;
        int ok = 0, okLive = 0;
        for (auto* p : copies) ok += writeCopy(p, want);
        for (auto* e : live) okLive += writeLive(e, want);
        copies.erase(std::remove_if(copies.begin(), copies.end(), [](uint32_t* p) { return !guarded([p] { return isCopy(p); }); }), copies.end());
        live.erase(std::remove_if(live.begin(), live.end(), [](uint32_t* e) { return !guarded([e] { return isLive(e); }); }), live.end());
        logf("tablet: size %.2f -> %d stock copies, %d animated", want, ok, okLive);
        applied = want;
        everResized |= ok + okLive > 0;
        if (ok + okLive) failures = 0;
        else if (++failures >= 3) logf("tablet: transform not found; resizing paused until the tablet reloads");
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
    if (p && !present) { rescan = true; failures = 0; }
    present = p;
}

}  // namespace tablet
