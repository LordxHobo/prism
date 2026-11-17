// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The filtering proxy itself. See Blocker.h.

#include <winsock2.h>
#include <ws2tcpip.h>

#include "Blocker.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>

namespace blocker {

namespace {

constexpr size_t kMaxHeaderBytes = 64 * 1024;

class Socket {
public:
    explicit Socket(SOCKET s = INVALID_SOCKET) : m_socket(s) { }
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    SOCKET get() const { return m_socket; }
    void reset(SOCKET s) { close(); m_socket = s; }
    void close()
    {
        if (m_socket != INVALID_SOCKET)
            closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    explicit operator bool() const { return m_socket != INVALID_SOCKET; }
private:
    SOCKET m_socket;
};

bool sendAll(SOCKET s, const char* data, size_t length)
{
    while (length) {
        int sent = send(s, data, static_cast<int>(std::min<size_t>(length, 1 << 20)), 0);
        if (sent <= 0)
            return false;
        data += sent;
        length -= static_cast<size_t>(sent);
    }
    return true;
}

bool sendText(SOCKET s, const std::string& text)
{
    return sendAll(s, text.data(), text.size());
}

// Connects to host:port with a timeout. Tries every address the name has.
SOCKET connectTo(const std::string& host, const std::string& port)
{
    addrinfo hints = { };
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &results) || !results)
        return INVALID_SOCKET;

    SOCKET connected = INVALID_SOCKET;
    for (addrinfo* address = results; address && connected == INVALID_SOCKET; address = address->ai_next) {
        SOCKET s = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        int result = connect(s, address->ai_addr, static_cast<int>(address->ai_addrlen));
        bool ok = result == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval timeout = { 15, 0 };
            // (Windows ignores select's first argument; other systems need it.)
            if (select(static_cast<int>(s) + 1, nullptr, &writable, &failed, &timeout) > 0 && FD_ISSET(s, &writable)) {
                int error = 0;
                int size = sizeof(error);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size);
                ok = error == 0;
            }
        }
        if (ok) {
            nonBlocking = 0;
            ioctlsocket(s, FIONBIO, &nonBlocking);
            BOOL noDelay = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            connected = s;
        } else
            closesocket(s);
    }
    freeaddrinfo(results);
    return connected;
}

// Copies bytes both ways until either side closes.
void relay(SOCKET a, SOCKET b)
{
    std::unique_ptr<char[]> buffer(new char[64 * 1024]);
    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(a, &readable);
        FD_SET(b, &readable);
        if (select(static_cast<int>(std::max(a, b)) + 1, &readable, nullptr, nullptr, nullptr) <= 0)
            return;
        for (SOCKET from : { a, b }) {
            if (!FD_ISSET(from, &readable))
                continue;
            SOCKET to = from == a ? b : a;
            int received = recv(from, buffer.get(), 64 * 1024, 0);
            if (received <= 0 || !sendAll(to, buffer.get(), static_cast<size_t>(received)))
                return;
        }
    }
}

// Splits "host:port" / "[v6]:port". Returns false if malformed.
bool splitHostPort(const std::string& text, std::string& host, std::string& port, const char* defaultPort)
{
    if (text.empty())
        return false;
    if (text[0] == '[') {
        size_t close = text.find(']');
        if (close == std::string::npos)
            return false;
        host = text.substr(1, close - 1);
        port = close + 2 <= text.size() && text[close + 1] == ':' ? text.substr(close + 2) : defaultPort;
    } else {
        size_t colon = text.rfind(':');
        if (colon != std::string::npos && text.find(':') == colon) {
            host = text.substr(0, colon);
            port = text.substr(colon + 1);
        } else {
            host = text;
            port = defaultPort;
        }
    }
    return !host.empty() && !port.empty() && port.size() <= 5
        && std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool startsWithNoCase(const std::string& text, const char* prefix)
{
    size_t length = strlen(prefix);
    if (text.size() < length)
        return false;
    for (size_t i = 0; i < length; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

void refuse(SOCKET client, const std::string& status)
{
    sendText(client, "HTTP/1.1 " + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

void handleClient(SOCKET rawClient)
{
    Socket client(rawClient);
    BOOL noDelay = TRUE;
    setsockopt(client.get(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    // Read the request head.
    std::string head;
    char buffer[8192];
    size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos) {
        int received = recv(client.get(), buffer, sizeof(buffer), 0);
        if (received <= 0)
            return;
        head.append(buffer, static_cast<size_t>(received));
        headEnd = head.find("\r\n\r\n");
        if (head.size() > kMaxHeaderBytes)
            return refuse(client.get(), "431 Request Header Fields Too Large");
    }
    std::string leftover = head.substr(headEnd + 4);
    head.resize(headEnd + 4);

    size_t lineEnd = head.find("\r\n");
    std::string requestLine = head.substr(0, lineEnd);
    size_t space1 = requestLine.find(' ');
    size_t space2 = requestLine.find(' ', space1 + 1);
    if (space1 == std::string::npos || space2 == std::string::npos)
        return refuse(client.get(), "400 Bad Request");
    std::string method = requestLine.substr(0, space1);
    std::string target = requestLine.substr(space1 + 1, space2 - space1 - 1);

    std::string host, port;
    std::string forwardHead;
    if (method == "CONNECT") {
        // HTTPS (and secure WebSockets): only the host is visible.
        if (!splitHostPort(target, host, port, "443"))
            return refuse(client.get(), "400 Bad Request");
    } else {
        // Plain http://host/path request.
        if (!startsWithNoCase(target, "http://"))
            return refuse(client.get(), "400 Bad Request");
        std::string rest = target.substr(7);
        size_t slash = rest.find_first_of("/?#");
        std::string authority = rest.substr(0, slash);
        std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
        if (path[0] != '/')
            path = "/" + path;
        size_t at = authority.rfind('@');
        if (at != std::string::npos)
            authority = authority.substr(at + 1);
        if (!splitHostPort(authority, host, port, "80"))
            return refuse(client.get(), "400 Bad Request");

        // Rewrite for the origin server: path-only request line, one request
        // per connection, no proxy headers.
        forwardHead = method + " " + path + requestLine.substr(space2) + "\r\n";
        size_t position = lineEnd + 2;
        while (position < head.size()) {
            size_t end = head.find("\r\n", position);
            if (end == std::string::npos || end == position)
                break;
            std::string line = head.substr(position, end - position);
            position = end + 2;
            if (startsWithNoCase(line, "proxy-connection:") || startsWithNoCase(line, "proxy-authorization:") || startsWithNoCase(line, "connection:") || startsWithNoCase(line, "keep-alive:"))
                continue;
            forwardHead += line + "\r\n";
        }
        forwardHead += "Connection: close\r\n\r\n";
    }

    if (isBlocked(host)) {
        recordBlocked();
        return refuse(client.get(), "403 Blocked by Prism");
    }

    Socket upstream(connectTo(host, port));
    if (!upstream)
        return refuse(client.get(), "502 Bad Gateway");

    if (method == "CONNECT") {
        if (!sendText(client.get(), "HTTP/1.1 200 Connection Established\r\n\r\n"))
            return;
    } else if (!sendText(upstream.get(), forwardHead))
        return;
    if (!leftover.empty() && !sendText(upstream.get(), leftover))
        return;
    relay(client.get(), upstream.get());
}

std::once_flag s_startOnce;
std::atomic<uint16_t> s_port { 0 };

void serve(SOCKET listener)
{
    for (;;) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (WSAGetLastError() == WSAEINTR || WSAGetLastError() == WSAECONNRESET)
                continue;
            Sleep(50);
            continue;
        }
        std::thread(handleClient, client).detach();
    }
}

} // namespace

uint16_t startProxy()
{
    std::call_once(s_startOnce, [] {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data))
            return;
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET)
            return;
        // Only this PC can use the proxy.
        sockaddr_in address = { };
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0; // any free port
        BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener, SOMAXCONN)) {
            closesocket(listener);
            return;
        }
        int size = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size);
        s_port = ntohs(address.sin_port);
        std::thread(serve, listener).detach();
    });
    return s_port.load();
}

} // namespace blocker
