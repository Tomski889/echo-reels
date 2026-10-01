// CAMERA tile helpers: EchoCam settings, the game's window, photos (see camera.h).
#include "camera.h"
#include <shlobj.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <algorithm>

namespace {

std::wstring pluginsDir(const Config& config) {
    return config.dir.substr(0, config.dir.find_last_of(L"\\/"));  // ...\plugins\EchoArcade -> ...\plugins
}

std::wstring echoCamIni(const Config& config) { return pluginsDir(config) + L"\\EchoCam.ini"; }

struct Biggest { DWORD pid; HWND hwnd; long area; };

BOOL CALLBACK biggestWindow(HWND hwnd, LPARAM param) {
    auto b = reinterpret_cast<Biggest*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != b->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    long area = (r.right - r.left) * (r.bottom - r.top);
    if (area > b->area) { b->area = area; b->hwnd = hwnd; }
    return TRUE;
}

}  // namespace

std::wstring echoCamDll(const Config& config) { return pluginsDir(config) + L"\\EchoCam.dll"; }

CameraOptions loadCameraOptions(const Config& config) {
    std::wstring ini = echoCamIni(config);
    wchar_t hand[16];
    GetPrivateProfileStringW(L"EchoCam", L"Hand", L"left", hand, 16, ini.c_str());
    CameraOptions o;
    bool right = _wcsicmp(hand, L"right") == 0;
    o.source = GetPrivateProfileIntW(L"EchoCam", L"OnPanel", 1, ini.c_str()) != 0 && !right ? CameraOptions::Tablet
               : right ? CameraOptions::RightHand : CameraOptions::LeftHand;
    o.selfie = GetPrivateProfileIntW(L"EchoCam", L"HandYaw", 0, ini.c_str()) == 180;
    wchar_t offset[64];
    GetPrivateProfileStringW(L"EchoCam", L"HandOffset", L"0 0.1 0.25", offset, 64, ini.c_str());
    float x, y, z;
    if (swscanf_s(offset, L"%f %f %f", &x, &y, &z) == 3) o.reach = std::clamp(z, CAMERA_REACH_MIN, CAMERA_REACH_MAX);
    o.frozen = false;  // a new camera session starts live
    return o;
}

void saveCameraOptions(const Config& config, const CameraOptions& options) {
    std::wstring ini = echoCamIni(config);
    // EchoCam: the tablet camera is OnPanel with the panel's (left) hand
    WritePrivateProfileStringW(L"EchoCam", L"Hand", options.source == CameraOptions::RightHand ? L"right" : L"left", ini.c_str());
    WritePrivateProfileStringW(L"EchoCam", L"OnPanel", options.source == CameraOptions::Tablet ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"EchoCam", L"HandYaw", options.selfie ? L"180" : L"0", ini.c_str());
    wchar_t offset[64];
    swprintf_s(offset, L"0 0.1 %.2f", options.reach);  // controller space: +z is forward (tested)
    WritePrivateProfileStringW(L"EchoCam", L"HandOffset", offset, ini.c_str());
    WritePrivateProfileStringW(L"EchoCam", L"Freeze", options.frozen ? L"1" : L"0", ini.c_str());
    hostLog("camera: %s, %s, reach %.2f m%s", options.source == CameraOptions::Tablet ? "tablet" : options.source == CameraOptions::RightHand ? "right hand" : "left hand",
            options.selfie ? "selfie" : "front",
            options.reach, options.frozen ? ", frozen" : "");
}

void setHandCamera(const Config& config, bool on) {
    if (GetFileAttributesW(echoCamDll(config).c_str()) == INVALID_FILE_ATTRIBUTES) return;
    WritePrivateProfileStringW(L"EchoCam", L"Mode", on ? L"hand" : L"head", echoCamIni(config).c_str());
    if (!on) WritePrivateProfileStringW(L"EchoCam", L"Freeze", L"0", echoCamIni(config).c_str());
    hostLog("camera: %s view", on ? "hand" : "head");
}

HWND findEchoWindow() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
    PROCESSENTRY32W entry{sizeof(entry)};
    Biggest best{0, nullptr, 0};
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"echovr.exe") != 0) continue;
        Biggest b{entry.th32ProcessID, nullptr, 0};
        EnumWindows(biggestWindow, reinterpret_cast<LPARAM>(&b));
        if (b.area > best.area) best = b;
    }
    CloseHandle(snapshot);
    return best.hwnd;
}

std::wstring savePhoto(const std::vector<uint8_t>& bgra, int w, int h) {
    if (w <= 0 || h <= 0 || bgra.size() < size_t(w) * h * 4) return L"";
    PWSTR pictures = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures))) return L"";
    std::wstring dir = std::wstring(pictures) + L"\\Echo";
    CoTaskMemFree(pictures);
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"Echo_%04d%02d%02d_%02d%02d%02d_%03d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::wstring path = dir + L"\\" + name;

    // 24-bit BGR (the captured alpha is not meaningful)
    std::vector<uint8_t> bgr(size_t(w) * h * 3);
    for (size_t i = 0, n = size_t(w) * h; i < n; i++) {
        bgr[i * 3] = bgra[i * 4];
        bgr[i * 3 + 1] = bgra[i * 4 + 1];
        bgr[i * 3 + 2] = bgra[i * 4 + 2];
    }
    // Runs on a worker thread: COM for WIC
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComScope { HRESULT hr; ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); } } scope{com};
    try {
        auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
        winrt::com_ptr<IWICStream> stream;
        winrt::check_hresult(factory->CreateStream(stream.put()));
        winrt::check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
        winrt::com_ptr<IWICBitmapEncoder> encoder;
        winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
        winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
        winrt::check_hresult(frame->Initialize(nullptr));
        winrt::check_hresult(frame->SetSize(UINT(w), UINT(h)));
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        winrt::check_hresult(frame->SetPixelFormat(&format));
        if (format != GUID_WICPixelFormat24bppBGR) throw winrt::hresult_error(E_FAIL);
        winrt::check_hresult(frame->WritePixels(UINT(h), UINT(w * 3), UINT(bgr.size()), bgr.data()));
        winrt::check_hresult(frame->Commit());
        winrt::check_hresult(encoder->Commit());
    } catch (...) {
        hostLog("camera: could not save %ls", path.c_str());
        DeleteFileW(path.c_str());
        return L"";
    }
    hostLog("camera: saved %ls (%dx%d)", path.c_str(), w, h);
    return name;
}
