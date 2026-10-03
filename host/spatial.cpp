// SpatialAudio (spatial.h).
#define NOMINMAX
#include "spatial.h"
#include "host.h"
#include "spatial_ipc.h"
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

#pragma comment(lib, "mmdevapi.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr int RATE = 48000;
constexpr float BROWSER_VOLUME = .01f, BOOST = 1 / BROWSER_VOLUME;

std::set<DWORD> processTree(DWORD root) {
    std::set<DWORD> tree{root};
    std::multimap<DWORD, DWORD> children;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return tree;
    PROCESSENTRY32W e{sizeof(e)};
    for (BOOL ok = Process32FirstW(snapshot, &e); ok; ok = Process32NextW(snapshot, &e)) children.emplace(e.th32ParentProcessID, e.th32ProcessID);
    CloseHandle(snapshot);
    std::vector<DWORD> todo{root};
    while (!todo.empty()) {
        DWORD p = todo.back();
        todo.pop_back();
        auto range = children.equal_range(p);
        for (auto it = range.first; it != range.second; ++it)
            if (tree.insert(it->second).second) todo.push_back(it->second);
    }
    return tree;
}

// Sets the volume of the tree's playback sessions on every output device
void setTreeVolume(DWORD root, float volume) {
    std::set<DWORD> tree = processTree(root);
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDeviceCollection> outputs;
    UINT count = 0;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) ||
        FAILED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &outputs)) || FAILED(outputs->GetCount(&count)))
        return;
    for (UINT d = 0; d < count; d++) {
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> sessions;
        int n = 0;
        if (FAILED(outputs->Item(d, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(manager.GetAddressOf()))) ||
            FAILED(manager->GetSessionEnumerator(&sessions)) || FAILED(sessions->GetCount(&n)))
            continue;
        for (int i = 0; i < n; i++) {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> control2;
            ComPtr<ISimpleAudioVolume> simple;
            DWORD pid = 0;
            float now = 0;
            if (SUCCEEDED(sessions->GetSession(i, &control)) && SUCCEEDED(control.As(&control2)) && SUCCEEDED(control2->GetProcessId(&pid)) &&
                tree.count(pid) && SUCCEEDED(control.As(&simple)) && SUCCEEDED(simple->GetMasterVolume(&now)) && std::fabs(now - volume) > .001f)
                simple->SetMasterVolume(volume, nullptr);
        }
    }
}

class Activated : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase,
                                                       IActivateAudioInterfaceCompletionHandler> {
public:
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Activated() { CloseHandle(event); }
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation*) override { SetEvent(event); return S_OK; }
};

// Windows' recording of what the process tree plays (Windows 10 2004 and later)
ComPtr<IAudioClient> loopbackClient(DWORD root) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = root;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    auto done = Microsoft::WRL::Make<Activated>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    ComPtr<IAudioClient> client;
    if (FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, done.Get(), &op))) return client;
    WaitForSingleObject(done->event, 5000);
    HRESULT hr = E_FAIL;
    ComPtr<IUnknown> unknown;
    if (FAILED(op->GetActivateResult(&hr, &unknown)) || FAILED(hr) || FAILED(unknown.As(&client))) client.Reset();
    return client;
}

// Where the sound comes from and how: smoothed towards the newest place of the tablet
struct Placement {
    float left = 1, right = 1;  // gains
    int delayLeft = 0, delayRight = 0;  // samples (the far ear hears it later)
    float muffle = 1;  // one-pole low-pass amount, 1 = open (in front), lower = duller (behind)
};

Placement place(const float* p, bool poster) {
    Placement out;
    float d = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (d < .05f) return out;
    float pan = std::clamp(p[0] / d, -1.f, 1.f), front = -p[2] / d;  // Oculus: -z is forward
    // The tablet is in your hand: fade quickly with distance. A poster is metres away: fade gently, never below a third.
    float distance = poster ? std::clamp(1.f / (1.f + std::max(0.f, d - 1.5f) * .35f), .35f, 1.f)
                            : std::clamp(1.f / (1.f + std::max(0.f, d - .35f) * 1.2f), .15f, 1.f);
    float farGain = 1 - .55f * std::fabs(pan);  // the ear away from the sound
    out.left = distance * (pan > 0 ? farGain : 1.f);
    out.right = distance * (pan < 0 ? farGain : 1.f);
    int itd = int(.00065f * RATE * std::fabs(pan) + .5f);
    (pan > 0 ? out.delayLeft : out.delayRight) = itd;
    out.muffle = front < 0 ? .35f + .65f * (1 + front) : 1.f;
    return out;
}

}  // namespace

void SpatialAudio::start(DWORD browserPid) {
    if (running() || !browserPid) return;
    stop_ = false;
    thread_ = std::thread(&SpatialAudio::run, this, browserPid);
}

void SpatialAudio::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void SpatialAudio::run(DWORD root) {
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT, 2, RATE, RATE * 8, 8, 32, 0};
    ComPtr<IAudioClient> capture = loopbackClient(root), render;
    ComPtr<IAudioCaptureClient> captureClient;
    ComPtr<IAudioRenderClient> renderClient;
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDevice> output;
    UINT32 renderFrames = 0;
    bool ok = capture &&
              SUCCEEDED(capture->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 1000000, 0, &format, nullptr)) &&
              SUCCEEDED(capture->GetService(IID_PPV_ARGS(&captureClient))) &&
              SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) &&
              SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &output)) &&
              SUCCEEDED(output->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(render.GetAddressOf()))) &&
              SUCCEEDED(render->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 600000, 0, &format, nullptr)) &&
              SUCCEEDED(render->GetBufferSize(&renderFrames)) && SUCCEEDED(render->GetService(IID_PPV_ARGS(&renderClient)));
    if (!ok) {
        hostLog("spatial audio: could not start (process loopback or output); the browser plays normally");
        CoUninitialize();
        return;
    }
    // The tablet's place, from EchoCam
    spatial_ipc::Shared* shared = nullptr;
    if (HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, spatial_ipc::NAME)) {
        shared = static_cast<spatial_ipc::Shared*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(spatial_ipc::Shared)));
        CloseHandle(map);
    }
    setTreeVolume(root, BROWSER_VOLUME);
    capture->Start();
    render->Start();
    hostLog("spatial audio: on (tablet place %s)", shared ? "shared by EchoCam" : "not available yet");

    constexpr int HISTORY = 64;  // samples kept for the ear delay
    float history[HISTORY] = {};
    int at = 0;
    Placement now;  // smoothed
    float lowLeft = 0, lowRight = 0;
    ULONGLONG lastVolume = GetTickCount64();
    std::vector<float> out;
    while (!stop_) {
        Sleep(5);
        ULONGLONG tick = GetTickCount64();
        if (tick - lastVolume > 1000) { setTreeVolume(root, BROWSER_VOLUME); lastVolume = tick; }  // new sessions start at full volume
        if (!shared) {
            if (HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, spatial_ipc::NAME)) {
                shared = static_cast<spatial_ipc::Shared*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(spatial_ipc::Shared)));
                CloseHandle(map);
            }
        }
        // The newest place: plain sound when EchoCam has not seen the tablet for half a second
        Placement target;
        bool placed = false;
        if (shared && shared->magic == spatial_ipc::MAGIC) {
            LONG before = shared->seq;
            float p[3] = {shared->position[0], shared->position[1], shared->position[2]};
            LONGLONG time = shared->time;
            bool poster = shared->kind == spatial_ipc::Poster;
            if (!(before & 1) && before == shared->seq && LONGLONG(tick) - time < 500) { target = place(p, poster); placed = true; }
        }
        UINT32 packet = 0;
        while (SUCCEEDED(captureClient->GetNextPacketSize(&packet)) && packet) {
            BYTE* data;
            UINT32 frames;
            DWORD flags;
            if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            const float* in = reinterpret_cast<const float*>(data);
            bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            out.resize(size_t(frames) * 2);
            for (UINT32 f = 0; f < frames; f++) {
                float l = silent ? 0 : in[f * 2] * BOOST, r = silent ? 0 : in[f * 2 + 1] * BOOST;
                if (!placed) { out[f * 2] = l; out[f * 2 + 1] = r; continue; }
                // gains glide (no clicks); the delays and muffle follow per packet
                now.left += (target.left - now.left) * .002f;
                now.right += (target.right - now.right) * .002f;
                now.muffle += (target.muffle - now.muffle) * .002f;
                float mono = (l + r) * .5f;
                history[at] = mono;
                float leftIn = history[(at - target.delayLeft + HISTORY) % HISTORY], rightIn = history[(at - target.delayRight + HISTORY) % HISTORY];
                at = (at + 1) % HISTORY;
                lowLeft += (leftIn - lowLeft) * now.muffle;
                lowRight += (rightIn - lowRight) * now.muffle;
                // keep a little of the original stereo so music does not collapse to one point
                out[f * 2] = lowLeft * now.left * .85f + l * .15f * now.left;
                out[f * 2 + 1] = lowRight * now.right * .85f + r * .15f * now.right;
            }
            captureClient->ReleaseBuffer(frames);
            UINT32 padding = 0;
            if (FAILED(render->GetCurrentPadding(&padding))) continue;
            UINT32 room = renderFrames - padding, write = std::min(room, frames);  // too far behind: drop the rest
            BYTE* target_buffer;
            if (write && SUCCEEDED(renderClient->GetBuffer(write, &target_buffer))) {
                memcpy(target_buffer, out.data(), size_t(write) * 8);
                renderClient->ReleaseBuffer(write, 0);
            }
        }
    }
    capture->Stop();
    render->Stop();
    setTreeVolume(root, 1.f);
    hostLog("spatial audio: off");
    CoUninitialize();
}

void restoreBrowserVolume(DWORD root) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    setTreeVolume(root, 1.f);
    if (SUCCEEDED(com)) CoUninitialize();
}
