// REELS tile input: Chrome DevTools Protocol over a local WebSocket (WinHTTP), so taps and
// swipes on the tablet reach the Instagram page in Edge while Echo VR keeps the focus.
#include "reels.h"
#include "host.h"
#include <winhttp.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// GET http://127.0.0.1:<port><path>; empty on failure.
std::string httpGet(HINTERNET session, int port, const wchar_t* path) {
    std::string body;
    HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", INTERNET_PORT(port), 0);
    if (!connection) return body;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available) {
            std::vector<char> chunk(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) break;
            body.append(chunk.data(), read);
        }
    }
    if (request) WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    return body;
}

// The WebSocket path ("/devtools/page/<id>") of the first page target in /json/list.
std::wstring firstPagePath(const std::string& list) {
    for (size_t at = list.find("\"type\": \"page\""); at != std::string::npos; at = list.find("\"type\": \"page\"", at + 1)) {
        size_t start = list.rfind('{', at);
        size_t end = list.find('}', at);
        std::string entry = list.substr(start, end - start);
        size_t url = entry.find("ws://");
        if (url == std::string::npos) continue;
        size_t path = entry.find("/devtools/", url);
        size_t quote = entry.find('"', path);
        if (path == std::string::npos || quote == std::string::npos) continue;
        std::string p = entry.substr(path, quote - path);
        return std::wstring(p.begin(), p.end());
    }
    return L"";
}

}  // namespace

bool CdpInput::ensure() {
    if (socket_ && !broken_) return true;
    uint64_t now = GetTickCount64();
    if (now - lastAttempt_ < 1000) return false;  // Edge may still be starting
    lastAttempt_ = now;
    disconnect();

    session_ = WinHttpOpen(L"EchoArcade", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) return false;
    std::wstring path = firstPagePath(httpGet(session_, port_, L"/json/list"));
    if (path.empty()) { hostLog("reels: no Edge page on DevTools port %d yet", port_); return false; }

    connection_ = WinHttpConnect(session_, L"127.0.0.1", INTERNET_PORT(port_), 0);
    HINTERNET request = connection_ ? WinHttpOpenRequest(connection_, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    bool ok = request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
              WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok) socket_ = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (request) WinHttpCloseHandle(request);
    if (!socket_) { hostLog("reels: DevTools WebSocket upgrade failed (%lu)", GetLastError()); disconnect(); return false; }
    broken_ = false;
    reader_ = std::thread(&CdpInput::reader, this);
    hostLog("reels: connected to Edge page %ls", path.c_str());
    return true;
}

void CdpInput::reader() {
    std::vector<char> buffer(64 * 1024);
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (WinHttpWebSocketReceive(socket_, buffer.data(), DWORD(buffer.size()), &read, &type) != NO_ERROR ||
            type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            broken_ = true;
            return;
        }
    }
}

void CdpInput::disconnect() {
    if (socket_) WinHttpWebSocketClose(socket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    if (reader_.joinable()) reader_.join();
    if (socket_) WinHttpCloseHandle(socket_);
    if (connection_) WinHttpCloseHandle(connection_);
    if (session_) WinHttpCloseHandle(session_);
    socket_ = connection_ = session_ = nullptr;
}

bool CdpInput::sendJson(const std::string& json) {
    if (!ensure()) return false;
    if (WinHttpWebSocketSend(socket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(json.data()), DWORD(json.size())) != NO_ERROR) {
        broken_ = true;
        return false;
    }
    return true;
}

// The window's client area in CSS pixels (Edge scales pages by the window's DPI).
void CdpInput::cssPoint(HWND window, float x, float y, int& cx, int& cy) const {
    RECT r{0, 0, SCREEN_W, SCREEN_H};
    if (window) GetClientRect(window, &r);
    float scale = window ? 96.f / float(GetDpiForWindow(window) ? GetDpiForWindow(window) : 96) : 1.f;
    cx = int(x * (r.right - r.left) * scale);
    cy = int(y * (r.bottom - r.top) * scale);
}

void CdpInput::tap(HWND window, float x, float y) {
    std::lock_guard<std::mutex> lock(mutex_);
    int cx, cy;
    cssPoint(window, x, y, cx, cy);
    char json[512];
    const char* types[] = {"mouseMoved", "mousePressed", "mouseReleased"};
    for (const char* type : types) {
        snprintf(json, sizeof(json),
                 R"({"id":%d,"method":"Input.dispatchMouseEvent","params":{"type":"%s","x":%d,"y":%d,"button":"left","buttons":%d,"clickCount":1}})",
                 nextId_++, type, cx, cy, strcmp(type, "mousePressed") == 0 ? 1 : 0);
        if (!sendJson(json)) return;
    }
    hostLog("reels: tap at %d,%d (css)", cx, cy);
}

void CdpInput::wheel(HWND window, float x, float y, int deltaY) {
    std::lock_guard<std::mutex> lock(mutex_);
    int cx, cy;
    cssPoint(window, x, y, cx, cy);
    char json[512];
    snprintf(json, sizeof(json),
             R"({"id":%d,"method":"Input.dispatchMouseEvent","params":{"type":"mouseWheel","x":%d,"y":%d,"deltaX":0,"deltaY":%d}})",
             nextId_++, cx, cy, deltaY);
    if (sendJson(json)) hostLog("reels: wheel %d at %d,%d", deltaY, cx, cy);
}

void CdpInput::key(const char* key, const char* code, int virtualKey) {
    std::lock_guard<std::mutex> lock(mutex_);
    char json[512];
    const char* types[] = {"rawKeyDown", "keyUp"};
    for (const char* type : types) {
        snprintf(json, sizeof(json),
                 R"({"id":%d,"method":"Input.dispatchKeyEvent","params":{"type":"%s","key":"%s","code":"%s","windowsVirtualKeyCode":%d}})",
                 nextId_++, type, key, code, virtualKey);
        if (!sendJson(json)) return;
    }
}
