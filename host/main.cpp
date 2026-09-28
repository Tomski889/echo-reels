// ArcadeHost.exe -- started by EchoArcade.dll when the PLAY tab is first opened.
// Owns the tablet UI (launcher, browsers, touch controls) and the running app;
// exits when Echo VR goes away.
#include "apps.h"
#include <winrt/base.h>
#include <algorithm>
#include <functional>
#include <future>
#include <map>

namespace {

// ---------------------------------------------------------------- layout
// Cells are 32 x ~32 px, so every control spans at least 2 x 2 cells.
struct PadButton { Pad pad; Rect rect; const wchar_t* label; };
const PadButton PAD[] = {
    {PadL2, {16, 16, 96, 72}, L"L2"}, {PadL, {104, 16, 184, 72}, L"L"},
    {PadUp, {67, 201, 133, 267}, L"\x25B2"}, {PadDown, {67, 333, 133, 399}, L"\x25BC"},
    {PadLeft, {1, 267, 67, 333}, L"\x25C0"}, {PadRight, {133, 267, 199, 333}, L"\x25B6"},
    {PadSelect, {16, 470, 100, 526}, L"SELECT"},
    {PadR, {840, 16, 920, 72}, L"R"}, {PadR2, {928, 16, 1008, 72}, L"R2"},
    {PadX, {891, 201, 957, 267}, L"X"}, {PadB, {891, 333, 957, 399}, L"B"},
    {PadY, {825, 267, 891, 333}, L"Y"}, {PadA, {957, 267, 1023, 333}, L"A"},
    {PadStart, {844, 470, 928, 526}, L"START"},
};
const Rect STICK_AREA{1, 201, 199, 399}, STICK_TOGGLE{16, 412, 184, 458};
const Rect RETRO_HOME{108, 470, 184, 526}, RETRO_MENU{936, 470, 1012, 526};
const Rect BALATRO_HOME{0, 0, 64, 64};
const Rect RESUME_BUTTON{312, 190, 712, 270}, QUIT_BUTTON{312, 300, 712, 380};
const Rect BACK_BUTTON{12, 10, 150, 66};
const Rect PAGE_UP{916, 84, 1012, 298}, PAGE_DOWN{916, 306, 1012, 520};
constexpr int ROWS_PER_PAGE = 6, ROW_Y = 84, ROW_H = 72;
const Rect SEEK_BAR{20, 448, 1004, 476};
constexpr int PLAYER_BUTTONS = 10;
const wchar_t* const PLAYER_LABELS[PLAYER_BUTTONS] = {L"-30s", L"-10s", L"PLAY", L"+10s", L"+30s", L"VOL -", L"VOL +", L"SUBS", L"AUDIO", L"CLOSE"};

Rect grow(Rect r, int by) { return {r.x0 - by, r.y0 - by, r.x1 + by, r.y1 + by}; }
Rect rowRect(int i) { return {12, ROW_Y + i * ROW_H, 904, ROW_Y + i * ROW_H + ROW_H - 6}; }
Rect playerButton(int i) { return {12 + i * 101, 484, 12 + i * 101 + 95, 566}; }

std::wstring clock(double s) {
    if (s < 0) s = 0;
    int t = int(s), h = t / 3600, m = t / 60 % 60, sec = t % 60;
    wchar_t b[32];
    if (h) swprintf_s(b, L"%d:%02d:%02d", h, m, sec); else swprintf_s(b, L"%d:%02d", m, sec);
    return b;
}

enum class Mode { Launcher, Browse, PlexLink, Running, Menu };

struct Entry {
    std::wstring label, sub;
    std::function<void()> open;
};
struct Screen {
    std::wstring title;
    std::vector<Entry> entries;
    int page = 0;
};

class Host {
public:
    Host(arcade::Shared* shared, bool standalone)
        : shared_(shared), standalone_(standalone), config_(loadConfig()), session_(config_) {
        readPos_ = shared_->touchWrite;
        apps_ = listApps(config_);
        loadSettings();
        shared_->hostPid = LONG(GetCurrentProcessId());
    }

    ~Host() {
        for (auto& f : workers_) f.wait();
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
            runPending();
            session_.poll();
            if ((mode_ == Mode::Running || mode_ == Mode::Menu) && !session_.alive()) appClosed();
            if (mode_ == Mode::Running && !session_.hasWindow() && session_.age() > 30000) {
                message_ = L"It did not open a window in 30 s (see host.log).";
                session_.kill();
                appClosed();
            }
            if (mode_ == Mode::PlexLink) pollPlexLink(now);
            reportPlexProgress(now);
            bool visible = shared_->pageVisible != 0 || standalone_;
            if (now - lastPublish >= (visible ? 16u : 200u)) {
                render();
                publish();
                lastPublish = now;
            }
            Sleep(4);
        }
        if (session_.alive()) {
            finishPlex();
            session_.quit();
            Sleep(1500);
        }
        session_.kill();
    }

private:
    // ------------------------------------------------ background work
    // Network and disk work runs on worker threads; results are applied on the UI thread.
    void async(std::function<std::function<void()>()> job, const std::wstring& what) {
        loading_ = what;
        spawn([this, job] {
            auto apply = job();
            std::lock_guard<std::mutex> lock(pendingLock_);
            pending_.push_back(apply);
        });
    }
    void spawn(std::function<void()> work) {
        workers_.erase(std::remove_if(workers_.begin(), workers_.end(), [](auto& f) {
            return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }), workers_.end());
        workers_.push_back(std::async(std::launch::async, std::move(work)));
    }
    void runPending() {
        std::vector<std::function<void()>> ready;
        {
            std::lock_guard<std::mutex> lock(pendingLock_);
            ready.swap(pending_);
        }
        for (auto& f : ready) { loading_.clear(); f(); }
    }

    // ------------------------------------------------ navigation
    void toLauncher() {
        mode_ = Mode::Launcher;
        screens_.clear();
        resetInput();
        apps_ = listApps(config_);
    }
    void resetInput() {
        held_.clear();
        gameCells_.clear();
        pointerDown_ = false;
        pressed_ = -1;
    }
    void push(Screen s) {
        screens_.push_back(std::move(s));
        mode_ = Mode::Browse;
        resetInput();
    }
    void back() {
        if (!screens_.empty()) screens_.pop_back();
        if (screens_.empty()) toLauncher();
        resetInput();
    }
    void appClosed() {
        finishPlex();
        if (message_.empty()) message_ = current_.title + L" closed.";
        if (isVideo(session_.id()) && !screens_.empty()) { mode_ = Mode::Browse; resetInput(); }
        else toLauncher();
    }

    void launch(const AppInfo& app) {
        pressed_ = -1;
        if (!app.missing.empty()) { message_ = app.title + L": " + app.missing; return; }
        message_.clear();
        current_ = app;
        if (app.id == AppId::Movies) { openMovies(); return; }
        if (app.id == AppId::Plex) { openPlex(); return; }
        if (!session_.launch(app.id)) { message_ = L"Could not start " + app.title + L" (see host.log)."; return; }
        mode_ = Mode::Running;
        resetInput();
    }

    void play(const std::wstring& target, const std::wstring& title, double start = 0) {
        message_.clear();
        if (!session_.launch(current_.id, target, start)) { message_ = L"Could not start mpv (see host.log)."; return; }
        playing_ = title;
        mode_ = Mode::Running;
        controlsUntil_ = GetTickCount64() + 4000;
        resetInput();
    }

    // ------------------------------------------------ movies
    void openMovies() {
        std::vector<std::wstring> folders;
        for (auto& f : config_.movies) if (GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) folders.push_back(f);
        if (folders.empty()) { message_ = L"No movie folders found - see movies= in arcade.ini"; return; }
        if (folders.size() == 1) { openFolder(folders[0], L"MOVIES"); return; }
        Screen s{L"MOVIES"};
        for (auto& f : folders) s.entries.push_back({f.substr(f.find_last_of(L"\\/") + 1), f, [this, f] { openFolder(f, f.substr(f.find_last_of(L"\\/") + 1)); }});
        push(s);
    }
    void openFolder(const std::wstring& folder, const std::wstring& title) {
        async([this, folder, title] {
            auto files = listVideos(folder);
            return std::function<void()>([this, files, title] {
                Screen s{title};
                for (auto& f : files) {
                    if (f.folder) s.entries.push_back({L"\x25B8  " + f.name, L"folder", [this, f] { openFolder(f.path, f.name); }});
                    else s.entries.push_back({f.name, L"", [this, f] { play(f.path, f.name); }});
                }
                if (s.entries.empty()) s.entries.push_back({L"No videos here", L"Add files to this folder, then reopen", nullptr});
                push(s);
            });
        }, L"Reading " + title + L"...");
    }

    // ------------------------------------------------ plex
    void openPlex() {
        if (!plex_.linked()) {
            async([this] {
                std::wstring code;
                std::string pin;
                bool ok = plex_.startLink(code, pin);
                return std::function<void()>([this, ok, code, pin] {
                    if (!ok) { message_ = L"Could not reach plex.tv"; return; }
                    linkCode_ = code; linkPin_ = pin; linkStarted_ = GetTickCount64();
                    mode_ = Mode::PlexLink;
                    resetInput();
                });
            }, L"Contacting plex.tv...");
            return;
        }
        async([this] {
            std::wstring error;
            bool ok = plex_.serverReady() || plex_.findServer(error);
            auto libs = ok ? plex_.libraries() : std::vector<PlexItem>{};
            return std::function<void()>([this, ok, error, libs] {
                if (!ok) { message_ = L"Plex: " + error; return; }
                Screen s{L"PLEX - " + plex_.serverName()};
                for (auto& l : libs) s.entries.push_back({l.title, l.subtitle, [this, l] { openPlexFolder(l); }});
                s.entries.push_back({L"Unlink Plex", L"Sign this tablet out of your Plex account", [this] { plex_.unlink(); toLauncher(); message_ = L"Plex unlinked."; }});
                push(s);
            });
        }, L"Finding your Plex server...");
    }
    void openPlexFolder(const PlexItem& folder) {
        async([this, folder] {
            auto items = plex_.children(folder.key);
            return std::function<void()>([this, folder, items] {
                Screen s{folder.title};
                for (auto& it : items) {
                    if (it.playable()) s.entries.push_back({it.title, it.subtitle, [this, it] { playPlex(it); }});
                    else s.entries.push_back({L"\x25B8  " + it.title, it.subtitle, [this, it] { openPlexFolder(it); }});
                }
                if (s.entries.empty()) s.entries.push_back({L"Nothing here", L"", nullptr});
                push(s);
            });
        }, L"Loading " + folder.title + L"...");
    }
    void playPlex(const PlexItem& item) {
        plexItem_ = item;
        plexPlaying_ = true;
        lastTimeline_ = 0;
        play(plex_.streamUrl(item), item.title, item.resumeSeconds > 30 ? item.resumeSeconds : 0);
    }
    void pollPlexLink(uint64_t now) {
        if (!loading_.empty() || now - lastLinkPoll_ < 2000) return;
        lastLinkPoll_ = now;
        if (now - linkStarted_ > 10 * 60 * 1000) { message_ = L"The Plex code expired."; toLauncher(); return; }
        std::string pin = linkPin_;
        spawn([this, pin] {
            bool linked = plex_.pollLink(pin);
            std::lock_guard<std::mutex> lock(pendingLock_);
            if (linked) pending_.push_back([this] { if (mode_ == Mode::PlexLink) { mode_ = Mode::Launcher; openPlex(); } });
        });
    }
    // Keep Plex's resume point and watched state in sync.
    void reportPlexProgress(uint64_t now) {
        if (!plexPlaying_ || mode_ != Mode::Running || now - lastTimeline_ < 10000 || !session_.mpv().connected()) return;
        lastTimeline_ = now;
        auto item = plexItem_;
        double pos = session_.mpv().position();
        bool paused = session_.mpv().paused();
        spawn([this, item, pos, paused] { plex_.timeline(item, paused ? "paused" : "playing", pos); });
    }
    void finishPlex() {
        if (!plexPlaying_) return;
        plexPlaying_ = false;
        auto item = plexItem_;
        double pos = session_.mpv().position();
        spawn([this, item, pos] { plex_.timeline(item, "stopped", pos); });
    }

    // ------------------------------------------------ touches
    void readTouches() {
        LONG write = shared_->touchWrite;
        if (write - readPos_ > LONG(arcade::TOUCH_RING)) readPos_ = write - LONG(arcade::TOUCH_RING);
        while (readPos_ != write) {
            auto& e = shared_->touches[uint32_t(readPos_) % arcade::TOUCH_RING];
            if (e.seq != uint32_t(readPos_) + 1) break;  // still being written
            MemoryBarrier();
            if (shared_->cols && e.cell < shared_->cols * shared_->rows)
                onTouch(touchFromCell(int(e.cell), e.kind == arcade::TouchDown, int(shared_->cols), int(shared_->rows)));
            readPos_++;
        }
    }

    // ------------------------------------------------ SETTINGS tab
    // Saved in echo_tweaks.ini beside the host, which the plugin reads when Echo starts.
    struct Setting { const wchar_t* label; const wchar_t* section; const wchar_t* key; float min, max, step, value; };
    std::wstring tweaksPath() const {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring p = path;
        return p.substr(0, p.find_last_of(L"\\/")) + L"\\echo_tweaks.ini";
    }
    void loadSettings() {
        settings_ = {{L"TABLET SIZE", L"tablet", L"scale", .75f, 2.f, .05f, 1.f},
                     {L"VIEW WIDTH (FOV)", L"fov", L"x", .8f, 2.f, .05f, 1.f},
                     {L"VIEW HEIGHT (FOV)", L"fov", L"y", .8f, 2.f, .05f, 1.f}};
        for (auto& s : settings_) {
            wchar_t buf[32];
            GetPrivateProfileStringW(s.section, s.key, L"1.0", buf, 32, tweaksPath().c_str());
            s.value = std::clamp(float(_wtof(buf)), s.min, s.max);
        }
        shared_->tabletScale = LONG(settings_[0].value * 1000);
    }
    void setSetting(size_t i, float v) {
        auto& s = settings_[i];
        v = std::clamp(std::round(v / s.step) * s.step, s.min, s.max);
        if (std::fabs(v - s.value) < .001f) return;
        s.value = v;
        wchar_t buf[16];
        swprintf(buf, 16, L"%.2f", v);
        WritePrivateProfileStringW(s.section, s.key, buf, tweaksPath().c_str());
        if (i == 0) shared_->tabletScale = LONG(v * 1000);
    }
    static int settingY(size_t i) { return 96 + int(i) * 150; }
    static Rect settingMinus(size_t i) { return {20, settingY(i) + 46, 110, settingY(i) + 116}; }
    static Rect settingPlus(size_t i) { return {914, settingY(i) + 46, 1004, settingY(i) + 116}; }
    static Rect settingTrack(size_t i) { return {140, settingY(i) + 66, 884, settingY(i) + 96}; }
    bool settingsMode() const { return shared_->pageMode == 1; }

    void settingsTouch(const Touch& t) {
        for (size_t i = 0; i < settings_.size(); i++) {
            auto& s = settings_[i];
            if (tapped(t, 600 + int(i) * 2, settingMinus(i))) { setSetting(i, s.value - s.step); return; }
            if (tapped(t, 601 + int(i) * 2, settingPlus(i))) { setSetting(i, s.value + s.step); return; }
            Rect track = settingTrack(i);
            if (t.down && grow(track, 26).contains(t.x, t.y)) {  // tap or slide along the bar
                float f = std::clamp(float(t.x - track.x0) / track.w(), 0.f, 1.f);
                setSetting(i, s.min + f * (s.max - s.min));
                return;
            }
        }
        if (!t.down) pressed_ = -1;
    }

    void drawSettings() {
        canvas_.clear(rgb(14, 16, 24));
        header(L"SETTINGS", false);
        for (size_t i = 0; i < settings_.size(); i++) {
            auto& s = settings_[i];
            int y = settingY(i);
            wchar_t value[16];
            swprintf(value, 16, L"%.2fx", s.value);
            canvas_.text({24, y, 700, y + 44}, s.label, 26, rgb(235, 235, 240), true, 0);
            canvas_.text({700, y, 1004, y + 44}, value, 26, rgb(90, 200, 255), true, 2);
            button(settingMinus(i), L"\x2212", pressed_ == 600 + int(i) * 2, rgb(44, 50, 72), 36);
            button(settingPlus(i), L"+", pressed_ == 601 + int(i) * 2, rgb(44, 50, 72), 36);
            Rect track = settingTrack(i);
            canvas_.fill(track, rgb(40, 44, 62));
            int fill = track.x0 + int(track.w() * (s.value - s.min) / (s.max - s.min));
            canvas_.fill({track.x0, track.y0, fill, track.y1}, rgb(70, 150, 190));
            canvas_.fill({fill - 14, track.y0 - 16, fill + 14, track.y1 + 16}, rgb(235, 240, 250));
        }
        canvas_.text({24, 516, 1004, 572}, L"Saved instantly. View (FOV) applies the next time Echo VR starts. "
                     L"Tablet size is saved now; resizing arrives in a plugin update.", 17, rgb(150, 160, 180), false, 0);
    }

    // Stop whatever the game was being fed when the tab changes under a held finger.
    void releaseGameInput() {
        for (auto& b : PAD) session_.pad(b.pad, false);
        session_.analog(0, 0);
        if (pointerDown_) session_.pointer("up", lastU_, lastV_);
        resetInput();
    }

    void onTouch(const Touch& t) {
        if (t.down) held_[t.cell] = t; else held_.erase(t.cell);
        if (!loading_.empty() && !settingsMode()) return;
        if (settingsMode()) { settingsTouch(t); return; }
        switch (mode_) {
            case Mode::Launcher: launcherTouch(t); break;
            case Mode::Browse: browseTouch(t); break;
            case Mode::PlexLink: if (!t.down && BACK_BUTTON.contains(t.x, t.y)) toLauncher(); break;
            case Mode::Menu: menuTouch(t); break;
            case Mode::Running:
                if (isVideo(session_.id())) playerTouch(t);
                else if (isRetro(session_.id())) retroTouch(t);
                else balatroTouch(t);
                break;
        }
    }

    // A tap = press and release on the same control.
    bool tapped(const Touch& t, int index, Rect r) {
        if (!r.contains(t.x, t.y)) return false;
        if (t.down) { pressed_ = index; return false; }
        bool hit = pressed_ == index;
        pressed_ = -1;
        return hit;
    }

    void launcherTouch(const Touch& t) {
        for (size_t i = 0; i < apps_.size(); i++)
            if (tapped(t, int(i), tileRect(i))) { launch(apps_[i]); return; }
        if (!t.down) pressed_ = -1;
    }

    void browseTouch(const Touch& t) {
        auto& s = screens_.back();
        int pages = std::max(1, (int(s.entries.size()) + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
        if (tapped(t, 100, BACK_BUTTON)) { back(); return; }
        if (tapped(t, 101, PAGE_UP)) { s.page = std::max(0, s.page - 1); return; }
        if (tapped(t, 102, PAGE_DOWN)) { s.page = std::min(pages - 1, s.page + 1); return; }
        for (int i = 0; i < ROWS_PER_PAGE; i++) {
            size_t index = size_t(s.page * ROWS_PER_PAGE + i);
            if (index >= s.entries.size()) break;
            if (tapped(t, i, rowRect(i))) {
                auto open = s.entries[index].open;  // may push/pop screens
                if (open) open();
                return;
            }
        }
        if (!t.down) pressed_ = -1;
    }

    void openMenu() {
        for (auto& b : PAD) session_.pad(b.pad, false);
        session_.analog(0, 0);
        if (pointerDown_) session_.pointer("up", lastU_, lastV_);
        pointerDown_ = false;
        gameCells_.clear();
        mode_ = Mode::Menu;
    }

    void menuTouch(const Touch& t) {
        if (tapped(t, 200, RESUME_BUTTON)) mode_ = Mode::Running;
        else if (tapped(t, 201, QUIT_BUTTON)) { session_.quit(); message_ = L"Closing " + current_.title + L"..."; }
    }

    void playerTouch(const Touch& t) {
        uint64_t now = GetTickCount64();
        bool shown = now < controlsUntil_ || session_.mpv().paused();  // paused: controls stay up
        if (!shown) { if (!t.down) controlsUntil_ = now + 5000; return; }  // first tap just reveals the controls
        controlsUntil_ = now + 5000;
        auto& mpv = session_.mpv();
        if (!t.down && SEEK_BAR.contains(t.x, t.y) && pressed_ < 0) {
            char cmd[96];
            snprintf(cmd, sizeof(cmd), R"(["seek",%.2f,"absolute-percent"])", 100.0 * (t.x - SEEK_BAR.x0) / SEEK_BAR.w());
            mpv.command(cmd);
            return;
        }
        for (int i = 0; i < PLAYER_BUTTONS; i++) {
            if (!tapped(t, 300 + i, playerButton(i))) continue;
            switch (i) {
                case 0: mpv.command(R"(["seek",-30])"); break;
                case 1: mpv.command(R"(["seek",-10])"); break;
                case 2: mpv.command(R"(["cycle","pause"])"); break;
                case 3: mpv.command(R"(["seek",10])"); break;
                case 4: mpv.command(R"(["seek",30])"); break;
                case 5: mpv.command(R"(["add","volume",-10])"); break;
                case 6: mpv.command(R"(["add","volume",10])"); break;
                case 7: mpv.command(R"(["cycle","sub"])"); break;
                case 8: mpv.command(R"(["cycle","audio"])"); break;
                case 9: finishPlex(); session_.quit(); break;
            }
            return;
        }
        if (!t.down && t.y < SEEK_BAR.y0 - 8 && pressed_ < 0) controlsUntil_ = 0;  // tap the picture to hide
        if (!t.down) pressed_ = -1;
    }

    void retroTouch(const Touch& t) {
        if (tapped(t, 400, RETRO_HOME)) { openMenu(); return; }
        if (tapped(t, 401, RETRO_MENU)) { session_.command("MENU_TOGGLE"); return; }
        if (tapped(t, 402, STICK_TOGGLE)) {
            stickMode_ = !stickMode_;
            for (auto p : {PadUp, PadDown, PadLeft, PadRight}) session_.pad(p, false);
            session_.analog(0, 0);
            return;
        }
        for (auto& b : PAD) {
            bool dpad = b.pad == PadUp || b.pad == PadDown || b.pad == PadLeft || b.pad == PadRight;
            if (dpad && stickMode_) continue;
            bool on = false;
            for (auto& [cell, h] : held_) on |= grow(b.rect, 6).contains(h.x, h.y);
            session_.pad(b.pad, on);
        }
        if (stickMode_) {
            // Newest finger in the stick area sets the direction; release recentres.
            float x = 0, y = 0;
            for (auto& [cell, h] : held_) {
                if (!STICK_AREA.contains(h.x, h.y)) continue;
                float cx = (STICK_AREA.x0 + STICK_AREA.x1) / 2.f, cy = (STICK_AREA.y0 + STICK_AREA.y1) / 2.f;
                x = (h.x - cx) / (STICK_AREA.w() / 2.f - 16);
                y = (h.y - cy) / (STICK_AREA.h() / 2.f - 16);
            }
            float len = std::sqrt(x * x + y * y);
            if (len > 1) { x /= len; y /= len; }
            stick_[0] = x; stick_[1] = y;
            session_.analog(x, y);
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

    // ------------------------------------------------ drawing
    Rect tileRect(size_t i) const {
        int col = int(i % 3), row = int(i / 3);
        return {20 + col * 332, 88 + row * 222, 20 + col * 332 + 320, 88 + row * 222 + 210};
    }

    void header(const std::wstring& title, bool backButton) {
        canvas_.fill({0, 0, SCREEN_W, 76}, rgb(22, 26, 40));
        if (backButton) {
            button(BACK_BUTTON, L"\x25C0  BACK", pressed_ == 100, rgb(50, 58, 84));
            canvas_.text({170, 0, 1004, 76}, title, 32, rgb(90, 200, 255), true, 0);
        } else {
            canvas_.text({24, 0, 600, 76}, title, 40, rgb(90, 200, 255), true, 0);
        }
    }

    void button(Rect r, const std::wstring& label, bool on, uint32_t color, int size = 0) {
        canvas_.fill(r, on ? rgb(250, 250, 250) : color);
        canvas_.frame(r, rgb(70, 76, 100), 2);
        canvas_.text(r, label, size ? size : (r.h() > 60 ? 28 : 20), on ? rgb(20, 20, 20) : rgb(235, 235, 240));
    }

    void messageBar() {
        if (message_.empty()) return;
        canvas_.fill({0, SCREEN_H - 40, SCREEN_W, SCREEN_H}, rgb(60, 30, 30));
        canvas_.text({20, SCREEN_H - 40, SCREEN_W - 20, SCREEN_H}, message_, 20, rgb(255, 220, 200));
    }

    void loadingOverlay() {
        if (loading_.empty()) return;
        canvas_.blend({0, 0, SCREEN_W, SCREEN_H}, rgb(0, 0, 0), 170);
        unsigned dots = unsigned(GetTickCount64() / 400 % 4);
        canvas_.text({0, 240, SCREEN_W, 330}, loading_ + std::wstring(dots, L' '), 32, rgb(255, 255, 255));
    }

    void drawLauncher() {
        canvas_.clear(rgb(14, 16, 24));
        header(L"ECHO ARCADE", false);
        canvas_.text({600, 0, 1000, 76}, L"tap a tile", 22, rgb(150, 160, 180), false, 2);
        for (size_t i = 0; i < apps_.size(); i++) {
            auto& a = apps_[i];
            Rect r = tileRect(i);
            bool on = pressed_ == int(i);
            canvas_.fill(r, on ? rgb(48, 56, 80) : rgb(30, 34, 50));
            canvas_.fill({r.x0, r.y0, r.x0 + 12, r.y1}, a.accent);
            canvas_.frame(r, on ? rgb(255, 255, 255) : rgb(50, 58, 84), on ? 4 : 2);
            canvas_.text({r.x0 + 30, r.y0 + 26, r.x1 - 12, r.y0 + 96}, a.title, 40, rgb(245, 245, 250), true, 0);
            canvas_.text({r.x0 + 30, r.y0 + 96, r.x1 - 12, r.y0 + 134}, a.subtitle, 21, rgb(180, 190, 210), false, 0);
            if (!a.missing.empty())
                canvas_.text({r.x0 + 30, r.y0 + 140, r.x1 - 12, r.y0 + 180}, a.missing, 17, rgb(255, 170, 90), false, 0);
        }
        messageBar();
    }

    void drawBrowse() {
        auto& s = screens_.back();
        canvas_.clear(rgb(14, 16, 24));
        header(s.title, true);
        int pages = std::max(1, (int(s.entries.size()) + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
        for (int i = 0; i < ROWS_PER_PAGE; i++) {
            size_t index = size_t(s.page * ROWS_PER_PAGE + i);
            if (index >= s.entries.size()) break;
            auto& e = s.entries[index];
            Rect r = rowRect(i);
            bool on = pressed_ == i;
            canvas_.fill(r, on ? rgb(56, 64, 92) : rgb(30, 34, 50));
            canvas_.fill({r.x0, r.y0, r.x0 + 8, r.y1}, current_.accent);
            if (e.sub.empty()) canvas_.text({r.x0 + 24, r.y0, r.x1 - 12, r.y1}, e.label, 26, rgb(240, 240, 245), true, 0);
            else {
                canvas_.text({r.x0 + 24, r.y0 + 4, r.x1 - 12, r.y0 + 40}, e.label, 25, rgb(240, 240, 245), true, 0);
                canvas_.text({r.x0 + 24, r.y0 + 38, r.x1 - 12, r.y1 - 2}, e.sub, 18, rgb(160, 170, 190), false, 0);
            }
        }
        button(PAGE_UP, L"\x25B2", pressed_ == 101, s.page > 0 ? rgb(50, 58, 84) : rgb(28, 30, 40), 36);
        button(PAGE_DOWN, L"\x25BC", pressed_ == 102, s.page < pages - 1 ? rgb(50, 58, 84) : rgb(28, 30, 40), 36);
        canvas_.text({12, 528, 904, 574}, L"page " + std::to_wstring(s.page + 1) + L" of " + std::to_wstring(pages) + L"   -   " +
                     std::to_wstring(s.entries.size()) + L" items", 18, rgb(130, 140, 160), false, 0);
        messageBar();
    }

    void drawPlexLink() {
        canvas_.clear(rgb(14, 16, 24));
        header(L"LINK PLEX", true);
        canvas_.text({0, 110, SCREEN_W, 160}, L"On your phone or PC, go to", 28, rgb(200, 205, 220), false);
        canvas_.text({0, 160, SCREEN_W, 230}, L"plex.tv/link", 52, rgb(229, 160, 13));
        canvas_.text({0, 240, SCREEN_W, 280}, L"and enter this code:", 28, rgb(200, 205, 220), false);
        canvas_.fill({312, 300, 712, 420}, rgb(30, 34, 50));
        canvas_.text({312, 300, 712, 420}, linkCode_, 80, rgb(255, 255, 255));
        canvas_.text({0, 450, SCREEN_W, 500}, L"This screen continues by itself once you approve.", 22, rgb(150, 160, 180), false);
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
            std::wstring name = isVideo(session_.id()) ? playing_ : current_.title;
            canvas_.text(content, L"Starting " + name + std::wstring(dots, L'.'), 32, rgb(220, 220, 230));
        }
        if (isVideo(session_.id())) drawPlayerControls();
        else if (isRetro(session_.id())) drawPad();
        else {
            canvas_.blend(BALATRO_HOME, rgb(0, 0, 0), 150);
            canvas_.text(BALATRO_HOME, L"\x2261", 40, rgb(255, 255, 255));
        }
    }

    void drawPlayerControls() {
        auto& mpv = session_.mpv();
        if ((GetTickCount64() >= controlsUntil_ && !mpv.paused()) || mode_ != Mode::Running) return;
        canvas_.blend({0, 0, SCREEN_W, 64}, rgb(0, 0, 0), 170);
        canvas_.text({20, 0, 1004, 64}, playing_, 24, rgb(255, 255, 255), true, 0);
        canvas_.blend({0, 408, SCREEN_W, SCREEN_H}, rgb(0, 0, 0), 190);
        double pos = mpv.position(), dur = mpv.duration();
        canvas_.text({20, 410, 500, 446}, clock(pos) + L" / " + clock(dur), 20, rgb(230, 230, 235), false, 0);
        canvas_.text({500, 410, 1004, 446}, L"volume " + std::to_wstring(int(mpv.volume())) + L"%", 20, rgb(180, 185, 200), false, 2);
        canvas_.fill(SEEK_BAR, rgb(60, 64, 80));
        if (dur > 0) canvas_.fill({SEEK_BAR.x0, SEEK_BAR.y0, SEEK_BAR.x0 + int(SEEK_BAR.w() * std::clamp(pos / dur, 0.0, 1.0)), SEEK_BAR.y1}, current_.accent);
        for (int i = 0; i < PLAYER_BUTTONS; i++) {
            std::wstring label = i == 2 ? (mpv.paused() ? L"PLAY" : L"PAUSE") : PLAYER_LABELS[i];
            button(playerButton(i), label, pressed_ == 300 + i, i == 9 ? rgb(120, 40, 40) : rgb(44, 50, 72), 20);
        }
    }

    void drawPad() {
        canvas_.fill({0, 0, 200, SCREEN_H}, rgb(20, 22, 30));
        canvas_.fill({824, 0, SCREEN_W, SCREEN_H}, rgb(20, 22, 30));
        bool live = mode_ == Mode::Running;
        for (auto& b : PAD) {
            bool dpad = b.pad == PadUp || b.pad == PadDown || b.pad == PadLeft || b.pad == PadRight;
            if (dpad && stickMode_) continue;
            bool on = false;
            for (auto& [cell, h] : held_) on |= grow(b.rect, 6).contains(h.x, h.y);
            button(b.rect, b.label, on && live, rgb(44, 50, 72));
        }
        if (stickMode_) {
            canvas_.fill(STICK_AREA, rgb(34, 38, 54));
            canvas_.frame(STICK_AREA, rgb(70, 76, 100), 2);
            int cx = (STICK_AREA.x0 + STICK_AREA.x1) / 2, cy = (STICK_AREA.y0 + STICK_AREA.y1) / 2;
            int kx = cx + int(stick_[0] * (STICK_AREA.w() / 2 - 30)), ky = cy + int(stick_[1] * (STICK_AREA.h() / 2 - 30));
            canvas_.fill({cx - 2, STICK_AREA.y0 + 8, cx + 2, STICK_AREA.y1 - 8}, rgb(60, 66, 90));
            canvas_.fill({STICK_AREA.x0 + 8, cy - 2, STICK_AREA.x1 - 8, cy + 2}, rgb(60, 66, 90));
            canvas_.fill({kx - 28, ky - 28, kx + 28, ky + 28}, rgb(200, 205, 220));
        }
        button(STICK_TOGGLE, stickMode_ ? L"D-PAD" : L"STICK", pressed_ == 402, rgb(40, 70, 90), 20);
        button(RETRO_HOME, L"HOME", pressed_ == 400, rgb(90, 40, 40));
        button(RETRO_MENU, L"MENU", pressed_ == 401, rgb(60, 50, 110));
    }

    void drawMenu() {
        drawApp();
        canvas_.blend({0, 0, SCREEN_W, SCREEN_H}, rgb(0, 0, 0), 190);
        canvas_.text({0, 90, SCREEN_W, 160}, current_.title + L" - " + current_.subtitle, 34, rgb(255, 255, 255));
        button(RESUME_BUTTON, L"RESUME", pressed_ == 200, rgb(40, 110, 60), 32);
        button(QUIT_BUTTON, L"QUIT TO LAUNCHER", pressed_ == 201, rgb(130, 40, 40), 32);
        if (!message_.empty()) canvas_.text({0, 420, SCREEN_W, 470}, message_, 22, rgb(255, 220, 200));
    }

    void render() {
        bool settings = settingsMode();
        if (settings != lastSettings_) { if (session_.alive()) releaseGameInput(); else resetInput(); lastSettings_ = settings; }
        if (settings) { drawSettings(); canvas_.opaque(); return; }
        switch (mode_) {
            case Mode::Launcher: drawLauncher(); break;
            case Mode::Browse: drawBrowse(); break;
            case Mode::PlexLink: drawPlexLink(); break;
            case Mode::Running: drawApp(); break;
            case Mode::Menu: drawMenu(); break;
        }
        loadingOverlay();
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
    Plex plex_;
    std::vector<AppInfo> apps_;
    AppInfo current_{};
    Mode mode_ = Mode::Launcher;
    std::vector<Screen> screens_;
    LONG readPos_ = 0;
    std::map<int, Touch> held_;
    std::vector<Touch> gameCells_;
    int pressed_ = -1;
    bool pointerDown_ = false, stickMode_ = false;
    float lastU_ = 0, lastV_ = 0, stick_[2] = {};
    std::wstring message_, loading_, playing_, linkCode_;
    std::string linkPin_;
    uint64_t linkStarted_ = 0, lastLinkPoll_ = 0, controlsUntil_ = 0, lastTimeline_ = 0;
    PlexItem plexItem_;
    bool plexPlaying_ = false;
    std::mutex pendingLock_;
    std::vector<std::function<void()>> pending_;
    std::vector<std::future<void>> workers_;
    std::vector<uint8_t> pixels_;
    int capW_ = 0, capH_ = 0;
    uint64_t captureSerial_ = 0;
    Rect fit_{};
    std::vector<Setting> settings_;
    bool lastSettings_ = false;
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
