// How loud an app is right now: the peak of the Windows audio sessions of a process and everything it started
// (Chrome plays sound from a child process). Used by the CHATGPT tile's orb, which grows while ChatGPT talks.
#pragma once
#include <windows.h>
#include <atomic>
#include <thread>

class ProcessAudioMeter {
public:
    ProcessAudioMeter();
    ~ProcessAudioMeter();
    void watch(DWORD rootPid);  // 0 stops watching
    float level() const { return level_; }  // 0..1, the newest peak
private:
    void run();
    std::atomic<DWORD> root_{0};
    std::atomic<float> level_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
