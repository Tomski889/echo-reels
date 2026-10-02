#pragma once
// Watch party (party-server\worker.js): a WebSocket to the relay. The host sends what its REELS / TIKTOK page is
// playing; guests receive it and open the same video. arcade.ini [host] party_server = the relay's address.
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class PartyLink {
public:
    ~PartyLink() { stop(); }
    // code: 4 digits. Connects (and reconnects) on its own thread until stop().
    void start(const std::wstring& server, const std::string& code, bool host);
    void stop();
    bool active() const { return running_; }
    bool host() const { return host_; }
    bool connected() const { return socket_ != nullptr; }
    std::string code() const { std::lock_guard<std::mutex> lock(mutex_); return code_; }
    int guests() const { return guests_; }
    std::string error() const { std::lock_guard<std::mutex> lock(mutex_); return error_; }
    bool send(const std::string& text);     // host only
    bool take(std::string& text);           // guest: the newest message not taken yet

private:
    void run();
    bool connect(void*& session, void*& connection);  // HINTERNET
    void fail(const std::string& why);

    std::wstring server_;
    std::string code_, key_, latest_, error_;
    bool host_ = false, fresh_ = false;
    std::atomic<bool> running_{false};
    std::atomic<int> guests_{0};
    std::atomic<void*> socket_{nullptr};  // HINTERNET
    mutable std::mutex mutex_;
    std::mutex sendMutex_;
    std::thread thread_;
};
