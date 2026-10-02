// How loud an app is right now: the peak of the Windows audio sessions of a process and everything it started
// (Chrome plays sound from a child process). Used by the CHATGPT tile's orb, which grows while ChatGPT talks.
#pragma once
#include <windows.h>
#include <atomic>
#include <thread>

// Mutes or unmutes a process's microphone input (its capture sessions on every active recording device). Other apps keep
// hearing the microphone. Returns how many sessions were changed (0: the process is not recording right now).
int setCaptureMute(DWORD pid, bool mute);
int setCaptureMuteTree(DWORD rootPid, bool mute);  // the same for a process and everything it started (Chrome)
DWORD findProcess(const wchar_t* exe);  // the first process with that file name, or 0

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
