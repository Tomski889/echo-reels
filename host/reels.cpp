// REELS tile input: Chrome DevTools Protocol over a local WebSocket (WinHTTP), so taps and
// swipes on the tablet reach the Instagram page in Edge while Echo VR keeps the focus.
#include "reels.h"
#include "host.h"
#include <winhttp.h>
#include <algorithm>
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
    for (auto& js : startupScripts_) sendStartupScript(js);
    return true;
}

int CdpInput::loginShown(int port) {
    HINTERNET session = WinHttpOpen(L"EchoArcade", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return -1;
    WinHttpSetTimeouts(session, 1000, 1000, 1000, 2000);
    std::string list = httpGet(session, port, L"/json/list");
    WinHttpCloseHandle(session);
    size_t at = list.find("\"type\": \"page\"");  // the page firstPagePath() connects to
    if (at == std::string::npos) return -1;
    size_t start = list.rfind('{', at), end = list.find('}', at);
    if (start == std::string::npos || end == std::string::npos) return -1;
    std::string entry = list.substr(start, end - start);
    // Instagram's, TikTok's and ChatGPT's sign-in pages
    for (const char* marker : {"/accounts/login", "/accounts/onetap", "/accounts/emailsignup", "/challenge", "/two_factor", "\"title\": \"Login",
                               "tiktok.com/login", "tiktok.com/signup", "\"title\": \"Log in",
                               "auth.openai.com", "chatgpt.com/auth/login"})
        if (entry.find(marker) != std::string::npos) return 1;
    return 0;
}

void CdpInput::reader() {
    std::vector<char> buffer(64 * 1024);
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (WinHttpWebSocketReceive(socket_, buffer.data(), DWORD(buffer.size()), &read, &type) != NO_ERROR ||
            type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            broken_ = true;
            replyReady_.notify_all();
            return;
        }
        std::lock_guard<std::mutex> lock(replyMutex_);
        if (waitId_ && type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            std::string message(buffer.data(), read);
            if (message.rfind("{\"id\":" + std::to_string(waitId_) + ",", 0) == 0) {
                reply_ = std::move(message);
                waitId_ = 0;
                replyReady_.notify_all();
            }
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

namespace {
std::string jsonString(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out + "\"";
}
}  // namespace

bool CdpInput::evaluate(const std::string& js) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sendJson("{\"id\":" + std::to_string(nextId_++) + ",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":" + jsonString(js) + "}}");
}

// The string value in a Runtime.evaluate reply: ..."result":{"type":"string","value":"..."}
static std::string replyString(const std::string& reply) {
    const std::string marker = "\"type\":\"string\",\"value\":\"";
    size_t at = reply.find(marker);
    if (at == std::string::npos) return "";
    std::string out;
    for (size_t i = at + marker.size(); i < reply.size(); i++) {
        char c = reply[i];
        if (c == '"') return out;
        if (c != '\\' || i + 1 >= reply.size()) { out += c; continue; }
        char e = reply[++i];
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': case 'f': break;
            case 'u': {  // \uXXXX as UTF-8
                if (i + 4 >= reply.size()) return "";
                unsigned code = std::stoul(reply.substr(i + 1, 4), nullptr, 16);
                i += 4;
                if (code < 0x80) out += char(code);
                else if (code < 0x800) { out += char(0xc0 | code >> 6); out += char(0x80 | (code & 0x3f)); }
                else { out += char(0xe0 | code >> 12); out += char(0x80 | (code >> 6 & 0x3f)); out += char(0x80 | (code & 0x3f)); }
                break;
            }
            default: out += e;
        }
    }
    return "";
}

std::string CdpInput::evaluateString(const std::string& js, DWORD timeoutMs) {
    int id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = nextId_++;
        {
            std::lock_guard<std::mutex> reply(replyMutex_);
            waitId_ = id;
            reply_.clear();
        }
        if (!sendJson("{\"id\":" + std::to_string(id) + ",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":" + jsonString(js) +
                      ",\"returnByValue\":true}}")) {
            std::lock_guard<std::mutex> reply(replyMutex_);
            waitId_ = 0;
            return "";
        }
    }
    std::unique_lock<std::mutex> reply(replyMutex_);
    replyReady_.wait_for(reply, std::chrono::milliseconds(timeoutMs), [&] { return waitId_ != id || broken_; });
    if (waitId_ == id) { waitId_ = 0; return ""; }
    return replyString(reply_);
}

bool CdpInput::sendStartupScript(const std::string& js) {
    return sendJson("{\"id\":" + std::to_string(nextId_++) + ",\"method\":\"Page.addScriptToEvaluateOnNewDocument\",\"params\":{\"source\":" +
                    jsonString(js) + "}}");
}

bool CdpInput::addStartupScript(const std::string& js) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(startupScripts_.begin(), startupScripts_.end(), js) == startupScripts_.end()) startupScripts_.push_back(js);
    return sendStartupScript(js);
}

bool CdpInput::browserCommand(const std::string& json) {
    HINTERNET session = WinHttpOpen(L"EchoArcade", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    // /json/version: "webSocketDebuggerUrl": "ws://127.0.0.1:<port>/devtools/browser/<id>"
    std::string version = httpGet(session, port_, L"/json/version");
    size_t at = version.find("/devtools/browser/"), end = at == std::string::npos ? at : version.find('"', at);
    bool sent = false;
    if (end != std::string::npos) {
        std::string p = version.substr(at, end - at);
        std::wstring path(p.begin(), p.end());
        HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", INTERNET_PORT(port_), 0);
        HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
        HINTERNET socket = nullptr;
        if (request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
            WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr))
            socket = WinHttpWebSocketCompleteUpgrade(request, 0);
        if (request) WinHttpCloseHandle(request);
        if (socket) {
            sent = WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(json.data()), DWORD(json.size())) == NO_ERROR;
            char reply[4096];
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            if (sent) WinHttpWebSocketReceive(socket, reply, sizeof(reply), &read, &type);  // wait for the answer before closing
            WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(socket);
            hostLog("reels: browser command %s: %.*s", sent ? "sent" : "failed", int(read < 200 ? read : 200), reply);
        }
        if (connection) WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    return sent;
}
