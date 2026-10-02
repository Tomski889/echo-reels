// Watch party client: WinHTTP WebSocket to the relay (party-server\worker.js), reconnecting until stopped.
#include "party.h"
#include "host.h"
#include <winhttp.h>
#include <random>
#include <vector>

namespace {
std::string randomDigits(int count) {
    std::random_device random;
    std::string s;
    for (int i = 0; i < count; i++) s += char('0' + random() % 10);
    return s;
}
std::string randomKey() {
    std::random_device random;
    static const char hex[] = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 32; i++) s += hex[random() % 16];
    return s;
}
}  // namespace

void PartyLink::start(const std::wstring& server, const std::string& code, bool host) {
    stop();
    server_ = server;
    host_ = host;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        code_ = code.empty() ? randomDigits(4) : code;
        key_ = randomKey();
        latest_.clear();
        error_.clear();
        fresh_ = false;
    }
    guests_ = 0;
    running_ = true;
    thread_ = std::thread(&PartyLink::run, this);
}

void PartyLink::stop() {
    running_ = false;
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (HINTERNET s = socket_.exchange(nullptr)) WinHttpCloseHandle(s);  // ends the receive below
    }
    if (thread_.joinable()) thread_.join();
}

void PartyLink::fail(const std::string& why) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_ != why) hostLog("party: %s", why.c_str());
    error_ = why;
}

bool PartyLink::connect(void*& session, void*& connection) {
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t hostName[256] = {}, path[512] = {};
    parts.lpszHostName = hostName;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 512;
    if (!WinHttpCrackUrl(server_.c_str(), 0, 0, &parts)) { fail("party_server in arcade.ini is not an address"); return false; }
    bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    std::string code, key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        code = code_;
        key = key_;
    }
    std::wstring base = path;
    while (!base.empty() && base.back() == L'/') base.pop_back();
    std::string query = "/party/" + code + (host_ ? "?role=host&key=" + key : "?role=guest");
    std::wstring requestPath = base + std::wstring(query.begin(), query.end());

    session = WinHttpOpen(L"EchoArcade", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
    connection = WinHttpConnect(session, hostName, parts.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", requestPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool sent = request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr);
    DWORD error = sent ? 0 : GetLastError(), status = 0, size = sizeof(status);
    if (sent) WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    HINTERNET socket = status == 101 ? WinHttpWebSocketCompleteUpgrade(request, 0) : nullptr;
    if (request) WinHttpCloseHandle(request);
    if (!socket) {
        if (status == 409 && host_) {  // someone else hosts this code: pick another
            std::lock_guard<std::mutex> lock(mutex_);
            code_ = randomDigits(4);
        } else {
            fail(sent ? "relay answered " + std::to_string(status) : "no answer from the party server (" + std::to_string(error) + ")");
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (!running_) { WinHttpCloseHandle(socket); return false; }
        socket_ = socket;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
    }
    hostLog("party: connected as %s, code %s", host_ ? "host" : "guest", code.c_str());
    return true;
}

void PartyLink::run() {
    std::vector<char> buffer(4096);
    while (running_) {
        HINTERNET session = nullptr, connection = nullptr;
        if (connect(session, connection)) {
            std::string message;
            for (;;) {
                HINTERNET socket = socket_.load();
                DWORD read = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
                if (!socket || WinHttpWebSocketReceive(socket, buffer.data(), DWORD(buffer.size()), &read, &type) != NO_ERROR ||
                    type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                    break;
                message.append(buffer.data(), read);
                if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
                    if (message.size() > 16384) message.clear();
                    continue;
                }
                if (host_) {  // {"type":"count","guests":N}
                    size_t at = message.find("\"guests\":");
                    if (at != std::string::npos) guests_ = atoi(message.c_str() + at + 9);
                } else {
                    std::lock_guard<std::mutex> lock(mutex_);
                    latest_ = message;
                    fresh_ = true;
                }
                message.clear();
            }
            {
                std::lock_guard<std::mutex> lock(sendMutex_);
                if (HINTERNET s = socket_.exchange(nullptr)) WinHttpCloseHandle(s);
            }
            guests_ = 0;
            if (running_) fail("lost the party server, reconnecting");
        }
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
        for (int i = 0; i < 30 && running_; i++) Sleep(100);
    }
}

bool PartyLink::send(const std::string& text) {
    if (!host_) return false;
    std::lock_guard<std::mutex> lock(sendMutex_);
    HINTERNET socket = socket_.load();
    return socket && WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(text.data()), DWORD(text.size())) == NO_ERROR;
}

bool PartyLink::take(std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fresh_) return false;
    text = latest_;
    fresh_ = false;
    return true;
}
