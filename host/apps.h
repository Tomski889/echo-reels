#pragma once
#include "host.h"
#include "media.h"
#include <winsock2.h>

enum class AppId { BalatroSteam, BalatroPortmaster, RetroArch, Doom, Movies, Plex };

struct AppInfo {
    AppId id;
    std::wstring title, subtitle, missing;  // missing: why it cannot start (empty = ready)
    uint32_t accent;
};
std::vector<AppInfo> listApps(const Config& config);
bool isRetro(AppId id);
bool isVideo(AppId id);

// RetroPad button ids (libretro RETRO_DEVICE_ID_JOYPAD_*).
enum Pad { PadB = 0, PadY, PadSelect, PadStart, PadUp, PadDown, PadLeft, PadRight, PadA, PadX, PadL, PadR, PadL2, PadR2, PadCount };

class Session {
public:
    Session(const Config& config);
    ~Session();
    bool launch(AppId id, const std::wstring& target = L"", double startSeconds = 0);  // target: video for Movies/Plex
    void poll();                 // finds and places the window, starts capture
    bool alive() const;
    bool hasWindow() const { return window_ != nullptr; }
    AppId id() const { return id_; }
    Rect contentRect() const;    // where the app goes on the tablet screen
    WindowCapture& capture() { return capture_; }
    void quit();                 // polite request, then forced after a grace period
    void kill();
    uint64_t age() const { return GetTickCount64() - started_; }

    // Input routing (UDP to 127.0.0.1 only).
    void pointer(const char* kind, float u, float v);  // Balatro bridge: "down", "move", "up"
    void pad(Pad button, bool down);
    void command(const char* text);                    // RetroArch network command
    void analog(float x, float y);                     // left stick, -1..1
    MpvControl& mpv() { return mpv_; }

private:
    void send(int port, const void* data, int size);
    const Config& config_;
    AppId id_ = AppId::BalatroSteam;
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    HWND window_ = nullptr;
    uint64_t started_ = 0, quitAt_ = 0, lastPlace_ = 0;
    bool padState_[PadCount] = {};
    int16_t stick_[2] = {};
    MpvControl mpv_;
    SOCKET udp_ = INVALID_SOCKET;
    WindowCapture capture_;
};
