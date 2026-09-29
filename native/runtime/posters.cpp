// Lobby-poster screen: DOCK, poster touch and the light gun (see posters.h).
//
// Engine facts (echovr.exe 1683152886), all checked by signature at startup:
//   CR15NetDynamicPosterCS (component key sym("R15NetDynamicPoster"))
//     +0x80 gamespace, +0xfc u16 poster count, +0x100 posters (0x58 each), +0x108 keys (16 each,
//     first qword = the poster actor's handle; its low 16 bits index the gamespace's actor
//     name tables, gamespace +0x370 -> +0xb8, else +0x80, as the engine's own messages do).
//     Poster: +0x04 u32 art slice, +0x08 texture it shows (-1 = the level's fallback art),
//     +0x10 texture it replaces on the model, +0x18 handle for +0x08, +0x48 server poster key.
//     The system has no per-frame update. 0x140d00050 start (phase callback, once per system
//     when its level starts) subscribes 0x140d0f900 to the gamespace event
//     evt_client_settings_changed; that handler resolves +0x48 to +0x08 and re-applies +0x08
//     when it changed (fallback art while it is -1). So DOCK runs from the touch-button
//     Update instead, on the poster systems learned at start.
//     0x140d20410 OverrideTexture(cs, key, poster, slice): loads +0x08 into +0x18 and puts it
//     on the poster model. 0x140cd9430 FallbackArt(cs, key, poster, slice): the level's own
//     art back on the model. 0x140ca3740 destructor.
//   Touch buttons (CR15ButtonInteractCS): after its update, +0x1f0 / +0x200 hold the two
//     fingertips in world space as (x, y, z, radius); the engine writes radius 0.0085 (8.5 mm).
//   CR15NetBulletCS 0x140ce2020 Fire(cs, bullet, info, speed, muzzle, dir, fromNetwork, flag):
//     bullet = +0xf8 + 0x298 * u16 at (+0xc8 + 4 * handle); its +0xf8 position and +0x104
//     direction are set by Fire.
//   Gamespace +0xd0 rotation (x, y, z, w), +0xe0 position, +0xec scale: its place in the world.
#include "posters.h"
#include "d3d12_stream.h"
#include "log.h"
#include "../generated/arcade_tab.h"
#include "../generated/posters.h"
#include "../vendor/minhook/include/MinHook.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace posters {
namespace {

using U = uint64_t;
using P = unsigned char*;
template <class T> T& at(void* p, size_t n) { return *reinterpret_cast<T*>(static_cast<P>(p) + n); }
constexpr U NONE = ~U(0);

constexpr unsigned RVA_START = 0xd00050, RVA_DESTROY = 0xca3740, RVA_OVERRIDE = 0xd20410, RVA_FALLBACK = 0xcd9430,
                   RVA_FIRE = 0xce2020, RVA_FIND = 0x302780;
struct Signature { unsigned rva; unsigned char bytes[16]; };
constexpr Signature SIGNATURES[] = {
    {RVA_FIND, {0x48,0x89,0x5c,0x24,0x08,0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55}},
    {RVA_START, {0x40,0x53,0x48,0x83,0xec,0x40,0x48,0x8b,0xd9,0xe8,0x32,0xe0,0x3c,0xff,0x85,0xc0}},
    {RVA_DESTROY, {0x48,0x89,0x4c,0x24,0x08,0x53,0x56,0x57,0x48,0x83,0xec,0x20,0x8b,0xf2,0x48,0x8b}},
    {RVA_OVERRIDE, {0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x48,0x83,0xec,0x40,0x48,0x8b,0x81,0xb8}},
    {RVA_FALLBACK, {0x48,0x89,0x5c,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x40,0x48,0x8b,0x81}},
    {RVA_FIRE, {0x48,0x89,0x5c,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}},
};
constexpr unsigned POSTER_STRIDE = 0x58, BULLET_STRIDE = 0x298;

using Start = U (*)(void*);
using Destroy = void* (*)(void*, unsigned);
using Override = int (*)(void*, void*, void*, U);
using Fallback = void (*)(void*, void*, void*, U);
using Fire = U (*)(void*, unsigned, void*, float, void*, void*, int, int);
// 0x140302780 FindComponent(out, gamespace, actor handle, component name): asks each component
// system for that actor's instance (OverrideTexture finds the poster's model this way).
struct FoundComponent { void* system = nullptr; unsigned index = 0xffff; unsigned char pad[4] = {}; };
using Find = void (*)(FoundComponent*, void*, U, U);
Find findComponent;
Start startOriginal;
Destroy destroyOriginal;
Override overrideTexture;
Fallback fallbackArt;
Fire fireOriginal;

arcade::Shared* shared = nullptr;
std::atomic<bool> fault{false};
std::mutex mutex;

// ---- geometry ----
struct Vec { float x = 0, y = 0, z = 0; };
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(Vec a) { return std::sqrt(dot(a, a)); }
bool finite(Vec a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z) && std::fabs(a.x) + std::fabs(a.y) + std::fabs(a.z) < 1e5f; }

// A placement: world = position + rotate(rotation, scale * local). Posters scale uniformly.
struct Xform {
    float q[4] = {0, 0, 0, 1};
    Vec t;
    float s = 1;
    Vec rotate(Vec v, bool inverse = false) const {
        Vec u{inverse ? -q[0] : q[0], inverse ? -q[1] : q[1], inverse ? -q[2] : q[2]};
        Vec c = cross(u, v) + v * q[3];
        return v + cross(u, c) * 2.f;
    }
    Vec apply(Vec v) const { return t + rotate(v * s); }
    Vec unapply(Vec w) const { return rotate(w - t, true) * (1.f / s); }
};

Xform gamespaceXform(void* gs) {
    Xform x;
    memcpy(x.q, static_cast<P>(gs) + 0xd0, 16);
    memcpy(&x.t, static_cast<P>(gs) + 0xe0, 12);
    x.s = at<float>(gs, 0xec);
    float n = x.q[0] * x.q[0] + x.q[1] * x.q[1] + x.q[2] * x.q[2] + x.q[3] * x.q[3];
    if (!(std::fabs(n - 1) < .01f) || !finite(x.t) || !(x.s > .01f && x.s < 100)) {
        static int logged = 0;
        if (logged++ < 3) logf("posters: gamespace %p has no usable placement; using the level's own space", gs);
        return Xform{};
    }
    return x;
}

struct Tri { Vec a, b, c, n; float uv[3][2]; };
struct Mesh { std::vector<Tri> tris; Vec center; };
std::vector<Mesh> meshes;
std::unordered_map<U, unsigned> table;  // poster actor -> POSTERS index

// Closest point on a triangle (Ericson, Real-Time Collision Detection 5.1.5), as barycentric weights.
void closest(const Tri& t, Vec p, float& wa, float& wb, float& wc) {
    Vec ab = t.b - t.a, ac = t.c - t.a, ap = p - t.a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { wa = 1; wb = wc = 0; return; }
    Vec bp = p - t.b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { wb = 1; wa = wc = 0; return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { float v = d1 / (d1 - d3); wa = 1 - v; wb = v; wc = 0; return; }
    Vec cp = p - t.c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { wc = 1; wa = wb = 0; return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { float w = d2 / (d2 - d6); wa = 1 - w; wc = w; wb = 0; return; }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); wb = 1 - w; wc = w; wa = 0; return; }
    float denom = 1.f / (va + vb + vc);
    wb = vb * denom; wc = vc * denom; wa = 1 - wb - wc;
}

// Mesh UV -> host screen pixel (the shader doubles v; see arcade_layout.py).
bool toPixel(float u, float v, int& x, int& y) {
    float fx = u * POSTER_TEX_W - POSTER_X0, fy = 2 * v * POSTER_TEX_H - POSTER_Y0;
    if (!(fx >= 0 && fy >= 0 && fx < POSTER_FRAME_W && fy < POSTER_FRAME_H)) return false;
    x = int(fx); y = int(fy);
    return true;
}
void interpolate(const Tri& t, float wa, float wb, float wc, float& u, float& v) {
    u = t.uv[0][0] * wa + t.uv[1][0] * wb + t.uv[2][0] * wc;
    v = t.uv[0][1] * wa + t.uv[1][1] * wb + t.uv[2][1] * wc;
}

// ---- dock state ----
struct Dock {
    bool active = false;
    void* cs = nullptr;
    void* gs = nullptr;
    unsigned index = 0;
    U actor = 0, handle = 0, savedName = NONE, savedKey = NONE;
    unsigned mesh = 0;
    Xform level, space;  // poster in its level, level in the world
    U lastSeen = 0;
} dock;
LONG handled = 0;               // last dockSerial answered
U lastTick = 0;                 // the touch-button Update last drove DOCK
std::set<void*> systems;        // live poster systems (from start to destructor)
std::set<void*> described;      // logged once each

// A poster-system key's actor name, as the engine names it (OverrideTexture's messages):
// gamespace +0x370 -> the +0xb8 table at the handle's low 16 bits, else (-1 there) +0x80.
U nameOf(void* gs, U handle) {
    P names = at<P>(gs, 0x370);
    if (!names) return NONE;
    size_t index = size_t(handle & 0xffff);
    for (size_t list : {size_t(0xb8), size_t(0x80)}) {
        P a = at<P>(names, list);
        U name = a ? at<U>(a, index * 8) : NONE;
        if (name != NONE) return name;
    }
    return NONE;
}

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    if (reinterpret_cast<uintptr_t>(p) < 0x10000 || reinterpret_cast<uintptr_t>(p) > 0x7fffffffffffull) return false;
    if (!VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    if (!(m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) return false;
    return static_cast<const unsigned char*>(p) + n <= static_cast<unsigned char*>(m.BaseAddress) + m.RegionSize;
}

bool safeFind(FoundComponent* f, void* gs, U handle, U component) {
    __try { findComponent(f, gs, handle, component); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The render record of a poster slot's model, found the way OverrideTexture finds the model
// (FindComponent with the handle and the poster's model component, +0x50), then as the
// texture apply (0x1403424d0) indexes it: +0xd0 u16 [index * 4] -> j, +0x100 u16
// [j * 16 + 4] -> k, record k of 0x88 bytes at +0x400.
P modelRecord(void* cs, unsigned slot) {
    void* gs = at<void*>(cs, 0x80);
    P item = at<P>(cs, 0x100) + size_t(slot) * POSTER_STRIDE;
    FoundComponent f;
    if (!safeFind(&f, gs, at<U>(at<P>(cs, 0x108), size_t(slot) * 16), at<U>(item, 0x50))) return nullptr;
    P ms = static_cast<P>(f.system);
    if (!ms || f.index == 0xffff || !readable(ms, 0x408)) return nullptr;
    P a = at<P>(ms, 0xd0), b = at<P>(ms, 0x100), records = at<P>(ms, 0x400);
    if (!readable(a + size_t(f.index) * 4, 2)) return nullptr;
    unsigned j = at<unsigned short>(a, size_t(f.index) * 4);
    if (!readable(b + size_t(j) * 16 + 4, 2)) return nullptr;
    unsigned k = at<unsigned short>(b, size_t(j) * 16 + 4);
    P rec = records + size_t(k) * 0x88;
    return readable(rec, 0x88) ? rec : nullptr;
}

// Which POSTERS entry's position is in a render record (as x, y, z in a row, or a matrix
// column 16 bytes apart), or -1. Sets `where` to the byte offset of x.
int posterAt(P rec, int& where) {
    const float* f = reinterpret_cast<const float*>(rec);
    constexpr int N = 0x88 / 4;
    for (unsigned i = 0; i < sizeof(POSTERS) / sizeof(POSTERS[0]); i++) {
        const float* p = POSTERS[i].pos;
        for (int o = 0; o < N; o++)
            for (int step : {1, 4}) {
                if (o + 2 * step >= N) continue;
                if (std::fabs(f[o] - p[0]) < .01f && std::fabs(f[o + step] - p[1]) < .01f && std::fabs(f[o + 2 * step] - p[2]) < .01f) {
                    where = o * 4 + (step == 4 ? 0x1000 : 0);
                    return int(i);
                }
            }
    }
    return -1;
}

std::map<std::pair<void*, unsigned>, int> slotPoster;  // (poster system, slot) -> POSTERS index or -1

// Which POSTERS entry a poster slot is, or -1: the poster at its model's rendered position;
// else by the gamespace name tables (they name the combat lobby's slots wrongly).
int posterOf(void* cs, unsigned slot) {
    auto cached = slotPoster.find({cs, slot});
    if (cached != slotPoster.end()) return cached->second;
    void* gs = at<void*>(cs, 0x80);
    U handle = at<U>(at<P>(cs, 0x108), size_t(slot) * 16);
    auto named = table.find(nameOf(gs, handle));
    int byName = named == table.end() ? -1 : int(named->second), where = -1;
    P rec = modelRecord(cs, slot);
    int byModel = rec ? posterAt(rec, where) : -1;
    int which = byModel >= 0 ? byModel : byName;
    slotPoster[{cs, slot}] = which;
    logf("posters:   slot %u handle %016llx: model %s -> %016llx (at %x), name table -> %016llx; using %s", slot, handle,
         rec ? "record" : "NOT FOUND", byModel >= 0 ? POSTERS[byModel].actor : NONE, where,
         byName >= 0 ? POSTERS[byName].actor : nameOf(gs, handle), byModel >= 0 ? "the model" : "the name table");
    if (rec && byModel < 0) {
        const float* f = reinterpret_cast<const float*>(rec);
        char line[512];
        int n = 0;
        for (int i = 0; i < 0x88 / 4 && n < int(sizeof(line)) - 16; i++) n += snprintf(line + n, sizeof(line) - n, " %.3f", f[i]);
        logf("posters:     record floats:%s", line);
    }
    return which;
}

bool hasFace(unsigned mesh) { return mesh != POSTER_NO_MESH; }

// A poster's middle in the world: its face's middle, or its origin when it has no face map.
Vec centerOf(const PosterInfo& info, const Xform& space) {
    Xform level{{info.rot[0], info.rot[1], info.rot[2], info.rot[3]}, {info.pos[0], info.pos[1], info.pos[2]}, info.scale[0]};
    return space.apply(level.apply(hasFace(info.mesh) ? meshes[info.mesh].center : Vec{}));
}

void answer(LONG state, const char* fmt, ...) {
    char text[sizeof(shared->dockText)];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    memcpy(shared->dockText, text, sizeof(text));
    shared->dockState = state;
    MemoryBarrier();
    shared->dockDone = handled;
    logf("posters: %s", text);
}

// ---- fingertips (from the touch-button systems) ----
struct Hands { Vec tip[2]; bool valid[2] = {}; void* gs = nullptr; U tick = 0; };
std::map<void*, Hands> hands;   // by button system
void* touchSource = nullptr;    // the button system whose fingertips drive the docked poster
struct Finger { bool down = false; int x = 0, y = 0; };
Finger fingers[2];

bool playerPosition(void* gs, U now, Vec& out) {
    const Hands* best = nullptr;
    for (auto& [cs, h] : hands) {
        if (now - h.tick > 1500 || !(h.valid[0] || h.valid[1])) continue;
        if (!best || (h.gs == gs && best->gs != gs)) best = &h;
    }
    if (!best) return false;
    out = best->valid[0] && best->valid[1] ? (best->tip[0] + best->tip[1]) * .5f : best->tip[best->valid[0] ? 0 : 1];
    return true;
}

void releaseFingers() {
    for (unsigned i = 0; i < 2; i++)
        if (fingers[i].down) { arcade::pushTouch(shared, i, arcade::PointUp, fingers[i].x, fingers[i].y); fingers[i].down = false; }
}

// A poster's own art back on its model: the server poster it had, else the level's art.
void restoreArt(void* cs, unsigned index, U name, U serverKey) {
    P item = at<P>(cs, 0x100) + size_t(index) * POSTER_STRIDE;
    U key[2] = {at<U>(at<P>(cs, 0x108), size_t(index) * 16), at<U>(at<P>(cs, 0x108), size_t(index) * 16 + 8)};
    at<U>(item, 0x48) = serverKey;
    at<U>(item, 0x08) = name;
    if (name != NONE) {
        int r = overrideTexture(cs, key, item, at<unsigned>(item, 4));
        if (r == 0) return;
        at<U>(item, 0x08) = NONE;
        logf("posters: restoring the server poster failed (%d); showing the level's art", r);
    }
    fallbackArt(cs, key, item, at<unsigned>(item, 4));  // nothing re-applies it per frame
}

void undock(const char* why, LONG reportState = arcade::Undocked) {
    if (!dock.active) return;
    stream::setPosterActive(false);  // no copies into the texture from here on
    releaseFingers();
    void* cs = dock.cs;
    unsigned index = dock.index;
    bool same = index < at<unsigned short>(cs, 0xfc) && at<U>(at<P>(cs, 0x108), size_t(index) * 16) == dock.handle;
    if (same) restoreArt(cs, index, dock.savedName, dock.savedKey);
    dock = Dock{};
    touchSource = nullptr;
    answer(reportState, "%s", why);
}

// Engine thread, inside the touch-button Update: the nearest poster of every live poster system.
void handleRequest(U now) {
    LONG serial = shared->dockSerial;
    if (serial == handled || systems.empty()) return;  // no poster system: the heartbeat answers
    handled = serial;
    if (!shared->dockWant) {
        if (dock.active) undock("Undocked: the poster shows its own picture again.");
        else answer(arcade::Undocked, "Not docked.");
        return;
    }
    void* bestCs = nullptr;
    unsigned best = 0;
    float bestDistance = 1e9f;
    Vec player, bestPlayer;
    Xform bestSpace;
    bool sawHands = false;
    for (void* cs : systems) {
        void* gs = at<void*>(cs, 0x80);
        if (!playerPosition(gs, now, player)) continue;
        sawHands = true;
        Xform space = gamespaceXform(gs);
        unsigned count = at<unsigned short>(cs, 0xfc);
        P keys = at<P>(cs, 0x108);
        for (unsigned i = 0; i < count && keys; i++) {
            int which = posterOf(cs, i);
            // Posters without a face map are never docked: on the combat-lobby set piece the
            // arcade texture drew blank and a shot at it crashed the GPU.
            if (which < 0 || !hasFace(POSTERS[which].mesh)) continue;
            const PosterInfo& info = POSTERS[which];
            float d = length(centerOf(info, space) - player);
            if (d < bestDistance) { bestDistance = d; bestCs = cs; best = i; bestPlayer = player; bestSpace = space; }
        }
    }
    if (!sawHands) {
        answer(arcade::DockFailed, "Could not find your hands - touch the tablet, then press DOCK again.");
        return;
    }
    if (!bestCs) { answer(arcade::DockFailed, "No poster here can show the arcade."); return; }
    if (dock.active) undock("Moving to the nearest poster.");
    handled = serial;  // undock() answered with the same serial; the final answer follows
    void* cs = bestCs;
    void* gs = at<void*>(cs, 0x80);
    P keys = at<P>(cs, 0x108);
    const PosterInfo& info = POSTERS[posterOf(cs, best)];
    P item = at<P>(cs, 0x100) + size_t(best) * POSTER_STRIDE;
    Dock d;
    d.cs = cs; d.gs = gs; d.index = best; d.actor = info.actor; d.handle = at<U>(keys, size_t(best) * 16); d.mesh = info.mesh;
    d.level = Xform{{info.rot[0], info.rot[1], info.rot[2], info.rot[3]}, {info.pos[0], info.pos[1], info.pos[2]}, info.scale[0]};
    d.space = bestSpace;
    d.savedName = at<U>(item, 0x08);
    d.savedKey = at<U>(item, 0x48);
    at<U>(item, 0x48) = NONE;  // stop the server's poster from being resolved back in
    at<U>(item, 0x08) = ARCADE_POSTER_TEXTURE;
    U key[2] = {at<U>(keys, size_t(best) * 16), at<U>(keys, size_t(best) * 16 + 8)};
    int r = overrideTexture(cs, key, item, NONE);
    if (r != 0) {
        restoreArt(cs, best, NONE, d.savedKey);  // as the engine does when a poster fails to load
        answer(arcade::DockFailed, "The poster refused the arcade screen (error %d, see runtime.log).", r);
        return;
    }
    d.active = true;
    d.lastSeen = now;
    dock = d;
    Vec center = centerOf(info, d.space);
    logf("posters: docked on %016llx (system %p slot %u, was %016llx/%016llx) at %.2f %.2f %.2f; you at %.2f %.2f %.2f; texture %s%s",
         info.actor, cs, best, d.savedName, d.savedKey, center.x, center.y, center.z, bestPlayer.x, bestPlayer.y, bestPlayer.z,
         stream::posterTextureReady() ? "ready" : "NOT CREATED YET", hasFace(d.mesh) ? "" : "; no face map (no touch or shots)");
    Vec lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (auto& t : meshes[d.mesh].tris)
        for (Vec p : {t.a, t.b, t.c}) {
            Vec w = d.space.apply(d.level.apply(p));
            lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
            hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
        }
    logf("posters: the docked face spans %.2f..%.2f, %.2f..%.2f, %.2f..%.2f", lo.x, hi.x, lo.y, hi.y, lo.z, hi.z);
    stream::setPosterActive(true);
    if (hasFace(d.mesh))
        answer(arcade::Docked, "Docked on the nearest poster (%.1f m away). Touch it, or shoot it in combat.", bestDistance);
    else
        answer(arcade::Docked, "Docked on the nearest poster (%.1f m away). This one has no touch or shots yet: use the tablet.", bestDistance);
}

void describe(void* cs) {
    void* gs = at<void*>(cs, 0x80);
    unsigned count = at<unsigned short>(cs, 0xfc), known = 0;
    P keys = at<P>(cs, 0x108);
    for (unsigned i = 0; i < count && keys; i++) known += posterOf(cs, i) >= 0 ? 1 : 0;
    Xform space = gamespaceXform(gs);
    logf("posters: poster system %p (gamespace %p at %.2f %.2f %.2f x%.2f): %u posters, %u can dock",
         cs, gs, space.t.x, space.t.y, space.t.z, space.s, count, known);  // posterOf logged each slot
}

// Keeps the arcade on the docked poster.
void maintain(U now) {
    if (!dock.active) return;
    void* cs = dock.cs;
    dock.lastSeen = now;
    unsigned index = dock.index;
    if (index >= at<unsigned short>(cs, 0xfc) || at<U>(at<P>(cs, 0x108), size_t(index) * 16) != dock.handle) {
        stream::setPosterActive(false);
        releaseFingers();
        dock = Dock{};
        answer(arcade::Undocked, "The poster went away; undocked.");
        return;
    }
    P item = at<P>(cs, 0x100) + size_t(index) * POSTER_STRIDE;
    // The server assigned new art to this poster while we are docked: remember it for
    // undock and keep the arcade on.
    if (at<U>(item, 0x48) != NONE) { dock.savedKey = at<U>(item, 0x48); at<U>(item, 0x48) = NONE; }
    if (at<U>(item, 0x08) != ARCADE_POSTER_TEXTURE) {
        logf("posters: the poster's art changed under the dock; putting the arcade back");
        dock.savedName = at<U>(item, 0x08);
        at<U>(item, 0x08) = ARCADE_POSTER_TEXTURE;
        U key[2] = {at<U>(at<P>(cs, 0x108), size_t(index) * 16), at<U>(at<P>(cs, 0x108), size_t(index) * 16 + 8)};
        stream::setPosterActive(false);
        int r = overrideTexture(cs, key, item, NONE);
        if (r == 0) stream::setPosterActive(true);
        else { at<U>(item, 0x08) = dock.savedName; dock = Dock{}; answer(arcade::Undocked, "The server replaced the poster; undocked."); }
    }
}

// Engine thread, after a touch-button Update (every frame while a lobby is up).
void tick(U now) {
    lastTick = now;
    for (void* cs : systems)
        if (at<unsigned short>(cs, 0xfc) && described.insert(cs).second) describe(cs);  // once it has its posters
    maintain(now);
    handleRequest(now);
}

// ---- hooks ----
void failPosters(const char* why) {
    if (fault.exchange(true)) return;
    logf("posters: FAULT: %s -- dock, poster touch and light gun disabled for this session", why);
    stream::setPosterActive(false);
}

void safeTick(U now) {
    __try { tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { failPosters("access fault in the poster tick"); }
}

U start(void* cs) {
    U r = startOriginal(cs);
    std::lock_guard<std::mutex> lock(mutex);
    systems.insert(cs);
    return r;
}

void* destroy(void* cs, unsigned flags) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        systems.erase(cs);
        described.erase(cs);
        for (auto it = slotPoster.begin(); it != slotPoster.end();) it = it->first.first == cs ? slotPoster.erase(it) : std::next(it);
        if (dock.active && dock.cs == cs) {
            stream::setPosterActive(false);  // before the level frees anything
            releaseFingers();
            dock = Dock{};
            answer(arcade::Undocked, "Left the lobby; undocked.");
        }
    }
    return destroyOriginal(cs, flags);
}

float lastFace = 1e9f, lastFront = 0;  // the last contact() test: metres from the face, and in front of it

// Where a fingertip touches the docked poster: pixel, or false when not touching.
bool contact(Vec tip, bool wasDown, int& x, int& y) {
    if (!hasFace(dock.mesh)) return false;
    Vec local = dock.level.unapply(dock.space.unapply(tip));
    float scale = dock.level.s * dock.space.s;
    float best = 1e9f, bestSigned = 0, u = 0, v = 0;
    for (auto& t : meshes[dock.mesh].tris) {
        float wa, wb, wc;
        closest(t, local, wa, wb, wc);
        Vec p = t.a * wa + t.b * wb + t.c * wc;
        float distance = length(local - p) * scale;
        if (distance < best) {
            best = distance;
            bestSigned = dot(local - p, t.n) * scale;
            interpolate(t, wa, wb, wc, u, v);
        }
    }
    lastFace = best;
    lastFront = bestSigned;
    // Touching: within 1.5 cm in front (4 cm once down) and up to 8 cm through the surface.
    float front = wasDown ? .04f : .015f;
    if (!(bestSigned < front && bestSigned > -.08f && best < .12f)) return false;
    return toPixel(u, v, x, y);
}

void afterButton(void* cs, U now) {
    Hands h;
    h.gs = at<void*>(cs, 0x80);
    h.tick = now;
    for (int i = 0; i < 2; i++) {
        const float* f = reinterpret_cast<const float*>(static_cast<P>(cs) + (i ? 0x200 : 0x1f0));
        h.tip[i] = {f[0], f[1], f[2]};
        h.valid[i] = f[3] > 0 && f[3] < .1f && finite(h.tip[i]) && (f[0] != 0 || f[1] != 0 || f[2] != 0);
    }
    if (!h.valid[0] && !h.valid[1]) return;
    if (hands.find(cs) == hands.end()) {
        static int logged = 0;
        if (logged++ < 8)
            logf("posters: hands from touch system %p (gamespace %p): %.2f %.2f %.2f / %.2f %.2f %.2f", cs, h.gs,
                 h.tip[0].x, h.tip[0].y, h.tip[0].z, h.tip[1].x, h.tip[1].y, h.tip[1].z);
    }
    hands[cs] = h;
    for (auto it = hands.begin(); it != hands.end();) it = now - it->second.tick > 5000 ? hands.erase(it) : std::next(it);
    if (!dock.active) return;
    auto source = hands.find(touchSource);
    bool stale = !touchSource || source == hands.end() || now - source->second.tick > 500;
    if (stale || (h.gs == dock.gs && touchSource != cs && source->second.gs != dock.gs)) {
        if (touchSource != cs) logf("posters: fingertips from touch system %p (gamespace %p)", cs, h.gs);
        touchSource = cs;
    }
    if (touchSource != cs) return;
    for (unsigned i = 0; i < 2; i++) {
        Finger& f = fingers[i];
        int x = 0, y = 0;
        bool on = h.valid[i] && contact(h.tip[i], f.down, x, y);
        static U lastNear = 0;
        if (h.valid[i] && !on && lastFace < .5f && now - lastNear > 2000) {
            lastNear = now;
            logf("posters: finger %u near the poster but not touching: %.3f m from the face, %.3f m in front", i, lastFace, lastFront);
        }
        if (on && !f.down) {
            f = {true, x, y};
            arcade::pushTouch(shared, i, arcade::PointDown, x, y);
            static int logged = 0;
            if (logged++ < 5) logf("posters: finger %u touched the poster at pixel %d,%d", i, x, y);
        } else if (on && (std::abs(x - f.x) >= 2 || std::abs(y - f.y) >= 2)) {
            f.x = x; f.y = y;
            arcade::pushTouch(shared, i, arcade::PointMove, x, y);
        } else if (!on && f.down) {
            f.down = false;
            arcade::pushTouch(shared, i, arcade::PointUp, f.x, f.y);
        }
    }
}

// Where a ray (world space) meets the docked poster's picture: pixel and distance in metres.
bool rayHit(Vec worldOrigin, Vec worldDir, int& x, int& y, float& metres) {
    if (!hasFace(dock.mesh)) return false;
    Vec o = dock.level.unapply(dock.space.unapply(worldOrigin));
    Vec d = dock.level.rotate(dock.space.rotate(worldDir, true), true);
    float bestT = 1e9f, u = 0, v = 0;
    for (auto& t : meshes[dock.mesh].tris) {  // Moller-Trumbore
        Vec e1 = t.b - t.a, e2 = t.c - t.a, pv = cross(d, e2);
        float det = dot(e1, pv);
        if (std::fabs(det) < 1e-9f) continue;
        float inv = 1 / det;
        Vec tv = o - t.a;
        float b = dot(tv, pv) * inv;
        if (b < 0 || b > 1) continue;
        Vec qv = cross(tv, e1);
        float c = dot(d, qv) * inv;
        if (c < 0 || b + c > 1) continue;
        float along = dot(e2, qv) * inv;
        if (along > 0 && along < bestT) { bestT = along; interpolate(t, 1 - b - c, b, c, u, v); }
    }
    metres = bestT * length(d) * dock.level.s * dock.space.s;
    return bestT < 1e9f && toPixel(u, v, x, y);
}

// Light gun: a local shot's ray against the docked poster's face.
void afterFire(void* cs, unsigned handle) {
    if (!dock.active || !hasFace(dock.mesh)) return;
    P bullet = at<P>(cs, 0xf8) + size_t(at<unsigned short>(at<P>(cs, 0xc8), size_t(handle & 0xffff) * 4)) * BULLET_STRIDE;
    Vec origin = at<Vec>(bullet, 0xf8), dir = at<Vec>(bullet, 0x104);
    if (!finite(origin) || !finite(dir) || length(dir) < .5f) return;
    Xform space = gamespaceXform(at<void*>(cs, 0x80));
    Vec worldOrigin = space.apply(origin), worldDir = space.rotate(dir);
    static int logged = 0;
    int x, y;
    float metres = 0;
    if (rayHit(worldOrigin, worldDir, x, y, metres)) {
        arcade::pushTouch(shared, arcade::SHOT_POINTER, arcade::Shot, x, y);
        InterlockedIncrement(&shared->shots);
        if (logged++ < 20) logf("light gun: hit pixel %d,%d (%.1f m from the muzzle)", x, y, metres);
    } else if (logged++ < 20) {
        // Which known poster the shot went nearest to: tells a bad poster lookup from a bad aim.
        U nearActor = 0;
        float closest = 1e9f;
        Vec unit = worldDir * (1.f / length(worldDir));
        unsigned count = at<unsigned short>(dock.cs, 0xfc);
        P keys = at<P>(dock.cs, 0x108);
        for (unsigned i = 0; i < count && keys; i++) {
            int which = posterOf(dock.cs, i);
            if (which < 0) continue;
            const PosterInfo& info = POSTERS[which];
            Vec c = centerOf(info, dock.space) - worldOrigin;
            float along = std::max(0.f, dot(c, unit)), d = length(c - unit * along);
            if (d < closest) { closest = d; nearActor = info.actor; }
        }
        logf("light gun: shot from %.2f %.2f %.2f dir %.2f %.2f %.2f missed the docked poster %016llx; nearest poster to the shot %016llx (%.1f m off)",
             worldOrigin.x, worldOrigin.y, worldOrigin.z, worldDir.x, worldDir.y, worldDir.z, dock.actor, nearActor, closest);
    }
}

void safeAfterFire(void* cs, unsigned handle) {
    __try { afterFire(cs, handle); } __except (EXCEPTION_EXECUTE_HANDLER) { failPosters("access fault reading a bullet"); }
}

U fire(void* cs, unsigned handle, void* info, float speed, void* muzzle, void* dir, int fromNetwork, int flag) {
    U r = fireOriginal(cs, handle, info, speed, muzzle, dir, fromNetwork, flag);
    if (!fault && !fromNetwork && dock.active) {
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);  // never block an engine thread
        if (!lock.owns_lock()) return r;
        safeAfterFire(cs, handle);
    }
    return r;
}

void safeAfterButton(void* cs, U now) {
    __try { afterButton(cs, now); } __except (EXCEPTION_EXECUTE_HANDLER) { failPosters("access fault reading fingertips"); }
}

void buildGeometry() {
    meshes.clear();
    table.clear();
    for (auto& m : POSTER_MESHES) {
        Mesh mesh;
        Vec sum;
        for (unsigned i = 0; i < m.count; i++) {
            const PosterTri& p = m.tris[i];
            Tri t;
            t.a = {p.p[0][0], p.p[0][1], p.p[0][2]};
            t.b = {p.p[1][0], p.p[1][1], p.p[1][2]};
            t.c = {p.p[2][0], p.p[2][1], p.p[2][2]};
            Vec n = cross(t.b - t.a, t.c - t.a);
            t.n = n * (1.f / std::max(length(n), 1e-9f));
            memcpy(t.uv, p.uv, sizeof(t.uv));
            sum = sum + (t.a + t.b + t.c) * (1.f / 3);
            mesh.tris.push_back(t);
        }
        mesh.center = sum * (1.f / float(std::max(1u, m.count)));
        meshes.push_back(std::move(mesh));
    }
    for (unsigned i = 0; i < sizeof(POSTERS) / sizeof(POSTERS[0]); i++) table[POSTERS[i].actor] = i;
}

bool hook(P exe, unsigned rva, void* detour, void** original) {
    MH_STATUS s = MH_CreateHook(exe + rva, detour, original);
    if (s == MH_OK) s = MH_EnableHook(exe + rva);
    if (s != MH_OK) { logf("posters: hook at %x failed: %s", rva, MH_StatusToString(s)); return false; }
    return true;
}

}  // namespace

bool install(unsigned char* exe, arcade::Shared* s) {
    shared = s;
    if (!shared) return false;
    for (auto& sig : SIGNATURES)
        if (memcmp(exe + sig.rva, sig.bytes, 16) != 0) { logf("posters: code signature mismatch at %x; dock disabled", sig.rva); return false; }
    buildGeometry();
    overrideTexture = reinterpret_cast<Override>(exe + RVA_OVERRIDE);
    fallbackArt = reinterpret_cast<Fallback>(exe + RVA_FALLBACK);
    findComponent = reinterpret_cast<Find>(exe + RVA_FIND);
    handled = shared->dockSerial;  // ignore requests from before this session
    shared->dockDone = handled;
    shared->dockState = arcade::Undocked;
    bool ok = hook(exe, RVA_START, reinterpret_cast<void*>(start), reinterpret_cast<void**>(&startOriginal)) &&
              hook(exe, RVA_DESTROY, reinterpret_cast<void*>(destroy), reinterpret_cast<void**>(&destroyOriginal)) &&
              hook(exe, RVA_FIRE, reinterpret_cast<void*>(fire), reinterpret_cast<void**>(&fireOriginal));
    if (!ok) { fault = true; return false; }
    logf("posters: dock ready: %zu posters, %zu face meshes", table.size(), meshes.size());
    return true;
}

void afterButtonUpdate(void* cs) {
    if (fault || !shared) return;
    U now = GetTickCount64();
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);  // never block an engine thread
    if (!lock.owns_lock()) return;
    safeAfterButton(cs, now);
    if (!fault) safeTick(now);
}

void heartbeat(unsigned long long now) {
    if (!shared) return;
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    LONG serial = shared->dockSerial;
    if (serial == handled) return;
    if (fault) { handled = serial; answer(arcade::DockFailed, "Dock is off for this session (see runtime.log)."); return; }
    // No poster system, or no touch-button Update to drive it: answer instead of leaving the
    // request hanging.
    static U pendingSince = 0;
    static LONG pendingSerial = 0;
    if (pendingSerial != serial) { pendingSerial = serial; pendingSince = now; }
    if (now - pendingSince < 1500 || (!systems.empty() && lastTick && now - lastTick < 1000)) return;
    handled = serial;
    if (!shared->dockWant) answer(arcade::Undocked, "Not docked.");
    else if (systems.empty()) answer(arcade::DockFailed, "No lobby poster here. Dock works in the social and combat lobbies.");
    else answer(arcade::DockFailed, "Could not find your hands - touch the tablet, then press DOCK again.");
}

}  // namespace posters
