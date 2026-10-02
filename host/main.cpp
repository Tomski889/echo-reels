// ArcadeHost.exe -- started by EchoArcade.dll when the PLAY tab is first opened.
// Owns the tablet UI (launcher, browsers, touch controls) and the running app;
// exits when Echo VR goes away.
#include "apps.h"
#include "camera.h"
#include "panel_ipc.h"
#include "voice.h"
#include "spatial.h"
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
const Rect ORB_BUTTON{SCREEN_W - 64, 0, SCREEN_W, 64};  // CHATGPT: back to the orb from the chat page
const Rect MIC_BUTTON{SCREEN_W - 330, 0, SCREEN_W - 80, 64};  // CHATGPT: whether ChatGPT hears your microphone
const Rect ORB_MIC_BUTTON{SCREEN_W / 2 - 240, SCREEN_H - 104, SCREEN_W / 2 + 240, SCREEN_H - 16};  // big: easy to hit, hard to miss
const Rect ORB_CHAT_BUTTON{SCREEN_W - 200, 12, SCREEN_W - 16, 76};  // the chat page
// REELS / TIKTOK: the sites start their videos muted; this unmutes every video as it plays
const char* const UNMUTE_SCRIPT = R"JS((() => {
  if (window.__echoUnmute) return;
  window.__echoUnmute = true;
  const unmute = () => document.querySelectorAll('video').forEach(v => { if (v.muted) v.muted = false; if (v.volume < 1) v.volume = 1; });
  document.addEventListener('play', unmute, true);
  setInterval(unmute, 500);
})();)JS";
// Keeps every microphone track the page asks for, so they can be switched off (silence) and on again
const char* const MIC_SCRIPT = R"JS((() => {
  if (window.__echoMicPatched) return;
  window.__echoMicPatched = true;
  window.__echoMuted = false;
  window.__echoTracks = [];
  const devices = navigator.mediaDevices;
  if (!devices || !devices.getUserMedia) return;
  const original = devices.getUserMedia.bind(devices);
  devices.getUserMedia = async (constraints) => {
    const stream = await original(constraints);
    stream.getAudioTracks().forEach(track => { window.__echoTracks.push(track); track.enabled = !window.__echoMuted; });
    return stream;
  };
  window.__echoMute = (muted) => {
    window.__echoMuted = muted;
    window.__echoTracks.forEach(track => { track.enabled = !muted; });
    return window.__echoTracks.length;
  };
})();)JS";
const Rect RESUME_BUTTON{312, 190, 712, 270}, QUIT_BUTTON{312, 300, 712, 380};
const Rect BACK_BUTTON{12, 10, 150, 66};
const Rect DOCK_BUTTON{744, 12, 1008, 64};                  // launcher header
const Rect MENU_DOCK_BUTTON{312, 400, 712, 470};            // in-app menu
constexpr uint64_t SHOT_PRESS_MS = 90;                      // a shot holds its tap this long
const Rect PAGE_UP{916, 84, 1012, 298}, PAGE_DOWN{916, 306, 1012, 520};
constexpr int ROWS_PER_PAGE = 6, ROW_Y = 84, ROW_H = 72;
const Rect SEEK_BAR{20, 448, 1004, 476};
// CAMERA: the live view above a row of buttons
enum CamButton { CamBack, CamHand, CamFlip, CamCloser, CamFurther, CamFreeze, CamTimer, CamShot };
constexpr int CAM_TIMERS[] = {0, 3, 5, 10};  // self-timer choices, seconds
// The tablet's CAMERA screen is its settings (the picture is on the side panel): one row per setting
enum CamSetting { SetSource, SetView, SetReach, SetSmooth, SetFisheye, SetResolution, SetTimer, CAM_SETTINGS };
constexpr const wchar_t* CAM_SETTING_NAMES[CAM_SETTINGS] = {L"SOURCE", L"VIEW", L"REACH", L"SMOOTHING", L"FISHEYE", L"RESOLUTION", L"TIMER"};
constexpr int CAM_ROW_Y = 66, CAM_ROW_H = 57, CAM_LABEL_W = 230;
const Rect CAM_BACK{12, 474, 232, 562}, CAM_FREEZE{402, 474, 622, 562}, CAM_PHOTO{792, 474, 1012, 562};
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

enum class Mode { Launcher, Browse, PlexLink, Running, Menu, Camera };

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
        : shared_(shared), standalone_(standalone), config_(loadConfig()), session_(config_), lightGun_(config_) {
        readPos_ = shared_->touchWrite;
        apps_ = listApps(config_);
        loadSettings();
        shared_->hostPid = LONG(GetCurrentProcessId());
        setHandCamera(config_, false);  // the head's view unless the CAMERA tile is open
        // The side panel's picture for EchoCam.dll (shown beside the tablet while CAMERA is open)
        panelMap_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(sizeof(panel_ipc::Shared)), panel_ipc::NAME);
        if (panelMap_) panel_ = static_cast<panel_ipc::Shared*>(MapViewOfFile(panelMap_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(panel_ipc::Shared)));
        if (panel_) { panel_->visible = 0; panel_->magic = panel_ipc::MAGIC; panelRead_ = panel_->touchWrite; fitSeen_ = panel_->calibrateDone; }
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
            if (mode_ == Mode::Running && session_.windowless() > 30000) {
                message_ = L"It did not open a window in 30 s (see host.log).";
                session_.kill();
                appClosed();
            }
            if (mode_ == Mode::PlexLink) pollPlexLink(now);
            applyChatMic(now);
            applyUnmute(now);
            if (mode_ == Mode::Camera) pollCamera(now);
            reportPlexProgress(now);
            pollDock();
            if (shotUpAt_ && now >= shotUpAt_) { shotUpAt_ = 0; onTouch({SHOT_TOUCH, false, shotX_, shotY_}); }
            bool visible = shared_->pageVisible != 0 || standalone_ || shared_->dockState == arcade::Docked;
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
        if (mode_ == Mode::Camera) setHandCamera(config_, false);
        releaseChatMic();
        spatial_.stop();  // gives the browser its volume back
        if (panel_) { panel_->visible = 0; UnmapViewOfFile(panel_); }
        if (panelMap_) CloseHandle(panelMap_);
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
        shotUpAt_ = 0;
        std::fill(std::begin(padHeld_), std::end(padHeld_), false);
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
        voiceMeter_.watch(0);
        spatial_.stop();
        releaseChatMic();
        finishPlex();
        lightGun_.appClosed();
        gunUsed_ = false;
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
        if (app.id == AppId::Camera) { openCamera(); return; }
        if (!session_.launch(app.id)) { message_ = L"Could not start " + app.title + L" (see host.log)."; return; }
        unmuteReady_ = false;
        if ((app.id == AppId::Reels || app.id == AppId::TikTok) && config_.spatialAudio) spatial_.start(session_.pid());
        if (app.id == AppId::ChatGpt) {  // the orb follows ChatGPT's voice: the browser's sound, measured
            voiceMeter_.watch(session_.pid());
            releaseChatMic();
            lastMuteApply_ = 0;
            chatOrb_ = false;
            chatOrbAuto_ = true;
            orbLevel_ = 0;
        }
        gunUsed_ = false;
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

    // ------------------------------------------------ camera
    // The game's desktop window shows the second viewport (-capturevp2), which EchoCam puts on a hand.
    void openCamera() {
        camOptions_ = loadCameraOptions(config_);
        saveCameraOptions(config_, camOptions_);  // unfrozen
        setHandCamera(config_, true);
        camShotAt_ = 0;
        camWindow_ = nullptr;
        camSerial_ = 0;
        camW_ = camH_ = 0;
        lastWindowSearch_ = 0;
        mode_ = Mode::Camera;
        resetInput();
    }
    void closeCamera() {
        if (panel_) panel_->visible = 0;
        applyCameraResolution(camWindow_, 0);  // the window's own size again outside the camera
        setHandCamera(config_, false);
        camShotAt_ = 0;
        camCapture_.stop();
        camWindow_ = nullptr;
        toLauncher();
    }
    void pollCamera(uint64_t now) {
        if (camShotAt_ && now >= camShotAt_) { camShotAt_ = 0; takePhoto(); }
        // Newest camera frame and the side panel, every loop (not only when the tablet screen redraws)
        if (camWindow_) camCapture_.latest(camPixels_, camW_, camH_, camSerial_);
        readPanelTouches();
        if (mode_ != Mode::Camera) return;  // closed by the panel's close button
        publishPanel(now);
        if (camWindow_ && IsWindow(camWindow_) && camCapture_.active()) return;
        if (now - lastWindowSearch_ < 1000) return;
        lastWindowSearch_ = now;
        camCapture_.stop();
        camWindow_ = findEchoWindow();
        if (camWindow_ && !camCapture_.start(camWindow_)) camWindow_ = nullptr;
        if (camWindow_) { hostLog("camera: capturing the Echo window %p", camWindow_); applyCameraResolution(camWindow_, camOptions_.resolution); }
    }
    std::wstring reachText() const {
        wchar_t text[48];
        swprintf_s(text, L"Camera %.2f m from your hand", camOptions_.reach);
        return text;
    }
    void cameraNote(const std::wstring& text) {
        camNote_ = text;
        camNoteUntil_ = GetTickCount64() + 3000;
    }
    void takePhoto() {
        if (camW_ <= 0 || camPixels_.empty()) { cameraNote(L"No camera picture yet"); return; }
        camFlashAt_ = GetTickCount64();
        auto pixels = std::make_shared<std::vector<uint8_t>>(camPixels_);
        int w = camW_, h = camH_;
        float strength = CAMERA_FISHEYE[camOptions_.fisheye].strength;
        spawn([this, pixels, w, h, strength] {
            if (strength > 0) {  // the photo bent like the live view
                std::vector<uint8_t> bent;
                fisheye(pixels->data(), w, h, w * 4, bent, strength);
                pixels->swap(bent);
            }
            std::wstring name = savePhoto(*pixels, w, h);
            std::lock_guard<std::mutex> lock(pendingLock_);
            pending_.push_back([this, name] { cameraNote(name.empty() ? L"Could not save the photo" : L"Saved Pictures\\Echo\\" + name); });
        });
    }
    // A camera control was used, on the tablet or on the side panel
    void cameraButton(int i) {
        switch (i) {
                case CamBack: closeCamera(); return;
                case CamHand: camOptions_.source = CameraOptions::Source((camOptions_.source + 1) % 3); break;  // tablet, left, right
                case CamFlip: camOptions_.selfie = !camOptions_.selfie; break;
                case CamCloser: camOptions_.reach = std::max(CAMERA_REACH_MIN, camOptions_.reach - .25f); cameraNote(reachText()); break;
                case CamFurther: camOptions_.reach = std::min(CAMERA_REACH_MAX, camOptions_.reach + .25f); cameraNote(reachText()); break;
                case CamFreeze: camOptions_.frozen = !camOptions_.frozen; cameraNote(camOptions_.frozen ? L"Camera frozen in place" : L"Camera follows your hand"); break;
                case CamTimer: camTimer_ = (camTimer_ + 1) % int(std::size(CAM_TIMERS)); return;
                case CamShot:
                    if (camShotAt_) { camShotAt_ = 0; cameraNote(L"Timer cancelled"); return; }
                    if (CAM_TIMERS[camTimer_] == 0) takePhoto();
                    else camShotAt_ = GetTickCount64() + uint64_t(CAM_TIMERS[camTimer_]) * 1000;
                    return;
        }
        saveCameraOptions(config_, camOptions_);
    }
    // Choices of a settings row: their labels; the chosen one is highlighted (REACH is - value +)
    std::vector<std::wstring> settingChoices(int row) const {
        switch (row) {
            case SetSource: return {L"TABLET", L"LEFT HAND", L"RIGHT HAND"};
            case SetView: return {L"FRONT", L"SELFIE"};
            case SetReach: { wchar_t v[24]; swprintf_s(v, L"%.2f m", camOptions_.reach); return {L"\x2212", v, L"+"}; }
            case SetSmooth: return {CAMERA_SMOOTHING_NAMES, CAMERA_SMOOTHING_NAMES + std::size(CAMERA_SMOOTHING_NAMES)};
            case SetResolution: { std::vector<std::wstring> v; for (auto& r : CAMERA_RESOLUTIONS) v.push_back(r.name); return v; }
            case SetFisheye: { std::vector<std::wstring> v; for (auto& f : CAMERA_FISHEYE) v.push_back(f.name); return v; }
            default: return {L"OFF", L"3s", L"5s", L"10s"};
        }
    }
    int settingChosen(int row) const {
        switch (row) {
            case SetSource: return int(camOptions_.source);
            case SetView: return camOptions_.selfie ? 1 : 0;
            case SetReach: return 1;
            case SetSmooth: return camOptions_.smoothing;
            case SetResolution: return camOptions_.resolution;
            case SetFisheye: return camOptions_.fisheye;
            default: return camTimer_;
        }
    }
    Rect settingChoice(int row, int i, int count) const {
        int y = CAM_ROW_Y + row * CAM_ROW_H, x0 = CAM_LABEL_W, w = (SCREEN_W - 12 - x0) / count;
        return {x0 + i * w + 4, y + 4, x0 + (i + 1) * w - 4, y + CAM_ROW_H - 6};
    }
    void chooseSetting(int row, int i) {
        switch (row) {
            case SetSource: camOptions_.source = CameraOptions::Source(i); break;
            case SetView: camOptions_.selfie = i == 1; break;
            case SetReach:
                if (i == 1) return;
                camOptions_.reach = std::clamp(camOptions_.reach + (i == 0 ? -.25f : .25f), CAMERA_REACH_MIN, CAMERA_REACH_MAX);
                break;
            case SetSmooth: camOptions_.smoothing = i; break;
            case SetFisheye: camOptions_.fisheye = i; break;
            case SetResolution:
                camOptions_.resolution = i;
                applyCameraResolution(camWindow_, i);
                cameraNote(L"Camera resolution: " + std::wstring(CAMERA_RESOLUTIONS[i].name));
                break;
            default: camTimer_ = i; return;
        }
        saveCameraOptions(config_, camOptions_);
    }
    void cameraTouch(const Touch& t) {
        for (int row = 0; row < CAM_SETTINGS; row++) {
            auto choices = settingChoices(row);
            for (int i = 0; i < int(choices.size()); i++)
                if (tapped(t, 800 + row * 10 + i, settingChoice(row, i, int(choices.size())))) { chooseSetting(row, i); return; }
        }
        if (tapped(t, 700 + CamBack, CAM_BACK)) { cameraButton(CamBack); return; }
        if (tapped(t, 700 + CamFreeze, CAM_FREEZE)) { cameraButton(CamFreeze); return; }
        if (tapped(t, 700 + CamShot, CAM_PHOTO)) { cameraButton(CamShot); return; }
        if (!t.down) pressed_ = -1;
    }
    std::wstring cameraLabel(int i) const {
        switch (i) {
            case CamBack: return L"\x25C0 BACK";
            case CamHand: return camOptions_.source == CameraOptions::Tablet ? L"TABLET" : camOptions_.source == CameraOptions::RightHand ? L"RIGHT HAND" : L"LEFT HAND";
            case CamFlip: return camOptions_.selfie ? L"SELFIE" : L"FRONT";
            case CamCloser: return L"\x2212";
            case CamFurther: return L"+";
            case CamFreeze: return camOptions_.frozen ? L"UNFREEZE" : L"FREEZE";
            case CamTimer: {
                wchar_t timer[16];
                if (CAM_TIMERS[camTimer_]) swprintf_s(timer, L"TIMER %ds", CAM_TIMERS[camTimer_]); else wcscpy_s(timer, L"TIMER OFF");
                return timer;
            }
            default: return camShotAt_ ? L"CANCEL" : L"\x25CF PHOTO";
        }
    }
    uint32_t cameraColor(int i) const {
        return i == CamBack ? rgb(50, 58, 84) : i == CamShot ? rgb(200, 150, 30) :
               i == CamFreeze && camOptions_.frozen ? rgb(40, 90, 130) : rgb(44, 50, 72);
    }

    // Side panel touches (EchoCam reports the fingertip): a press on a control, then its release, uses it
    void readPanelTouches() {
        if (!panel_) return;
        LONG write = panel_->touchWrite;
        if (write - panelRead_ > LONG(panel_ipc::TOUCHES)) panelRead_ = write - LONG(panel_ipc::TOUCHES);
        while (panelRead_ != write) {
            auto& t = panel_->touches[uint32_t(panelRead_) % panel_ipc::TOUCHES];
            if (t.seq != panelRead_ + 1) break;  // still being written
            MemoryBarrier();
            int x = int(t.x), y = int(t.y);
            if (t.down) {
                panelPressed_ = -1;
                for (int i = 0; i < PANEL_CONTROLS; i++)
                    if (grow(panelControl(i), 8).contains(x, y)) panelPressed_ = i;
                if (panelPressed_ == 1 && camShotAt_ == 0 && CAM_TIMERS[camTimer_] == 0) {  // the shutter fires on the press
                    panelPressed_ = -1;
                    cameraButton(CamShot);
                    panelShutterHeld_ = GetTickCount64();
                }
            } else if (panelPressed_ >= 0) {
                int i = panelPressed_;
                panelPressed_ = -1;
                if (mode_ == Mode::Camera) cameraButton(PANEL_ACTIONS[i]);
            }
            panelRead_++;
        }
    }

    // ------------------------------------------------ touches
    void readTouches() {
        LONG write = shared_->touchWrite;
        if (write - readPos_ > LONG(arcade::TOUCH_RING)) readPos_ = write - LONG(arcade::TOUCH_RING);
        while (readPos_ != write) {
            auto& e = shared_->touches[uint32_t(readPos_) % arcade::TOUCH_RING];
            if (e.seq != uint32_t(readPos_) + 1) break;  // still being written
            MemoryBarrier();
            if (e.kind <= arcade::TouchDown) {
                if (shared_->cols && e.cell < shared_->cols * shared_->rows)
                    onTouch(touchFromCell(int(e.cell), e.kind == arcade::TouchDown, int(shared_->cols), int(shared_->rows)));
            } else {
                int x = std::clamp(int(e.xy & 0xffff), 0, SCREEN_W - 1), y = std::clamp(int(e.xy >> 16), 0, SCREEN_H - 1);
                if (e.kind == arcade::Shot) onShot(x, y);
                else if (e.cell < 2) onTouch({POSTER_TOUCH + int(e.cell), e.kind != arcade::PointUp, x, y, e.kind == arcade::PointMove});
            }
            readPos_++;
        }
    }

    // ------------------------------------------------ dock (lobby poster) and light gun
    void requestDock(bool dock) {
        shared_->dockWant = dock ? 1 : 0;
        MemoryBarrier();
        dockAsked_ = InterlockedIncrement(&shared_->dockSerial);
        dockAskedAt_ = GetTickCount64();
        message_ = dock ? L"Looking for the nearest lobby poster..." : L"Undocking...";
        hostLog("dock: asked to %s", dock ? "dock" : "undock");
    }
    bool docked() const { return shared_->dockState == arcade::Docked; }
    bool dockPending() const { return dockAsked_ && shared_->dockDone != dockAsked_; }
    void pollDock() {
        LONG done = shared_->dockDone;
        if (done == dockSeen_) return;
        dockSeen_ = done;
        char text[sizeof(shared_->dockText) + 1] = {};
        memcpy(text, const_cast<const char*>(shared_->dockText), sizeof(shared_->dockText));
        std::wstring w;
        for (const char* c = text; *c; c++) w += wchar_t(static_cast<unsigned char>(*c));
        if (!w.empty()) message_ = w;
        hostLog("dock: state %ld: %s", shared_->dockState, text);
        if (!docked()) { held_.erase(POSTER_TOUCH); held_.erase(POSTER_TOUCH + 1); }
    }
    std::wstring dockLabel() const {
        if (dockPending() && GetTickCount64() - dockAskedAt_ < 5000) return L"DOCKING...";
        return docked() ? L"UNDOCK POSTER" : L"DOCK TO POSTER";
    }
    void dockTapped() { requestDock(!docked()); }

    // A bullet hit the docked poster. On RetroArch's picture it is a light-gun shot at that
    // spot (lightgun.cpp); anywhere else a short tap exactly where it landed (a click in
    // Balatro, a button on the launcher, menus, player and on-screen RetroPad).
    void onShot(int x, int y) {
        float u, v;
        if (lightGun_.enabled() && mode_ == Mode::Running && isRetro(session_.id()) && session_.hasWindow() &&
            !settingsMode() && fit_.contains(x, y) && toApp(x, y, u, v)) {
            lightGun_.shoot(session_.window(), u, v);
            gunUsed_ = true;
            return;
        }
        if (shotUpAt_) onTouch({SHOT_TOUCH, false, shotX_, shotY_});
        shotX_ = x; shotY_ = y;
        onTouch({SHOT_TOUCH, true, x, y});
        shotUpAt_ = GetTickCount64() + SHOT_PRESS_MS;
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
        // FOV is not here on purpose: it is set by hand in echo_tweaks.ini [fov].
        settings_ = {{L"TABLET SIZE", L"tablet", L"scale", .5f, 4.f, .05f, 1.f}};
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

    // 3D SOUND toggle (arcade.ini [host] spatial_audio), below the sliders
    Rect spatialToggle() const { int y = settingY(settings_.size()); return {744, y + 10, 1004, y + 84}; }
    void setSpatialAudio(bool on) {
        config_.spatialAudio = on;
        WritePrivateProfileStringW(L"host", L"spatial_audio", on ? L"1" : L"0", (config_.dir + L"\\arcade.ini").c_str());
        bool feed = (session_.id() == AppId::Reels || session_.id() == AppId::TikTok) && session_.alive();
        if (!on) spatial_.stop();  // also gives the browser its volume back
        else if (feed) spatial_.start(session_.pid());
        hostLog("settings: 3D sound %s", on ? "on" : "off");
    }

    void settingsTouch(const Touch& t) {
        if (tapped(t, 650, spatialToggle())) { setSpatialAudio(!config_.spatialAudio); return; }
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
        int y = settingY(settings_.size());
        canvas_.text({24, y, 720, y + 50}, L"3D SOUND", 26, rgb(235, 235, 240), true, 0);
        canvas_.text({24, y + 46, 720, y + 90}, L"REELS and TIKTOK sound comes from the tablet", 18, rgb(150, 160, 180), false, 0);
        button(spatialToggle(), config_.spatialAudio ? L"ON" : L"OFF", pressed_ == 650, config_.spatialAudio ? rgb(40, 110, 60) : rgb(70, 50, 50), 30);
        canvas_.text({24, 516, 1004, 572}, L"Saved instantly; the tablet size and 3D sound change live. "
                     L"View (FOV) is set in echo_tweaks.ini.", 17, rgb(150, 160, 180), false, 0);
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
            case Mode::Camera: cameraTouch(t); break;
            case Mode::Running:
                if (isVideo(session_.id())) playerTouch(t);
                else if (isRetro(session_.id())) retroTouch(t);
                else if (session_.id() == AppId::ChatGpt) chatTouch(t);
                else if (isFeed(session_.id())) reelsTouch(t);
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
        if (tapped(t, 500, DOCK_BUTTON)) { dockTapped(); return; }
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
        else if (tapped(t, 202, MENU_DOCK_BUTTON)) dockTapped();
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
            if (on && !padHeld_[b.pad]) gunButton(b.pad);
            padHeld_[b.pad] = on;
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

    // Once you have shot at the game, the pad's A/B/X/START/SELECT also press the light gun's
    // own buttons (GunCon A/B, Justifier special...), which the RetroPad can't reach: the gun
    // replaces the pad on its port.
    void gunButton(Pad pad) {
        if (!gunUsed_ || !session_.hasWindow()) return;
        GunButton b = pad == PadA ? GunAuxA : pad == PadB ? GunAuxB : pad == PadX ? GunAuxC :
                      pad == PadStart ? GunStart : pad == PadSelect ? GunSelect : GunButtonCount;
        if (b != GunButtonCount) lightGun_.press(session_.window(), b);
    }

    bool toApp(int x, int y, float& u, float& v) {
        if (!fit_.w() || !fit_.h()) return false;
        u = std::clamp((x - fit_.x0 + .5f) / fit_.w(), 0.f, 1.f);
        v = std::clamp((y - fit_.y0 + .5f) / fit_.h(), 0.f, 1.f);
        return true;
    }

    void balatroTouch(const Touch& t) {
        if (BALATRO_HOME.contains(t.x, t.y)) { if (!t.down) openMenu(); return; }
        auto same = std::find_if(gameCells_.begin(), gameCells_.end(), [&](const Touch& g) { return g.cell == t.cell; });
        if (t.move && same != gameCells_.end()) *same = t;
        else if (t.down) gameCells_.push_back(t);
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

    // CHATGPT: the microphone can be muted for ChatGPT only (Echo's voice chat is not affected), e.g. to talk to your team.
    // A script in the page keeps the microphone tracks it asks for and switches them off and on (MIC_SCRIPT); it is put in
    // as soon as the browser's DevTools answer, so voice sessions started after the tile opened are covered.
    void applyChatMic(uint64_t now) {
        if (session_.id() != AppId::ChatGpt || mode_ == Mode::Launcher || now - lastMuteApply_ < 1000) return;
        lastMuteApply_ = now;
        auto& page = session_.reels();
        if (!micGranted_)
            micGranted_ = page.browserCommand(R"({"id":1,"method":"Browser.grantPermissions","params":{"origin":"https://chatgpt.com","permissions":["audioCapture"]}})");
        // An earlier build muted the browser's recording in Windows (which remembers it per app): keep it unmuted
        if (now - lastWindowsUnmute_ > 3000) { setCaptureMuteTree(session_.pid(), false); lastWindowsUnmute_ = now; }
        if (!micScriptReady_)
            micScriptReady_ = page.addStartupScript(MIC_SCRIPT) && page.evaluate(MIC_SCRIPT);
        if (micScriptReady_ && chatMuted_ != micAppliedMuted_ &&
            page.evaluate(std::string("window.__echoMute && window.__echoMute(") + (chatMuted_ ? "true" : "false") + ")")) {
            micAppliedMuted_ = chatMuted_;
            hostLog("chatgpt: microphone %s for ChatGPT", chatMuted_ ? "muted" : "on");
        }
    }
    // REELS / TIKTOK: the unmute script, put in as soon as the browser's DevTools answer (and kept for pages loaded later)
    void applyUnmute(uint64_t now) {
        if (mode_ != Mode::Running && mode_ != Mode::Menu) return;
        AppId id = session_.id();
        if ((id != AppId::Reels && id != AppId::TikTok) || unmuteReady_ || now - lastUnmuteTry_ < 1000) return;
        lastUnmuteTry_ = now;
        auto& page = session_.reels();
        unmuteReady_ = page.addStartupScript(UNMUTE_SCRIPT) && page.evaluate(UNMUTE_SCRIPT);
        if (unmuteReady_) hostLog("browser: videos unmuted");
    }

    void releaseChatMic() {
        chatMuted_ = micAppliedMuted_ = micScriptReady_ = micGranted_ = false;
    }
    void toggleChatMic() {
        uint64_t now = GetTickCount64();
        if (now - lastMicToggle_ < 700) return;  // a second touch of the same press
        lastMicToggle_ = now;
        chatMuted_ = !chatMuted_;
        lastMuteApply_ = 0;
        applyChatMic(now);
    }

    // CHATGPT: in the orb view a tap shows the chat page; on the page the top right button goes back to the orb
    void chatTouch(const Touch& t) {
        if (chatOrb_) {
            if (BALATRO_HOME.contains(t.x, t.y)) { if (!t.down) openMenu(); return; }
            if (tapped(t, 460, ORB_MIC_BUTTON)) { toggleChatMic(); return; }
            if (tapped(t, 462, ORB_CHAT_BUTTON)) { chatOrb_ = false; chatOrbAuto_ = false; return; }
            if (!t.down) pressed_ = -1;
            return;
        }
        if (ORB_BUTTON.contains(t.x, t.y) && !reelsActive_) { if (!t.down) chatOrb_ = true; return; }
        if (MIC_BUTTON.contains(t.x, t.y) && !reelsActive_) { if (tapped(t, 461, MIC_BUTTON)) toggleChatMic(); return; }
        reelsTouch(t);
    }

    // REELS: a quick touch is a click where it landed; sliding up or down (like a phone) scrolls to
    // the next or previous reel. The menu button (top left) works as in Balatro.
    void reelsTouch(const Touch& t) {
        if (BALATRO_HOME.contains(t.x, t.y) && !reelsActive_) { if (!t.down) openMenu(); return; }
        if (t.down && !reelsActive_) {
            reelsActive_ = true;
            reelsStartX_ = reelsLastX_ = t.x;
            reelsStartY_ = reelsLastY_ = t.y;
            return;
        }
        if (t.down) { reelsLastX_ = t.x; reelsLastY_ = t.y; return; }
        if (!held_.empty() || !reelsActive_) return;  // wait until every finger cell is released
        reelsActive_ = false;
        int dx = reelsLastX_ - reelsStartX_, dy = reelsLastY_ - reelsStartY_;
        float u, v;
        if (!toApp(reelsStartX_, reelsStartY_, u, v)) return;
        HWND window = session_.window();
        if (std::abs(dy) >= 70 && std::abs(dy) > std::abs(dx)) {
            // Finger moved up = next reel, like swiping on a phone
            session_.reels().wheel(window, u, v, dy < 0 ? 600 : -600);
        } else {
            session_.reels().tap(window, u, v);
        }
    }

    // ------------------------------------------------ drawing
    Rect tileRect(size_t i) const {
        if (apps_.size() > 6) {  // Four columns once there are more than six tiles
            int col = int(i % 4), row = int(i / 4);
            return {20 + col * 248, 88 + row * 222, 20 + col * 248 + 236, 88 + row * 222 + 210};
        }
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
        canvas_.text({420, 0, 730, 76}, docked() ? L"on the lobby poster" : L"tap a tile", 22, rgb(150, 160, 180), false, 2);
        button(DOCK_BUTTON, dockLabel(), pressed_ == 500, docked() ? rgb(40, 110, 60) : rgb(50, 58, 84), 22);
        for (size_t i = 0; i < apps_.size(); i++) {
            auto& a = apps_[i];
            Rect r = tileRect(i);
            bool on = pressed_ == int(i);
            canvas_.fill(r, on ? rgb(48, 56, 80) : rgb(30, 34, 50));
            canvas_.fill({r.x0, r.y0, r.x0 + 12, r.y1}, a.accent);
            canvas_.frame(r, on ? rgb(255, 255, 255) : rgb(50, 58, 84), on ? 4 : 2);
            canvas_.text({r.x0 + 30, r.y0 + 26, r.x1 - 12, r.y0 + 96}, a.title, apps_.size() > 6 ? 32 : 40, rgb(245, 245, 250), true, 0);
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

    // CHATGPT's orb: a cloudy blue and white ball that swirls slowly and swells with ChatGPT's voice
    void drawOrb() {
        float target = std::min(1.f, voiceMeter_.level() * 2.5f);
        orbLevel_ += (target - orbLevel_) * (target > orbLevel_ ? .45f : .08f);  // quick to swell, slow to settle
        float t = GetTickCount64() / 1000.f, level = orbLevel_, dim = chatMuted_ ? .55f : 1.f;
        canvas_.clear(rgb(0, 0, 0));
        uint32_t* px = canvas_.pixels();
        const float cx = SCREEN_W / 2.f, cy = SCREEN_H / 2.f - 6;
        const float r = 150 + 70 * level + 4 * std::sin(t * 1.5f), glow = 14 + 70 * level;
        int y0 = std::max(0, int(cy - r - glow)), y1 = std::min(SCREEN_H - 1, int(cy + r + glow));
        int x0 = std::max(0, int(cx - r - glow)), x1 = std::min(SCREEN_W - 1, int(cx + r + glow));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float dx = x - cx, dy = y - cy, d = std::sqrt(dx * dx + dy * dy);
                if (d > r + glow) continue;
                float cr, cg, cb, a;
                if (d > r) {  // soft blue glow around it, stronger while talking
                    float g = 1 - (d - r) / glow;
                    a = g * g * (.25f + .55f * level);
                    cr = 40; cg = 140; cb = 255;
                } else {
                    float u = dx / r, v = dy / r;
                    float n = std::sin(u * 3.1f + t * .9f + std::sin(v * 2.3f - t * .7f) * 1.4f) +
                              std::sin(v * 3.7f - t * .6f + std::sin(u * 2.1f + t * .5f) * 1.2f);
                    float mix = std::clamp(.5f + .22f * n + .35f * v - .2f * u - .25f * level, 0.f, 1.f);  // 0 white .. 1 blue
                    cr = 235 + (25 - 235) * mix; cg = 246 + (120 - 246) * mix; cb = 255;
                    float edge = std::clamp(1 - d / r, 0.f, 1.f);  // a little darker towards the rim
                    float shade = .82f + .18f * std::sqrt(edge) + .12f * level;
                    cr *= shade * dim; cg *= shade * dim; cb *= std::min(1.f, shade) * dim;
                    a = std::clamp(r + .5f - d, 0.f, 1.f);
                }
                uint32_t& p = px[y * SCREEN_W + x];
                int br = (p >> 16) & 255, bg = (p >> 8) & 255, bb = p & 255;
                int R = std::min(255, int(br + (cr - br) * a)), G = std::min(255, int(bg + (cg - bg) * a)), B = std::min(255, int(bb + (cb - bb) * a));
                p = rgb(R, G, B);
            }
        micButton(ORB_MIC_BUTTON, pressed_ == 460);
        canvas_.fill(ORB_CHAT_BUTTON, pressed_ == 462 ? rgb(250, 250, 250) : rgb(30, 34, 50));
        canvas_.frame(ORB_CHAT_BUTTON, rgb(90, 96, 120), 2);
        canvas_.text(ORB_CHAT_BUTTON, L"CHAT", 26, pressed_ == 462 ? rgb(20, 20, 20) : rgb(220, 225, 235));
        canvas_.blend(BALATRO_HOME, rgb(0, 0, 0), 150);
        canvas_.text(BALATRO_HOME, L"\x2261", 40, rgb(200, 200, 210));
    }

    // Whether ChatGPT hears you: red while muted
    void micButton(Rect r, bool on) {
        canvas_.fill(r, on ? rgb(250, 250, 250) : chatMuted_ ? rgb(150, 40, 40) : rgb(40, 110, 60));
        canvas_.frame(r, rgb(235, 235, 235), 2);
        canvas_.text(r, chatMuted_ ? L"MIC MUTED - TAP TO TALK" : L"MIC ON - TAP TO MUTE", r.h() > 70 ? 30 : 20, on ? rgb(20, 20, 20) : rgb(255, 255, 255));
    }

    void drawApp() {
        if (session_.id() == AppId::ChatGpt) {
            if (chatOrbAuto_ && voiceMeter_.level() > .05f) { chatOrb_ = true; chatOrbAuto_ = false; }  // ChatGPT started talking
            if (chatOrb_ && mode_ == Mode::Running) { drawOrb(); return; }
        }
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
            if (session_.id() == AppId::ChatGpt) {  // back to the orb, and Echo's microphone
                canvas_.blend(ORB_BUTTON, rgb(0, 0, 0), 150);
                canvas_.text(ORB_BUTTON, L"\x25C9", 36, rgb(120, 190, 255));
                micButton(MIC_BUTTON, pressed_ == 461);
            }
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

    void drawCamera() {
        canvas_.clear(rgb(14, 16, 24));
        canvas_.fill({0, 0, SCREEN_W, 60}, rgb(22, 26, 40));
        canvas_.text({24, 0, 600, 60}, L"CAMERA SETTINGS", 30, rgb(90, 200, 255), true, 0);
        uint64_t now = GetTickCount64();
        std::wstring status = !camWindow_ ? L"looking for the Echo window..." : camShotAt_ && camShotAt_ > now ?
            L"photo in " + std::to_wstring((camShotAt_ - now + 999) / 1000) + L" s" : camOptions_.frozen ? L"frozen in place" : L"live on the side panel";
        canvas_.text({560, 0, SCREEN_W - 20, 60}, status, 20, rgb(150, 160, 180), false, 2);
        for (int row = 0; row < CAM_SETTINGS; row++) {
            int y = CAM_ROW_Y + row * CAM_ROW_H;
            canvas_.text({24, y, CAM_LABEL_W, y + CAM_ROW_H - 2}, CAM_SETTING_NAMES[row], 22, rgb(220, 224, 235), true, 0);
            auto choices = settingChoices(row);
            int chosen = settingChosen(row);
            for (int i = 0; i < int(choices.size()); i++) {
                bool on = row == SetReach ? i == 1 : i == chosen;
                uint32_t color = row == SetReach && i == 1 ? rgb(24, 28, 40) : on ? rgb(40, 110, 150) : rgb(44, 50, 72);
                button(settingChoice(row, i, int(choices.size())), choices[i], pressed_ == 800 + row * 10 + i, color, row == SetReach && i != 1 ? 32 : 20);
            }
        }
        button(CAM_BACK, L"\x25C0  BACK", pressed_ == 700 + CamBack, rgb(50, 58, 84), 24);
        button(CAM_FREEZE, camOptions_.frozen ? L"UNFREEZE" : L"FREEZE", pressed_ == 700 + CamFreeze, camOptions_.frozen ? rgb(40, 90, 130) : rgb(44, 50, 72), 24);
        button(CAM_PHOTO, camShotAt_ ? L"CANCEL" : L"\x25CF  PHOTO", pressed_ == 700 + CamShot, rgb(200, 150, 30), 24);
        if (now < camNoteUntil_) {
            canvas_.blend({0, 432, SCREEN_W, 470}, rgb(0, 0, 0), 170);
            canvas_.text({16, 432, SCREEN_W - 16, 470}, camNote_, 20, rgb(255, 255, 255), true, 0);
        }
    }

    // The side panel beside the tablet, like a phone's camera screen: title bar with close, the camera view, a shutter.
    // The settings stay on the tablet's own screen. Corners are transparent (the compositor blends the panel's alpha).
    static constexpr int PANEL_W = panel_ipc::WIDTH, PANEL_H = panel_ipc::HEIGHT, PANEL_RADIUS = 34, PANEL_BORDER = 6;
    static constexpr Rect PANEL_TITLE{0, 0, PANEL_W, 92}, PANEL_VIEW{18, 92, PANEL_W - 18, 636}, PANEL_BAR{0, 636, PANEL_W, PANEL_H};
    static constexpr int PANEL_CONTROLS = 2, PANEL_SHUTTER_X = PANEL_W / 2, PANEL_SHUTTER_Y = 702, PANEL_SHUTTER_R = 50;
    static constexpr int PANEL_ACTIONS[PANEL_CONTROLS] = {CamBack, CamShot};  // close (top right), shutter
    static Rect panelControl(int i) {
        static constexpr Rect rects[PANEL_CONTROLS] = {
            {PANEL_W - 96, 18, PANEL_W - 30, 78},
            {PANEL_SHUTTER_X - 62, PANEL_SHUTTER_Y - 62, PANEL_SHUTTER_X + 62, PANEL_SHUTTER_Y + 62}};
        return rects[i];
    }
    void disc(int cx, int cy, int r, uint32_t color) {
        uint32_t* px = panelCanvas_.pixels();
        for (int y = std::max(0, cy - r); y <= std::min(PANEL_H - 1, cy + r); y++)
            for (int x = std::max(0, cx - r); x <= std::min(PANEL_W - 1, cx + r); x++)
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) px[y * PANEL_W + x] = color;
    }

    void roundPanel() {
        uint32_t* px = panelCanvas_.pixels();
        const uint32_t border = rgb(200, 196, 188);
        for (int y = 0; y < PANEL_H; y++)
            for (int x = 0; x < PANEL_W; x++) {
                // distance outside the rounded rectangle's inner corner circles
                int cx = x < PANEL_RADIUS ? PANEL_RADIUS : x >= PANEL_W - PANEL_RADIUS ? PANEL_W - 1 - PANEL_RADIUS : x;
                int cy = y < PANEL_RADIUS ? PANEL_RADIUS : y >= PANEL_H - PANEL_RADIUS ? PANEL_H - 1 - PANEL_RADIUS : y;
                float d = std::sqrt(float((x - cx) * (x - cx) + (y - cy) * (y - cy)));
                bool edge = x < PANEL_BORDER || y < PANEL_BORDER || x >= PANEL_W - PANEL_BORDER || y >= PANEL_H - PANEL_BORDER;
                if (d > PANEL_RADIUS) px[y * PANEL_W + x] = 0;  // outside: transparent
                else if (d > PANEL_RADIUS - PANEL_BORDER || (edge && (cx == x || cy == y))) px[y * PANEL_W + x] = border;
            }
    }

    // Up to ~72 pictures a second: each new camera frame, and overlays as they change
    void publishPanel(uint64_t now) {
        if (!panel_) return;
        bool fresh = camSerial_ != panelSerial_, overlay = (camShotAt_ && camShotAt_ > now) || now < camNoteUntil_ || (camFlashAt_ && now - camFlashAt_ < 250) ||
                     panel_->hover || panelPressed_ >= 0;
        if ((!fresh && !overlay && now - lastPanel_ < 500) || now - lastPanel_ < 13) { panel_->visible = 1; return; }
        lastPanel_ = now;
        panelSerial_ = camSerial_;
        auto& c = panelCanvas_;
        c.clear(rgb(30, 30, 32));
        // Title bar
        c.fill(PANEL_TITLE, rgb(36, 35, 34));
        c.text({34, 0, PANEL_W - 110, PANEL_TITLE.y1}, L"Camera", 48, rgb(222, 214, 190), false, 0);
        Rect close = panelControl(0);
        c.fill(close, panelPressed_ == 0 ? rgb(250, 250, 250) : rgb(20, 20, 20));
        c.frame(close, rgb(235, 235, 235), 3);
        c.text(close, L"\x2715", 30, panelPressed_ == 0 ? rgb(20, 20, 20) : rgb(240, 240, 240));
        // Camera view: fills its area (cropped to the area's shape)
        c.fill(PANEL_VIEW, rgb(10, 10, 14));
        if (camWindow_ && camW_ > 0) {
            float want = float(PANEL_VIEW.w()) / PANEL_VIEW.h(), have = float(camW_) / camH_;
            int cw = camW_, ch = camH_, cx = 0, cy = 0;
            if (have > want) { cw = int(camH_ * want); cx = (camW_ - cw) / 2; }
            else { ch = int(camW_ / want); cy = (camH_ - ch) / 2; }
            c.blit(camPixels_.data() + (size_t(cy) * camW_ + cx) * 4, cw, ch, camW_ * 4, PANEL_VIEW);
            if (float strength = CAMERA_FISHEYE[camOptions_.fisheye].strength; strength > 0) {
                uint32_t* px = c.pixels() + PANEL_VIEW.y0 * PANEL_W + PANEL_VIEW.x0;
                panelViewCopy_.resize(size_t(PANEL_VIEW.w()) * PANEL_VIEW.h());
                for (int y = 0; y < PANEL_VIEW.h(); y++) memcpy(&panelViewCopy_[size_t(y) * PANEL_VIEW.w()], px + y * PANEL_W, PANEL_VIEW.w() * 4);
                panelFisheye_.apply(panelViewCopy_.data(), PANEL_VIEW.w(), PANEL_VIEW.h(), PANEL_VIEW.w() * 4, px, PANEL_W * 4, strength);
            }
        } else {
            c.text({PANEL_VIEW.x0, 300, PANEL_VIEW.x1, 360}, L"Start Echo VR", 30, rgb(220, 220, 230));
            c.text({PANEL_VIEW.x0, 360, PANEL_VIEW.x1, 410}, L"with -capturevp2", 26, rgb(160, 170, 190), false);
        }
        if (camFlashAt_ && now - camFlashAt_ < 250) c.blend(PANEL_VIEW, rgb(255, 255, 255), int(200 * (250 - (now - camFlashAt_)) / 250));
        if (camShotAt_ && camShotAt_ > now) {
            wchar_t count[8];
            swprintf_s(count, L"%llu", (camShotAt_ - now + 999) / 1000);
            c.blend({188, 260, 388, 450}, rgb(0, 0, 0), 150);
            c.text({188, 260, 388, 450}, count, 140, rgb(255, 255, 255));
        }
        if (camOptions_.frozen) {
            c.blend({PANEL_VIEW.x1 - 140, PANEL_VIEW.y0 + 12, PANEL_VIEW.x1 - 12, PANEL_VIEW.y0 + 52}, rgb(0, 0, 0), 150);
            c.text({PANEL_VIEW.x1 - 140, PANEL_VIEW.y0 + 12, PANEL_VIEW.x1 - 12, PANEL_VIEW.y0 + 52}, L"FROZEN", 20, rgb(120, 200, 255));
        }
        if (now < camNoteUntil_) {
            c.blend({PANEL_VIEW.x0, PANEL_VIEW.y1 - 44, PANEL_VIEW.x1, PANEL_VIEW.y1}, rgb(0, 0, 0), 170);
            c.text({PANEL_VIEW.x0 + 12, PANEL_VIEW.y1 - 44, PANEL_VIEW.x1 - 12, PANEL_VIEW.y1}, camNote_, 18, rgb(255, 255, 255), true, 0);
        }
        // Shutter: a white ring around a disc (amber while the self-timer counts down; a tap then cancels it)
        c.fill(PANEL_BAR, rgb(36, 35, 34));
        bool held = panelPressed_ == 1 || (panelShutterHeld_ && now - panelShutterHeld_ < 150), counting = camShotAt_ && camShotAt_ > now;
        disc(PANEL_SHUTTER_X, PANEL_SHUTTER_Y, PANEL_SHUTTER_R, rgb(240, 240, 240));
        disc(PANEL_SHUTTER_X, PANEL_SHUTTER_Y, PANEL_SHUTTER_R - 7, rgb(36, 35, 34));
        disc(PANEL_SHUTTER_X, PANEL_SHUTTER_Y, PANEL_SHUTTER_R - 12, held ? rgb(170, 170, 175) : counting ? rgb(230, 170, 40) : rgb(245, 245, 245));
        if (CAM_TIMERS[camTimer_]) {  // the self-timer setting, beside the shutter
            wchar_t timer[8];
            swprintf_s(timer, L"%ds", CAM_TIMERS[camTimer_]);
            c.text({PANEL_SHUTTER_X + 70, PANEL_SHUTTER_Y - 25, PANEL_SHUTTER_X + 170, PANEL_SHUTTER_Y + 25}, timer, 26, rgb(222, 214, 190), true, 0);
        }
        if (panel_->hover) {  // where the finger points
            int x = int(panel_->hoverX), y = int(panel_->hoverY);
            c.fill({x - 9, y - 9, x + 9, y + 9}, rgb(255, 255, 255));
            c.fill({x - 5, y - 5, x + 5, y + 5}, rgb(90, 200, 255));
        }
        c.opaque();
        roundPanel();
        LONG back = (panel_->front + 1) & 1;
        memcpy(panel_->frames[back], c.pixels(), sizeof(panel_->frames[back]));
        MemoryBarrier();
        panel_->front = back;
        InterlockedIncrement(&panel_->serial);
        panel_->visible = 1;
    }

    void drawMenu() {
        drawApp();
        canvas_.blend({0, 0, SCREEN_W, SCREEN_H}, rgb(0, 0, 0), 190);
        canvas_.text({0, 90, SCREEN_W, 160}, current_.title + L" - " + current_.subtitle, 34, rgb(255, 255, 255));
        button(RESUME_BUTTON, L"RESUME", pressed_ == 200, rgb(40, 110, 60), 32);
        button(QUIT_BUTTON, L"QUIT TO LAUNCHER", pressed_ == 201, rgb(130, 40, 40), 32);
        button(MENU_DOCK_BUTTON, dockLabel(), pressed_ == 202, docked() ? rgb(40, 110, 60) : rgb(50, 58, 84), 28);
        if (!message_.empty()) canvas_.text({0, 486, SCREEN_W, 540}, message_, 22, rgb(255, 220, 200));
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
            case Mode::Camera: drawCamera(); break;
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
    LightGun lightGun_;
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
    bool pointerDown_ = false, stickMode_ = false, gunUsed_ = false, padHeld_[PadCount] = {};
    float lastU_ = 0, lastV_ = 0, stick_[2] = {};
    bool reelsActive_ = false;  // REELS: a finger is down (start and latest cell centre)
    int reelsStartX_ = 0, reelsStartY_ = 0, reelsLastX_ = 0, reelsLastY_ = 0;
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
    LONG dockAsked_ = 0, dockSeen_ = 0;
    uint64_t dockAskedAt_ = 0, shotUpAt_ = 0;
    int shotX_ = 0, shotY_ = 0;
    // CAMERA
    WindowCapture camCapture_;
    HWND camWindow_ = nullptr;
    CameraOptions camOptions_;
    std::vector<uint8_t> camPixels_;
    int camW_ = 0, camH_ = 0;
    uint64_t camSerial_ = 0, lastWindowSearch_ = 0, camFlashAt_ = 0, camNoteUntil_ = 0, camShotAt_ = 0;
    int camTimer_ = 0;  // index into CAM_TIMERS
    // Side panel (EchoCam.dll draws it beside the tablet)
    HANDLE panelMap_ = nullptr;
    panel_ipc::Shared* panel_ = nullptr;
    Canvas panelCanvas_{panel_ipc::WIDTH, panel_ipc::HEIGHT};
    uint64_t panelSerial_ = 0, lastPanel_ = 0;
    LONG panelRead_ = 0, fitSeen_ = 0;
    uint64_t panelShutterHeld_ = 0;
    // CHATGPT orb
    ProcessAudioMeter voiceMeter_;
    SpatialAudio spatial_;  // REELS / TIKTOK sound from the tablet
    bool chatOrb_ = false, chatOrbAuto_ = false;  // showing the orb; switch to it when ChatGPT first talks
    float orbLevel_ = 0;
    bool chatMuted_ = false, micAppliedMuted_ = false, micScriptReady_ = false;  // ChatGPT does not hear the microphone
    uint64_t lastMicToggle_ = 0, lastWindowsUnmute_ = 0, lastUnmuteTry_ = 0;
    bool unmuteReady_ = false;  // REELS / TIKTOK: the unmute script is in the page
    bool micGranted_ = false;  // chatgpt.com may use the microphone (granted through DevTools)
    uint64_t lastMuteApply_ = 0;
    int lastMuteCount_ = 0;
    FisheyeMap panelFisheye_;
    std::vector<uint32_t> panelViewCopy_;  // shutter shown pressed briefly after an instant shot
    int panelPressed_ = -1;
    std::wstring camNote_;
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
