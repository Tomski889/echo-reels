// Spatial sound for the browser tiles: the browser's sound is played from where the tablet is (EchoCam shares its place
// relative to the head, spatial_ipc.h). The browser itself is turned down to 1 % (Windows' per-app volume) and its sound is
// recorded through Windows' process loopback, boosted back, placed (level, ear delay, distance, muffled behind) and played
// on the default output. Without the tablet's place it plays unchanged.
#pragma once
#include <windows.h>
#include <atomic>
#include <thread>

// The browser tree's per-app volume back to full. Windows remembers it per app, and 3D sound sets it to 1 %, so a host that
// ended without setting it back (Echo closed) leaves the browser almost silent next time. Any thread.
void restoreBrowserVolume(DWORD root);

class SpatialAudio {
public:
    ~SpatialAudio() { stop(); }
    void start(DWORD browserPid);  // the browser's root process
    void stop();                   // also gives the browser its full volume back
    bool running() const { return thread_.joinable(); }
private:
    void run(DWORD root);
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
