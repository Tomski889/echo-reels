// Movie playback (mpv) and Plex browsing for ArcadeHost.
#pragma once
#include "host.h"
#include <atomic>
#include <functional>
#include <thread>

// ---- mpv JSON IPC (named pipe) ----
class MpvControl {
public:
    ~MpvControl();
    static std::wstring pipeName();
    void connect();                    // retries in the background until mpv's pipe exists
    void disconnect();
    bool connected() const { return connected_; }
    void command(const std::string& jsonArray);  // e.g. ["cycle","pause"]
    double position() const { return position_; }
    double duration() const { return duration_; }
    bool paused() const { return paused_; }
    double volume() const { return volume_; }
private:
    void readLoop();
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::thread reader_;
    std::mutex writeLock_;
    std::atomic<bool> running_{false}, connected_{false}, paused_{false};
    std::atomic<double> position_{0}, duration_{0}, volume_{100};
};

// ---- Plex (plex.tv PIN link + direct play from your server) ----
struct PlexItem {
    std::wstring title, subtitle;
    std::string key;         // browse path (/library/...) for folders
    std::string ratingKey;   // metadata id for playable items
    std::string partKey;     // /library/parts/... for playable items
    std::string type;        // movie, episode, show, season, library, ...
    double resumeSeconds = 0, durationSeconds = 0;
    bool playable() const { return !partKey.empty(); }
};

class Plex {
public:
    Plex();
    bool linked() const { return !token_.empty(); }
    bool serverReady() const { return !server_.empty(); }
    std::wstring serverName() const { return serverName_; }

    // All network calls block; ArcadeHost runs them on worker threads.
    bool startLink(std::wstring& code, std::string& pinId);  // shows a code for plex.tv/link
    bool pollLink(const std::string& pinId);                 // true once the user approved
    bool findServer(std::wstring& error);                    // picks a reachable server
    std::vector<PlexItem> libraries();                       // plus "Continue Watching"
    std::vector<PlexItem> children(const std::string& key);
    std::wstring streamUrl(const PlexItem& item) const;
    void timeline(const PlexItem& item, const char* state, double seconds);
    void unlink();

private:
    std::string get(const std::wstring& url, const char* method = "GET");
    void save();
    std::string clientId_, token_;
    std::wstring server_, serverName_;
    std::string serverToken_;
};

// Local video files and folders under the configured movie folders.
struct FileEntry { std::wstring name, path; bool folder; };
std::vector<FileEntry> listVideos(const std::wstring& folder);
bool isVideoFile(const std::wstring& path);
