#pragma once
// REELS tile: Instagram Reels in Chrome or Edge (app mode), driven through the browser's DevTools
// protocol on 127.0.0.1, so taps and swipes reach the page without focusing its window.
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class CdpInput {
public:
    ~CdpInput() { disconnect(); }
    void setPort(int port) { port_ = port; }
    // Page coordinates are CSS pixels; x, y: 0..1 across the page's viewport.
    void tap(HWND window, float x, float y);
    void wheel(HWND window, float x, float y, int deltaY);  // + scrolls down (next reel)
    void key(const char* key, const char* code, int virtualKey);
    // JavaScript in the page now (Runtime.evaluate), and in every page loaded from now on (before its own scripts)
    bool evaluate(const std::string& js);
    bool addStartupScript(const std::string& js);
    // A command for the whole browser (its own DevTools socket, e.g. Browser.grantPermissions); true once sent
    bool browserCommand(const std::string& json);
    void disconnect();
    // 1 while the page is Instagram's login (or a login check), 0 when not, -1 unknown. Asks /json/list, no WebSocket.
    static int loginShown(int port);

private:
    bool ensure();                         // (re)connects to the first page target
    bool sendJson(const std::string& json);
    void cssPoint(HWND window, float x, float y, int& cx, int& cy) const;
    void reader();                         // drains responses and events

    int port_ = 9335;
    std::mutex mutex_;
    void* session_ = nullptr;              // HINTERNET
    void* connection_ = nullptr;
    void* socket_ = nullptr;
    std::thread reader_;
    std::atomic<bool> broken_{false};
    int nextId_ = 1;
    uint64_t lastAttempt_ = 0;
};
