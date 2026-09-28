// ArcadeHost.exe -- started by EchoArcade.dll when the ARCADE tab is first opened.
// Owns the launcher UI and the running app; exits when Echo VR goes away.
#include "apps.h"
#include <winrt/base.h>
#include <algorithm>
#include <map>

namespace {

struct PadButton { Pad pad; Rect rect; const wchar_t* label; };
// Left/right panels beside the 624x574 RetroArch view. Cells are 32x32 px.
const PadButton PAD[] = {
    {PadL, {20, 20, 180, 76}, L"L"},
    {PadUp, {67, 201, 133, 267}, L"\x25B2"}, {PadDown, {67, 333, 133, 399}, L"\x25BC"},
    {PadLeft, {1, 267, 67, 333}, L"\x25C0"}, {PadRight, {133, 267, 199, 333}, L"\x25B6"},
    {PadSelect, {16, 470, 100, 526}, L"SELECT"},
    {PadR, {844, 20, 1004, 76}, L"R"},
    {PadX, {891, 201, 957, 267}, L"X"}, {PadB, {891, 333, 957, 399}, L"B"},
    {PadY, {825, 267, 891, 333}, L"Y"}, {PadA, {957, 267, 1023, 333}, L"A"},
    {PadStart, {844, 470, 928, 526}, L"START"},
};
const Rect RETRO_HOME{108, 470, 184, 526}, RETRO_MENU{936, 470, 1012, 526};
const Rect BALATRO_HOME{0, 0, 64, 64};
const Rect RESUME_BUTTON{312, 190, 712, 270}, QUIT_BUTTON{312, 300, 712, 380};

Rect grow(Rect r, int by) { return {r.x0 - by, r.y0 - by, r.x1 + by, r.y1 + by}; }

enum class Mode { Launcher, Running, Menu };

class Host {
public:
    Host(arcade::Shared* shared, bool standalone)
        : shared_(shared), standalone_(standalone), config_(loadConfig()), session_(config_) {
        readPos_ = shared_->touchWrite;
        apps_ = listApps(config_);
        shared_->hostPid = LONG(GetCurrentProcessId());
        status(L"");
    }

    void run() {
        uint64_t lastPublish = 0, seenGame = 0;
        while (true) {
            uint64_t now = GetTickCount64();
            shared_->hostHeartbeat = LONG64(now);
            if (arcade::alive(shared_->gameHeartbeat, now)) seenGame = now;
            if (!standalone_ && now - seenGame > (seenGame ? 15000u : 120000u)) {
                hostLog("Echo VR is gone; shutting down");
                break;
            }
            readTouches();
            session_.poll();
            if (mode_ != Mode::Launcher && !session_.alive()) {
                if (message_.empty()) message_ = L"The game closed.";
                toLauncher();
            }
            if (mode_ == Mode::Running && !session_.hasWindow() && session_.age() > 30000) {
                message_ = L"The game did not open a window in 30 s (see host.log).";
                session_.kill();
                toLauncher();
            }
            if (mode_ == Mode::Running && isRetro(session_.id())) session_.refreshPad();
            bool visible = shared_->pageVisible != 0 || standalone_;
            if (now - lastPublish >= (visible ? 16u : 200u)) {
                render();
                publish();
                lastPublish = now;
            }
            Sleep(4);
        }
        session_.quit();
        Sleep(1500);
        session_.kill();
    }

private:
    void status(const std::wstring& s) {
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, shared_->status, int(sizeof(shared_->status)) - 1, nullptr, nullptr);
    }

    void toLauncher() {
        mode_ = Mode::Launcher;
        held_.clear();
        gameCells_.clear();
        pointerDown_ = false;
        apps_ = listApps(config_);
    }

    // ---- touches ----
    void readTouches() {
        LONG write = shared_->touchWrite;
        if (write - readPos_ > LONG(arcade::TOUCH_RING)) readPos_ = write - LONG(arcade::TOUCH_RING);
        while (readPos_ != write) {
            auto& e = shared_->touches[uint32_t(readPos_) % arcade::TOUCH_RING];
            if (e.seq != uint32_t(readPos_) + 1) break;  // still being written
            MemoryBarrier();
            if (e.cell < shared_->cols * shared_->rows && shared_->cols)
                onTouch(touchFromCell(int(e.cell), e.kind == arcade::TouchDown, int(shared_->cols), int(shared_->rows)));
            readPos_++;
        }
    }

    void onTouch(const Touch& t) {
        if (t.down) held_[t.cell] = t; else held_.erase(t.cell);
        switch (mode_) {
            case Mode::Launcher: launcherTouch(t); break;
            case Mode::Menu: menuTouch(t); break;
            case Mode::Running: isRetro(session_.id()) ? retroTouch(t) : balatroTouch(t); break;
        }
    }

    void launcherTouch(const Touch& t) {
        for (size_t i = 0; i < apps_.size(); i++) {
            if (!tileRect(i).contains(t.x, t.y)) continue;
            if (t.down) { pressedTile_ = int(i); return; }
            if (pressedTile_ == int(i)) launch(apps_[i]);
        }
        if (!t.down) pressedTile_ = -1;
    }

    void launch(const AppInfo& app) {
        pressedTile_ = -1;
        if (!app.missing.empty()) { message_ = app.title + L": " + app.missing; return; }
        message_.clear();
        if (!session_.launch(app.id)) { message_ = L"Could not start " + app.title + L" (see host.log)."; return; }
        current_ = app;
        mode_ = Mode::Running;
        held_.clear();
    }

    void openMenu() {
        for (auto& b : PAD) session_.pad(b.pad, false);
        if (pointerDown_) session_.pointer("up", lastU_, lastV_);
        pointerDown_ = false;
        gameCells_.clear();
        mode_ = Mode::Menu;
    }

    void menuTouch(const Touch& t) {
        if (t.down) return;  // act on release so the opening tap can't also choose
        if (RESUME_BUTTON.contains(t.x, t.y)) mode_ = Mode::Running;
        else if (QUIT_BUTTON.contains(t.x, t.y)) { session_.quit(); message_ = L"Closing " + current_.title + L"..."; }
    }

    void retroTouch(const Touch& t) {
        if (!t.down && RETRO_HOME.contains(t.x, t.y)) { openMenu(); return; }
        if (!t.down && RETRO_MENU.contains(t.x, t.y)) { session_.command("MENU_TOGGLE"); return; }
        for (auto& b : PAD) {
            bool on = false;
            for (auto& [cell, h] : held_) on |= grow(b.rect, 6).contains(h.x, h.y);
            session_.pad(b.pad, on);
        }
    }

    bool toApp(int x, int y, float& u, float& v) {
        if (!fit_.w() || !fit_.h()) return false;
        u = std::clamp((x - fit_.x0 + .5f) / fit_.w(), 0.f, 1.f);
        v = std::clamp((y - fit_.y0 + .5f) / fit_.h(), 0.f, 1.f);
        return true;
    }

    void balatroTouch(const Touch& t) {
        if (BALATRO_HOME.contains(t.x, t.y)) { if (!t.down) openMenu(); return; }
        if (t.down) gameCells_.push_back(t);
        else gameCells_.erase(std::remove_if(gameCells_.begin(), gameCells_.end(), [&](const Touch& g) { return g.cell == t.cell; }), gameCells_.end());
        float u, v;
        if (!gameCells_.empty()) {
            // The newest held cell is the finger's position; sliding across cells drags.
            if (!toApp(gameCells_.back().x, gameCells_.back().y, u, v)) return;
            session_.pointer("move", u, v);
            if (!pointerDown_) session_.pointer("down", u, v);
            pointerDown_ = true;
            lastU_ = u; lastV_ = v;
        } else if (pointerDown_) {
            session_.pointer("up", lastU_, lastV_);
            pointerDown_ = false;
        }
    }

    // ---- drawing ----
    Rect tileRect(size_t i) const {
        int col = int(i % 2), row = int(i / 2);
        return {32 + col * 490, 96 + row * 226, 32 + col * 490 + 470, 96 + row * 226 + 206};
    }

    void drawLauncher() {
        canvas_.clear(rgb(14, 16, 24));
        canvas_.fill({0, 0, SCREEN_W, 76}, rgb(22, 26, 40));
        canvas_.text({32, 0, 600, 76}, L"ECHO ARCADE", 40, rgb(90, 200, 255), true, 0);
        canvas_.text({600, 0, 992, 76}, L"tap a game to play", 22, rgb(150, 160, 180), false, 2);
        for (size_t i = 0; i < apps_.size(); i++) {
            auto& a = apps_[i];
            Rect r = tileRect(i);
            bool pressed = pressedTile_ == int(i);
            canvas_.fill(r, pressed ? rgb(48, 56, 80) : rgb(30, 34, 50));
            canvas_.fill({r.x0, r.y0, r.x0 + 14, r.y1}, a.accent);
            canvas_.frame(r, pressed ? rgb(255, 255, 255) : rgb(50, 58, 84), pressed ? 4 : 2);
            canvas_.text({r.x0 + 40, r.y0 + 30, r.x1 - 20, r.y0 + 110}, a.title, 50, rgb(245, 245, 250), true, 0);
            canvas_.text({r.x0 + 40, r.y0 + 110, r.x1 - 20, r.y0 + 150}, a.subtitle, 26, rgb(180, 190, 210), false, 0);
            if (!a.missing.empty())
                canvas_.text({r.x0 + 40, r.y0 + 150, r.x1 - 20, r.y0 + 190}, a.missing, 19, rgb(255, 170, 90), false, 0);
        }
        if (!message_.empty()) {
            canvas_.fill({0, SCREEN_H - 42, SCREEN_W, SCREEN_H}, rgb(60, 30, 30));
            canvas_.text({20, SCREEN_H - 42, SCREEN_W - 20, SCREEN_H}, message_, 20, rgb(255, 220, 200));
        }
    }

    void drawApp() {
        Rect content = session_.contentRect();
        canvas_.clear(rgb(10, 10, 14));
        uint64_t before = captureSerial_;
        session_.capture().latest(pixels_, capW_, capH_, captureSerial_);
        if (captureSerial_ != before && captureSerial_ == 1) hostLog("first captured frame %dx%d", capW_, capH_);
        if (session_.hasWindow() && capW_ > 0) {
            canvas_.blit(pixels_.data(), capW_, capH_, capW_ * 4, content);
            float scale = std::min(float(content.w()) / capW_, float(content.h()) / capH_);
            int w = int(capW_ * scale), h = int(capH_ * scale);
            fit_ = {content.x0 + (content.w() - w) / 2, content.y0 + (content.h() - h) / 2, 0, 0};
            fit_.x1 = fit_.x0 + w; fit_.y1 = fit_.y0 + h;
        } else {
            unsigned dots = unsigned(session_.age() / 400 % 4);
            canvas_.text(content, L"Starting " + current_.title + std::wstring(dots, L'.'), 36, rgb(220, 220, 230));
        }
        if (isRetro(session_.id())) drawPad();
        else {
            canvas_.blend(BALATRO_HOME, rgb(0, 0, 0), 150);
            canvas_.text(BALATRO_HOME, L"\x2261", 40, rgb(255, 255, 255));
        }
    }

    void drawPad() {
        canvas_.fill({0, 0, 200, SCREEN_H}, rgb(20, 22, 30));
        canvas_.fill({824, 0, SCREEN_W, SCREEN_H}, rgb(20, 22, 30));
        auto button = [&](Rect r, const wchar_t* label, bool on, uint32_t color) {
            canvas_.fill(r, on ? rgb(250, 250, 250) : color);
            canvas_.frame(r, rgb(70, 76, 100), 2);
            canvas_.text(r, label, r.h() > 60 ? 30 : 20, on ? rgb(20, 20, 20) : rgb(235, 235, 240));
        };
        for (auto& b : PAD) {
            bool on = false;
            for (auto& [cell, h] : held_) on |= grow(b.rect, 6).contains(h.x, h.y);
            button(b.rect, b.label, on && mode_ == Mode::Running, rgb(44, 50, 72));
        }
        button(RETRO_HOME, L"HOME", false, rgb(90, 40, 40));
        button(RETRO_MENU, L"MENU", false, rgb(60, 50, 110));
    }

    void drawMenu() {
        drawApp();
        canvas_.blend({0, 0, SCREEN_W, SCREEN_H}, rgb(0, 0, 0), 190);
        canvas_.text({0, 90, SCREEN_W, 160}, current_.title + L" - " + current_.subtitle, 34, rgb(255, 255, 255));
        for (auto [r, label, color] : {std::tuple{RESUME_BUTTON, L"RESUME", rgb(40, 110, 60)},
                                       std::tuple{QUIT_BUTTON, L"QUIT TO LAUNCHER", rgb(130, 40, 40)}}) {
            canvas_.fill(r, color);
            canvas_.frame(r, rgb(255, 255, 255), 2);
            canvas_.text(r, label, 32, rgb(255, 255, 255));
        }
        if (!message_.empty()) canvas_.text({0, 420, SCREEN_W, 470}, message_, 22, rgb(255, 220, 200));
    }

    void render() {
        switch (mode_) {
            case Mode::Launcher: drawLauncher(); break;
            case Mode::Running: drawApp(); break;
            case Mode::Menu: drawMenu(); break;
        }
        canvas_.opaque();
    }

    void publish() {
        uint32_t index = arcade::writableFrame(shared_);
        memcpy(shared_->frames[index], canvas_.pixels(), arcade::FRAME_BYTES);
        arcade::publishFrame(shared_, index);
    }

    arcade::Shared* shared_;
    bool standalone_;
    Config config_;
    Session session_;
    Canvas canvas_;
    std::vector<AppInfo> apps_;
    AppInfo current_{};
    Mode mode_ = Mode::Launcher;
    LONG readPos_ = 0;
    std::map<int, Touch> held_;
    std::vector<Touch> gameCells_;
    int pressedTile_ = -1;
    bool pointerDown_ = false;
    float lastU_ = 0, lastV_ = 0;
    std::wstring message_;
    std::vector<uint8_t> pixels_;
    int capW_ = 0, capH_ = 0;
    uint64_t captureSerial_ = 0;
    Rect fit_{};
};

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\EchoArcade.Host");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    bool standalone = wcsstr(cmdLine, L"--standalone") != nullptr;
    HANDLE mapping = nullptr;
    arcade::Shared* shared = arcade::open(&mapping);
    if (!shared) { hostLog("shared memory unavailable (%lu)", GetLastError()); return 1; }
    hostLog("ArcadeHost started pid=%lu standalone=%d", GetCurrentProcessId(), int(standalone));
    {
        Host host(shared, standalone);
        host.run();
    }
    shared->hostHeartbeat = 0;
    shared->hostPid = 0;
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    CloseHandle(single);
    return 0;
}
