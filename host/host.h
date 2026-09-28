// ArcadeHost: renders the tablet screen (launcher, captured apps, touch controls)
// into shared memory for EchoArcade.dll, and routes tablet touches to the apps.
#pragma once
#include "../native/shared/arcade_ipc.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

constexpr int SCREEN_W = int(arcade::WIDTH), SCREEN_H = int(arcade::HEIGHT);

struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
};

void hostLog(const char* format, ...);

// ---- configuration (EchoArcade\arcade.ini, written by the installer) ----
struct Config {
    std::wstring dir;                 // folder containing ArcadeHost.exe
    std::wstring love;                // love.exe (11.5)
    std::wstring balatroSteam;        // prepared Steam game folder (extracted + bridge)
    std::wstring balatroPortmaster;   // prepared PortMaster build folder
    std::wstring retroarch;           // retroarch.exe
    std::wstring doomCore, doomWad;   // prboom core + WAD for the DOOM tile
    std::wstring roms;                // RetroArch's Load Content start folder
    std::wstring windowMode;          // "desktop" (default) or "offscreen"
    int balatroPort = 55410, retroCmdPort = 55355, retroPadPort = 55400;
};
Config loadConfig();

// ---- 2D drawing into the 1024x574 BGRA screen (GDI for text) ----
class Canvas {
public:
    Canvas();
    ~Canvas();
    uint32_t* pixels() { return px_; }
    void clear(uint32_t color);
    void fill(Rect r, uint32_t color);
    void blend(Rect r, uint32_t color, int alpha);  // alpha 0..255
    void frame(Rect r, uint32_t color, int thickness);
    void text(Rect r, const std::wstring& s, int size, uint32_t color, bool bold = true, int align = 1);  // 0 left 1 center 2 right
    void blit(const uint8_t* src, int srcW, int srcH, int srcPitch, Rect dst);  // aspect-fit, bilinear
    void opaque();  // force alpha to 255 (GDI clears it; the tablet sprite honours alpha)
private:
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr, old_ = nullptr;
    uint32_t* px_ = nullptr;
};

constexpr uint32_t rgb(int r, int g, int b) { return 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b); }

// ---- window capture (Windows.Graphics.Capture) ----
class WindowCapture {
public:
    WindowCapture();
    ~WindowCapture();
    bool start(HWND window);
    void stop();
    bool active() const;
    // Copies the newest frame if it changed since `serial`. Returns false if none.
    bool latest(std::vector<uint8_t>& out, int& w, int& h, uint64_t& serial);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- touch model ----
struct Touch { int cell; bool down; int x, y; };  // x, y: cell centre in screen pixels
Touch touchFromCell(int cell, bool down, int cols, int rows);
