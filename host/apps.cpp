#include "apps.h"
#include <ws2tcpip.h>
#include <cstdio>
#include <thread>

namespace {

bool exists(const std::wstring& path) { return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring folderOf(const std::wstring& file) { return file.substr(0, file.find_last_of(L"\\/")); }

// Window size each app renders at; matches its rectangle on the tablet screen.
constexpr int RETRO_PANEL = 200;
constexpr int RETRO_W = SCREEN_W - 2 * RETRO_PANEL, RETRO_H = SCREEN_H;

struct Found { DWORD pid; HWND hwnd; };
BOOL CALLBACK findWindow(HWND hwnd, LPARAM param) {
    auto f = reinterpret_cast<Found*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != f->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    if (r.right - r.left < 100 || r.bottom - r.top < 100) return TRUE;
    f->hwnd = hwnd;
    return FALSE;
}

void writeRetroConfig(const Config& c, const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") || !f) return;
    fprintf(f,
        "network_cmd_enable = \"true\"\nnetwork_cmd_port = \"%d\"\n"
        "network_remote_enable = \"true\"\nnetwork_remote_base_port = \"%d\"\nnetwork_remote_enable_user_p1 = \"true\"\n"
        "pause_nonactive = \"false\"\nvideo_fullscreen = \"false\"\nvideo_windowed_fullscreen = \"false\"\n"
        "video_window_save_positions = \"false\"\nui_menubar_enable = \"false\"\nui_companion_start_on_boot = \"false\"\n"
        "video_window_show_decorations = \"false\"\nmenu_mouse_enable = \"false\"\nmenu_pointer_enable = \"false\"\n"
        "input_autodetect_enable = \"true\"\nmenu_show_advanced_settings = \"false\"\n"
        "quit_press_twice = \"false\"\nconfirm_quit = \"false\"\n"
        "rgui_browser_directory = \"%ls\"\n",
        c.retroCmdPort, c.retroPadPort, c.roms.c_str());
    fclose(f);
}

}  // namespace

bool isRetro(AppId id) { return id == AppId::RetroArch || id == AppId::Doom; }

std::vector<AppInfo> listApps(const Config& c) {
    std::vector<AppInfo> apps;
    auto balatro = [&](AppId id, const wchar_t* sub, const std::wstring& dir, uint32_t accent) {
        std::wstring missing;
        if (!exists(c.love)) missing = L"LOVE runtime missing - rerun the installer";
        else if (!exists(dir + L"\\main.lua")) missing = L"Install Balatro on Steam, then rerun prepare";
        apps.push_back({id, L"BALATRO", sub, missing, accent});
    };
    balatro(AppId::BalatroSteam, L"Steam original", c.balatroSteam, rgb(222, 64, 64));
    balatro(AppId::BalatroPortmaster, L"PortMaster handheld UI", c.balatroPortmaster, rgb(64, 132, 230));
    apps.push_back({AppId::RetroArch, L"RETROARCH", L"All cores - full menu",
                    exists(c.retroarch) ? L"" : L"RetroArch missing - rerun the installer", rgb(150, 90, 220)});
    std::wstring doomMissing = !exists(c.retroarch) ? L"RetroArch missing" :
                               !exists(c.doomCore) ? L"prboom core missing" : !exists(c.doomWad) ? L"doom1.wad missing" : L"";
    apps.push_back({AppId::Doom, L"DOOM", L"Shareware via RetroArch", doomMissing, rgb(200, 120, 40)});
    return apps;
}

Session::Session(const Config& config) : config_(config) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    udp_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

Session::~Session() {
    kill();
    if (udp_ != INVALID_SOCKET) closesocket(udp_);
}

Rect Session::contentRect() const {
    if (isRetro(id_)) return {RETRO_PANEL, 0, RETRO_PANEL + RETRO_W, RETRO_H};
    return {0, 0, SCREEN_W, SCREEN_H};
}

bool Session::launch(AppId id) {
    kill();
    id_ = id;
    std::wstring exe, args, cwd;
    auto setEnv = [](const wchar_t* k, const std::wstring& v) { SetEnvironmentVariableW(k, v.empty() ? nullptr : v.c_str()); };
    setEnv(L"ECHO_ARCADE_PORT", L"");
    if (id == AppId::BalatroSteam || id == AppId::BalatroPortmaster) {
        bool pm = id == AppId::BalatroPortmaster;
        exe = config_.love;
        cwd = pm ? config_.balatroPortmaster : config_.balatroSteam;
        // --fused makes LOVE use %APPDATA%\Balatro, i.e. the Steam game's own saves.
        args = (pm ? L"\"" : L"--fused \"") + cwd + L"\"";
        setEnv(L"ECHO_ARCADE_PORT", std::to_wstring(config_.balatroPort));
        setEnv(L"ECHO_ARCADE_STEAM", pm ? L"0" : L"1");
        setEnv(L"ECHO_ARCADE_WIDTH", std::to_wstring(SCREEN_W));
        setEnv(L"ECHO_ARCADE_HEIGHT", std::to_wstring(SCREEN_H));
        setEnv(L"BALATRO_PM_WINDOWS_WINDOWED", L"1");
        setEnv(L"BALATRO_PM_WINDOWS_WIDTH", std::to_wstring(SCREEN_W));
        setEnv(L"BALATRO_PM_WINDOWS_HEIGHT", std::to_wstring(SCREEN_H));
        setEnv(L"BALATRO_PM_PERF_OPTIMIZATIONS", L"1");
        setEnv(L"BALATRO_PM_FPS_CAP", L"60");
        setEnv(L"BALATRO_PM_SKIP_RUMBLE", L"1");
    } else {
        exe = config_.retroarch;
        cwd = folderOf(exe);
        std::wstring cfg = config_.dir + L"\\retroarch_echo.cfg";
        writeRetroConfig(config_, cfg);
        args = L"--appendconfig \"" + cfg + L"\"";
        if (id == AppId::Doom) args += L" -L \"" + config_.doomCore + L"\" \"" + config_.doomWad + L"\"";
    }
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi)) {
        hostLog("launch failed (%lu): %ls", GetLastError(), cmd.c_str());
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    started_ = GetTickCount64();
    quitAt_ = 0;
    std::fill(std::begin(padState_), std::end(padState_), false);
    hostLog("launched pid=%lu: %ls", pid_, cmd.c_str());
    return true;
}

bool Session::alive() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

void Session::poll() {
    if (!process_) return;
    if (!alive()) {
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        hostLog("app exited with code %lu", code);
        kill();
        return;
    }
    if (quitAt_ && GetTickCount64() - quitAt_ > 4000) { hostLog("app did not quit in time; terminating"); kill(); return; }
    if (window_ && !IsWindow(window_)) { window_ = nullptr; capture_.stop(); }
    Rect r = contentRect();
    int x = config_.windowMode == L"offscreen" ? -8000 : 0;
    if (window_) {
        // Apps resize themselves (RetroArch does on content load); keep nudging it back.
        uint64_t now = GetTickCount64();
        RECT wr;
        if (now - lastPlace_ > 1000 && GetWindowRect(window_, &wr) && (wr.right - wr.left != r.w() || wr.bottom - wr.top != r.h())) {
            SetWindowPos(window_, HWND_BOTTOM, x, 0, r.w(), r.h(), SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            lastPlace_ = now;
        }
        return;
    }
    Found f{pid_, nullptr};
    EnumWindows(findWindow, reinterpret_cast<LPARAM>(&f));
    if (!f.hwnd) return;
    window_ = f.hwnd;
    lastPlace_ = GetTickCount64();
    // Borderless, sized to its tablet rectangle, parked where it won't get in the way.
    // Changing another process's window sends it messages synchronously, and a busy
    // app (RetroArch loading a core) would stall us, so do it on a throwaway thread.
    HWND hwnd = window_;
    std::thread([hwnd, x, w = r.w(), h = r.h()] {
        LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        SetWindowLongW(hwnd, GWL_STYLE, (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MAXIMIZEBOX | WS_MINIMIZEBOX)) | WS_POPUP);
        LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        SetWindowLongW(hwnd, GWL_EXSTYLE, (ex & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW);
        SetWindowPos(hwnd, HWND_BOTTOM, x, 0, w, h, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }).detach();
    hostLog("window %p found; placing %dx%d (%ls mode)", window_, r.w(), r.h(), config_.windowMode.c_str());
    capture_.start(window_);
}

void Session::quit() {
    if (!process_ || quitAt_) return;
    quitAt_ = GetTickCount64();
    if (isRetro(id_)) command("QUIT");
    else {
        const char msg[] = "quit";
        send(config_.balatroPort, msg, int(sizeof(msg) - 1));
    }
}

void Session::kill() {
    capture_.stop();
    window_ = nullptr;
    if (process_) {
        if (alive()) TerminateProcess(process_, 0);
        CloseHandle(process_);
    }
    process_ = nullptr;
    pid_ = 0;
}

void Session::send(int port, const void* data, int size) {
    if (udp_ == INVALID_SOCKET) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(u_short(port));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(udp_, static_cast<const char*>(data), size, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

void Session::pointer(const char* kind, float u, float v) {
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "%s %.4f %.4f", kind, u, v);
    send(config_.balatroPort, msg, n);
}

void Session::command(const char* text) {
    send(config_.retroCmdPort, text, int(strlen(text)));
    hostLog("retroarch command: %s", text);
}

// RetroArch network RetroPad packet (input_remote.c: struct remote_message).
struct RemoteMessage { int32_t port, device, index, id; uint16_t state; };

void Session::pad(Pad button, bool down) {
    if (padState_[button] == down) return;
    padState_[button] = down;
    RemoteMessage msg{0, 1 /* RETRO_DEVICE_JOYPAD */, 0, int32_t(button), uint16_t(down)};
    send(config_.retroPadPort, &msg, int(sizeof(msg)));
}

void Session::refreshPad() {
    uint64_t now = GetTickCount64();
    if (now - lastPadSend_ < 500) return;
    lastPadSend_ = now;
    for (int b = 0; b < PadCount; b++) {
        RemoteMessage msg{0, 1, 0, b, uint16_t(padState_[b])};
        send(config_.retroPadPort, &msg, int(sizeof(msg)));
    }
}
