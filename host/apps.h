#pragma once
#include "host.h"
#include <winsock2.h>

enum class AppId { BalatroSteam, BalatroPortmaster, RetroArch, Doom };

struct AppInfo {
    AppId id;
    std::wstring title, subtitle, missing;  // missing: why it cannot start (empty = ready)
    uint32_t accent;
};
std::vector<AppInfo> listApps(const Config& config);
bool isRetro(AppId id);

// RetroPad button ids (libretro RETRO_DEVICE_ID_JOYPAD_*).
enum Pad { PadB = 0, PadY, PadSelect, PadStart, PadUp, PadDown, PadLeft, PadRight, PadA, PadX, PadL, PadR, PadCount = 12 };

class Session {
public:
    Session(const Config& config);
    ~Session();
    bool launch(AppId id);
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
    void refreshPad();

private:
    void send(int port, const void* data, int size);
    const Config& config_;
    AppId id_ = AppId::BalatroSteam;
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    HWND window_ = nullptr;
    uint64_t started_ = 0, quitAt_ = 0, lastPadSend_ = 0, lastPlace_ = 0;
    bool padState_[PadCount] = {};
    SOCKET udp_ = INVALID_SOCKET;
    WindowCapture capture_;
};
