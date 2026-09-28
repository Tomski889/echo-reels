#include "d3d12_stream.h"
#include "log.h"
#include "../generated/arcade_tab.h"
#include "../vendor/minhook/include/MinHook.h"
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <thread>

#pragma comment(lib, "d3d12.lib")

namespace stream {
namespace {

// Vtable slots (declaration order in d3d12.h).
constexpr int DEV_CREATE_COMMITTED = 27, DEV_CREATE_PLACED = 29;
constexpr int DEV4_CREATE_COMMITTED1 = 53;
constexpr int DEV8_CREATE_COMMITTED2 = 69, DEV8_CREATE_PLACED1 = 70;
constexpr int QUEUE_EXECUTE = 10;
constexpr int LIST_RESOURCE_BARRIER = 26;

using CreateCommitted = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreatePlaced = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreateCommitted1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
// DESC1 starts with the same fields as DESC, which is all we read.
using CreateCommitted2 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using CreatePlaced1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using Barrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);

CreateCommitted origCommitted;
CreatePlaced origPlaced;
CreateCommitted1 origCommitted1;
CreateCommitted2 origCommitted2;
CreatePlaced1 origPlaced1;
Execute origExecute;
Barrier origBarrier;

arcade::Shared* shared = nullptr;
std::atomic<bool> visible{false}, running{false}, broken{false};
std::thread worker;

std::mutex m;  // guards everything below
ID3D12Device* device = nullptr;
ID3D12Resource* target = nullptr;       // AddRef'd while we hold it
D3D12_RESOURCE_DESC targetDesc{};
std::atomic<ID3D12Resource*> targetFast{nullptr};
D3D12_RESOURCE_STATES targetState = D3D12_RESOURCE_STATE_COMMON;
ID3D12CommandQueue* queue = nullptr;
ID3D12Fence* fence = nullptr;
UINT64 fenceValue = 0;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};

enum class SlotState { Free, Filling, Ready };
struct Slot {
    ID3D12Resource* upload = nullptr;
    uint8_t* mapped = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    UINT64 fenceValue = 0;
    LONG serial = 0;
    SlotState state = SlotState::Free;
};
Slot slots[2];
bool gpuReady = false;
thread_local bool ownCall = false;

bool matches(const D3D12_RESOURCE_DESC* d) {
    return d && d->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d->Width == ARCADE_TEX_W &&
           d->Height == ARCADE_TEX_H && d->DepthOrArraySize == 1 && d->MipLevels <= 1 &&
           (d->Format == DXGI_FORMAT_B8G8R8A8_UNORM || d->Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
            d->Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
}

void releaseGpu() {
    for (auto& s : slots) {
        if (s.upload) { s.upload->Unmap(0, nullptr); s.upload->Release(); }
        if (s.list) s.list->Release();
        if (s.allocator) s.allocator->Release();
        s = Slot{};
    }
    if (fence) fence->Release();
    fence = nullptr; fenceValue = 0; gpuReady = false;
}

void adopt(ID3D12Device* dev, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, REFIID riid, void** ppv, const char* how) {
    if (!ppv || !*ppv || !matches(desc)) return;
    ID3D12Resource* resource = nullptr;
    if (FAILED(static_cast<IUnknown*>(*ppv)->QueryInterface(IID_PPV_ARGS(&resource)))) return;
    std::lock_guard<std::mutex> lock(m);
    if (device && device != dev) { releaseGpu(); queue = nullptr; }
    if (target) target->Release();
    target = resource;  // keep the reference from QueryInterface
    targetFast = resource;
    targetDesc = *desc;
    targetState = state;
    device = dev;
    logf("stream: screen texture created via %s res=%p fmt=%d state=0x%x", how, resource, int(desc->Format), unsigned(state));
}

HRESULT STDMETHODCALLTYPE hookCommitted(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS f,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID riid, void** ppv) {
    HRESULT hr = origCommitted(d, hp, f, desc, s, c, riid, ppv);
    if (SUCCEEDED(hr) && matches(desc)) adopt(d, desc, s, riid, ppv, "CreateCommittedResource");
    return hr;
}
HRESULT STDMETHODCALLTYPE hookPlaced(ID3D12Device* d, ID3D12Heap* h, UINT64 o, const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID riid, void** ppv) {
    HRESULT hr = origPlaced(d, h, o, desc, s, c, riid, ppv);
    if (SUCCEEDED(hr) && matches(desc)) adopt(d, desc, s, riid, ppv, "CreatePlacedResource");
    return hr;
}
HRESULT STDMETHODCALLTYPE hookCommitted1(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS f,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, ID3D12ProtectedResourceSession* p, REFIID riid, void** ppv) {
    HRESULT hr = origCommitted1(d, hp, f, desc, s, c, p, riid, ppv);
    if (SUCCEEDED(hr) && matches(desc)) adopt(d, desc, s, riid, ppv, "CreateCommittedResource1");
    return hr;
}
HRESULT STDMETHODCALLTYPE hookCommitted2(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS f,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, ID3D12ProtectedResourceSession* p, REFIID riid, void** ppv) {
    HRESULT hr = origCommitted2(d, hp, f, desc, s, c, p, riid, ppv);
    if (SUCCEEDED(hr) && matches(desc)) adopt(d, desc, s, riid, ppv, "CreateCommittedResource2");
    return hr;
}
HRESULT STDMETHODCALLTYPE hookPlaced1(ID3D12Device* d, ID3D12Heap* h, UINT64 o, const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID riid, void** ppv) {
    HRESULT hr = origPlaced1(d, h, o, desc, s, c, riid, ppv);
    if (SUCCEEDED(hr) && matches(desc)) adopt(d, desc, s, riid, ppv, "CreatePlacedResource1");
    return hr;
}

// Track the texture's state from the engine's own barriers so ours match.
void STDMETHODCALLTYPE hookBarrier(ID3D12GraphicsCommandList* list, UINT n, const D3D12_RESOURCE_BARRIER* barriers) {
    ID3D12Resource* t = targetFast.load(std::memory_order_relaxed);
    if (t && !ownCall) {
        for (UINT i = 0; i < n; i++) {
            if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && barriers[i].Transition.pResource == t) {
                std::lock_guard<std::mutex> lock(m);
                if (target == t && targetState != barriers[i].Transition.StateAfter) {
                    targetState = barriers[i].Transition.StateAfter;
                    logf("stream: engine moved screen texture to state 0x%x", unsigned(targetState));
                }
            }
        }
    }
    origBarrier(list, n, barriers);
}

bool createGpu() {  // caller holds m
    if (gpuReady) return true;
    if (!device || !target) return false;
    UINT64 total = 0;
    device->GetCopyableFootprints(&targetDesc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    if (footprint.Footprint.RowPitch < arcade::PITCH) { logf("stream: unexpected row pitch %u", footprint.Footprint.RowPitch); broken = true; return false; }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buf.Width = total; buf.Height = 1; buf.DepthOrArraySize = 1;
    buf.MipLevels = 1; buf.SampleDesc.Count = 1; buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ownCall = true;
    bool ok = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    for (auto& s : slots) {
        ok = ok && SUCCEEDED(origCommitted(device, &heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&s.upload)));
        ok = ok && SUCCEEDED(s.upload->Map(0, nullptr, reinterpret_cast<void**>(&s.mapped)));
        ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator)));
        ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator, nullptr, IID_PPV_ARGS(&s.list)));
        ok = ok && SUCCEEDED(s.list->Close());
    }
    ownCall = false;
    if (!ok) { logf("stream: failed to create upload resources"); releaseGpu(); broken = true; return false; }
    gpuReady = true;
    logf("stream: upload path ready (%llu bytes x2, row pitch %u)", total, footprint.Footprint.RowPitch);
    return true;
}

// Worker: copy the newest host frame into a free upload slot.
void workerLoop() {
    LONG lastSerial = -1;
    while (running) {
        Sleep(2);
        if (!visible || broken || !shared || shared->latestFrame == LONG(arcade::NO_FRAME)) continue;
        LONG serial = shared->frameSerial;
        if (serial == lastSerial) continue;
        Slot* slot = nullptr;
        UINT pitch = 0;
        {
            std::lock_guard<std::mutex> lock(m);
            if (!createGpu()) continue;
            UINT64 done = fence->GetCompletedValue();
            for (auto& s : slots)
                if (s.state == SlotState::Free && done >= s.fenceValue) { slot = &s; break; }
            if (!slot) continue;
            slot->state = SlotState::Filling;
            pitch = footprint.Footprint.RowPitch;
        }
        LONG index = shared->latestFrame;
        InterlockedExchange(&shared->readingFrame, index);
        bool ok = index >= 0 && index < LONG(arcade::FRAME_BUFFERS) && shared->latestFrame == index;
        if (ok) {
            const uint8_t* src = shared->frames[index];
            for (UINT y = 0; y < arcade::HEIGHT; y++)
                memcpy(slot->mapped + size_t(y) * pitch, src + size_t(y) * arcade::PITCH, arcade::PITCH);
        }
        InterlockedExchange(&shared->readingFrame, LONG(arcade::NO_FRAME));
        std::lock_guard<std::mutex> lock(m);
        slot->serial = serial;
        slot->state = ok ? SlotState::Ready : SlotState::Free;
        if (ok) lastSerial = serial;
    }
}

void recordAndSubmit(ID3D12CommandQueue* q) {  // caller holds m
    Slot* ready = nullptr;
    for (auto& s : slots)
        if (s.state == SlotState::Ready && (!ready || s.serial - ready->serial > 0)) ready = &s;
    if (!ready || FAILED(ready->allocator->Reset())) return;
    ownCall = true;
    auto list = ready->list;
    list->Reset(ready->allocator, nullptr);
    D3D12_RESOURCE_STATES before = targetState;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = target;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (before != D3D12_RESOURCE_STATE_COPY_DEST) {
        b.Transition.StateBefore = before; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{ready->upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (before != D3D12_RESOURCE_STATE_COPY_DEST) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = before;
        list->ResourceBarrier(1, &b);
    }
    bool closed = SUCCEEDED(list->Close());
    ownCall = false;
    if (!closed) { logf("stream: command list close failed; streaming disabled"); broken = true; return; }
    ID3D12CommandList* lists[] = {list};
    origExecute(q, 1, lists);
    q->Signal(fence, ++fenceValue);
    ready->fenceValue = fenceValue;
    ready->state = SlotState::Free;
    static UINT64 uploads = 0;
    if (++uploads == 1 || uploads % 1800 == 0) logf("stream: %llu frames uploaded (state 0x%x)", uploads, unsigned(before));
}

// Pick the busiest direct queue on the texture's device: that is the frame queue.
void chooseQueue(ID3D12CommandQueue* q) {  // caller holds m
    static ID3D12CommandQueue* seen[8] = {};
    static unsigned counts[8] = {}, total = 0;
    int slot = -1;
    for (int i = 0; i < 8 && slot < 0; i++) if (seen[i] == q) slot = i;
    if (slot < 0) {
        D3D12_COMMAND_QUEUE_DESC desc = q->GetDesc();
        ID3D12Device* qd = nullptr;
        bool eligible = desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qd))) && qd == device;
        if (qd) qd->Release();
        if (!eligible) return;
        for (int i = 0; i < 8 && slot < 0; i++) if (!seen[i]) { seen[i] = q; slot = i; }
        if (slot < 0) return;
    }
    counts[slot]++;
    if (++total < 240) return;
    int best = 0;
    for (int i = 1; i < 8; i++) if (counts[i] > counts[best]) best = i;
    queue = seen[best];
    logf("stream: using direct queue %p (%u of %u submissions)", queue, counts[best], total);
}

void STDMETHODCALLTYPE hookExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (visible && !broken && targetFast.load(std::memory_order_relaxed) && !ownCall) {
        std::unique_lock<std::mutex> lock(m, std::try_to_lock);
        if (lock.owns_lock() && target && gpuReady) {
            if (!queue) chooseQueue(q);
            if (q == queue) recordAndSubmit(q);
        }
    }
    origExecute(q, n, lists);
}

bool hook(void* function, void* detour, void** original, const char* name) {
    MH_STATUS s = MH_CreateHook(function, detour, original);
    if (s == MH_OK) s = MH_EnableHook(function);
    if (s != MH_OK) { logf("stream: hook %s failed: %s", name, MH_StatusToString(s)); return false; }
    return true;
}

}  // namespace

bool install(arcade::Shared* s) {
    shared = s;
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        logf("stream: could not create a probe D3D12 device");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* l = nullptr;
    bool ok = SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) &&
              SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) &&
              SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l)));
    if (ok) {
        auto dv = *reinterpret_cast<void***>(dev);
        auto qv = *reinterpret_cast<void***>(q);
        auto lv = *reinterpret_cast<void***>(l);
        ok = hook(dv[DEV_CREATE_COMMITTED], reinterpret_cast<void*>(hookCommitted), reinterpret_cast<void**>(&origCommitted), "CreateCommittedResource") &&
             hook(dv[DEV_CREATE_PLACED], reinterpret_cast<void*>(hookPlaced), reinterpret_cast<void**>(&origPlaced), "CreatePlacedResource") &&
             hook(qv[QUEUE_EXECUTE], reinterpret_cast<void*>(hookExecute), reinterpret_cast<void**>(&origExecute), "ExecuteCommandLists") &&
             hook(lv[LIST_RESOURCE_BARRIER], reinterpret_cast<void*>(hookBarrier), reinterpret_cast<void**>(&origBarrier), "ResourceBarrier");
        ID3D12Device4* d4 = nullptr;
        if (ok && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d4)))) {
            hook(dv[DEV4_CREATE_COMMITTED1], reinterpret_cast<void*>(hookCommitted1), reinterpret_cast<void**>(&origCommitted1), "CreateCommittedResource1");
            d4->Release();
        }
        ID3D12Device8* d8 = nullptr;
        if (ok && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d8)))) {
            hook(dv[DEV8_CREATE_COMMITTED2], reinterpret_cast<void*>(hookCommitted2), reinterpret_cast<void**>(&origCommitted2), "CreateCommittedResource2");
            hook(dv[DEV8_CREATE_PLACED1], reinterpret_cast<void*>(hookPlaced1), reinterpret_cast<void**>(&origPlaced1), "CreatePlacedResource1");
            d8->Release();
        }
    }
    if (l) l->Release();
    if (a) a->Release();
    if (q) q->Release();
    dev->Release();
    if (!ok) { logf("stream: D3D12 hooks not installed; the tablet will show the splash image only"); return false; }
    running = true;
    worker = std::thread(workerLoop);
    logf("stream: D3D12 hooks installed");
    return true;
}

void setVisible(bool v) {
    if (visible.exchange(v) != v) logf("stream: page %s", v ? "visible, streaming" : "hidden, paused");
}

void shutdown() {
    running = false;
    if (worker.joinable()) worker.join();
}

}  // namespace stream
