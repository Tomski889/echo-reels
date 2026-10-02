// ProcessAudioMeter (voice.h): polls the peak meters of every active playback device's audio sessions that belong to the
// watched process or its descendants, about 30 times a second, on its own COM thread.
#include "voice.h"
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// root and all processes it started (directly or not)
std::set<DWORD> processTree(DWORD root) {
    std::set<DWORD> tree{root};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return tree;
    std::multimap<DWORD, DWORD> children;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        children.emplace(entry.th32ParentProcessID, entry.th32ProcessID);
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

// The peak meters of the tree's sessions on every active playback device
std::vector<ComPtr<IAudioMeterInformation>> sessionMeters(DWORD root) {
    std::vector<ComPtr<IAudioMeterInformation>> meters;
    std::set<DWORD> tree = processTree(root);
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDeviceCollection> outputs;
    UINT count = 0;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) ||
        FAILED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &outputs)) || FAILED(outputs->GetCount(&count)))
        return meters;
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
            ComPtr<IAudioMeterInformation> meter;
            DWORD pid = 0;
            if (SUCCEEDED(sessions->GetSession(i, &control)) && SUCCEEDED(control.As(&control2)) &&
                SUCCEEDED(control2->GetProcessId(&pid)) && tree.count(pid) && SUCCEEDED(control.As(&meter)))
                meters.push_back(meter);
        }
    }
    return meters;
}

}  // namespace

int setCaptureMute(DWORD pid, bool mute) {
    int changed = 0;
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDeviceCollection> inputs;
    UINT count = 0;
    if (!pid || FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) ||
        FAILED(devices->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &inputs)) || FAILED(inputs->GetCount(&count)))
        return 0;
    for (UINT d = 0; d < count; d++) {
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> sessions;
        int n = 0;
        if (FAILED(inputs->Item(d, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(manager.GetAddressOf()))) ||
            FAILED(manager->GetSessionEnumerator(&sessions)) || FAILED(sessions->GetCount(&n)))
            continue;
        for (int i = 0; i < n; i++) {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> control2;
            ComPtr<ISimpleAudioVolume> volume;
            DWORD owner = 0;
            if (SUCCEEDED(sessions->GetSession(i, &control)) && SUCCEEDED(control.As(&control2)) && SUCCEEDED(control2->GetProcessId(&owner)) &&
                owner == pid && SUCCEEDED(control.As(&volume)) && SUCCEEDED(volume->SetMute(mute, nullptr)))
                changed++;
        }
    }
    return changed;
}

int setCaptureMuteTree(DWORD rootPid, bool mute) {
    int changed = 0;
    if (rootPid)
        for (DWORD pid : processTree(rootPid)) changed += setCaptureMute(pid, mute);
    return changed;
}

int boostProcessTree(DWORD rootPid) {
    int changed = 0;
    if (!rootPid) return 0;
    for (DWORD pid : processTree(rootPid)) {
        HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) continue;
        PROCESS_POWER_THROTTLING_STATE throttling{PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0};
        bool ok = SetProcessInformation(process, ProcessPowerThrottling, &throttling, sizeof(throttling)) != 0;
        if (GetPriorityClass(process) != ABOVE_NORMAL_PRIORITY_CLASS) ok = SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS) && ok;
        CloseHandle(process);
        changed += ok;
    }
    return changed;
}

DWORD findProcess(const wchar_t* exe) {
    DWORD found = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry))
        if (_wcsicmp(entry.szExeFile, exe) == 0) found = entry.th32ProcessID;
    CloseHandle(snapshot);
    return found;
}

ProcessAudioMeter::ProcessAudioMeter() : thread_(&ProcessAudioMeter::run, this) {}

ProcessAudioMeter::~ProcessAudioMeter() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void ProcessAudioMeter::watch(DWORD rootPid) {
    root_ = rootPid;
    if (!rootPid) level_ = 0;
}

void ProcessAudioMeter::run() {
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<ComPtr<IAudioMeterInformation>> meters;
    DWORD watched = 0;
    ULONGLONG lastScan = 0;
    while (!stop_) {
        Sleep(33);
        DWORD root = root_;
        if (!root) { meters.clear(); watched = 0; level_ = 0; continue; }
        // Sessions come and go (Chrome starts its audio process when sound begins): look again every second
        ULONGLONG now = GetTickCount64();
        if (root != watched || now - lastScan > 1000) {
            meters = sessionMeters(root);
            watched = root;
            lastScan = now;
        }
        float peak = 0;
        for (auto& m : meters) {
            float v = 0;
            if (SUCCEEDED(m->GetPeakValue(&v))) peak = (std::max)(peak, v);
        }
        level_ = peak;
    }
    meters.clear();
    CoUninitialize();
}
