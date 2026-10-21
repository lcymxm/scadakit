#include "scada/socket.hpp"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace scada {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalid = INVALID_SOCKET;
NativeSocket to_native(std::uintptr_t handle) { return static_cast<NativeSocket>(handle); }
std::uintptr_t to_handle(NativeSocket socket) { return static_cast<std::uintptr_t>(socket); }
void close_native(NativeSocket socket) { ::closesocket(socket); }
int last_error() { return WSAGetLastError(); }
bool would_block(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalid = -1;
NativeSocket to_native(std::uintptr_t handle) { return static_cast<NativeSocket>(handle); }
std::uintptr_t to_handle(NativeSocket socket) { return static_cast<std::uintptr_t>(socket); }
void close_native(NativeSocket socket) { ::close(socket); }
int last_error() { return errno; }
bool would_block(int error) { return error == EWOULDBLOCK || error == EINPROGRESS; }
#endif

void set_nonblocking(NativeSocket socket, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    ::ioctlsocket(socket, FIONBIO, &mode);
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    if (flags < 0) {
        return;
    }
    ::fcntl(socket, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

void set_nodelay(NativeSocket socket) {
    const int one = 1;
    ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                 sizeof(one));
}

// Waits for readability (or writability) on one socket.
bool wait_for(NativeSocket socket, bool for_write, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    const int ready = ::select(0, for_write ? nullptr : &set, for_write ? &set : nullptr, nullptr,
                               &timeout);
#else
    const int ready = ::select(socket + 1, for_write ? nullptr : &set, for_write ? &set : nullptr,
                               nullptr, &timeout);
#endif
    return ready > 0;
}

}  // namespace

namespace detail {

void ensure_networking() {
#ifdef _WIN32
    static const bool started = []() {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

}  // namespace detail

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = detail::kInvalidSocket;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = detail::kInvalidSocket;
    }
    return *this;
}

void TcpSocket::close() {
    if (handle_ != detail::kInvalidSocket) {
        close_native(to_native(handle_));
        handle_ = detail::kInvalidSocket;
    }
}

TcpSocket TcpSocket::connect_to(const std::string& host, std::uint16_t port, int timeout_ms,
                                std::string& error) {
    detail::ensure_networking();

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
        error = "cannot resolve host '" + host + "'";
        return TcpSocket{};
    }

    NativeSocket socket = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (socket == kInvalid) {
        ::freeaddrinfo(result);
        error = "cannot create socket";
        return TcpSocket{};
    }

    // Non-blocking connect so a dead device cannot hang the polling loop.
    set_nonblocking(socket, true);
    const int rc = ::connect(socket, result->ai_addr, static_cast<int>(result->ai_addrlen));
    ::freeaddrinfo(result);

    if (rc != 0 && !would_block(last_error())) {
        close_native(socket);
        error = "connection refused by " + host + ":" + service;
        return TcpSocket{};
    }

    if (rc != 0) {
        if (!wait_for(socket, true, timeout_ms)) {
            close_native(socket);
            error = "connection to " + host + ":" + service + " timed out";
            return TcpSocket{};
        }
        int so_error = 0;
        socklen_t length = sizeof(so_error);
        ::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &length);
        if (so_error != 0) {
            close_native(socket);
            error = "connection to " + host + ":" + service + " failed";
            return TcpSocket{};
        }
    }

    set_nonblocking(socket, false);
    set_nodelay(socket);
    return TcpSocket{to_handle(socket)};
}

int TcpSocket::send_bytes(const std::uint8_t* data, std::size_t size) {
    if (!valid()) {
        return -1;
    }
    std::size_t sent = 0;
    while (sent < size) {
        const int rc = ::send(to_native(handle_), reinterpret_cast<const char*>(data + sent),
                              static_cast<int>(size - sent), 0);
        if (rc <= 0) {
            return -1;
        }
        sent += static_cast<std::size_t>(rc);
    }
    return static_cast<int>(sent);
}

int TcpSocket::receive(std::uint8_t* buffer, std::size_t size, int timeout_ms) {
    if (!valid()) {
        return -1;
    }
    if (!wait_readable(timeout_ms)) {
        return -1;
    }
    return ::recv(to_native(handle_), reinterpret_cast<char*>(buffer), static_cast<int>(size), 0);
}

bool TcpSocket::receive_exactly(std::uint8_t* buffer, std::size_t size, int timeout_ms) {
    std::size_t received = 0;
    while (received < size) {
        const int rc = receive(buffer + received, size - received, timeout_ms);
        if (rc <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(rc);
    }
    return true;
}

bool TcpSocket::wait_readable(int timeout_ms) const {
    if (!valid()) {
        return false;
    }
    return wait_for(to_native(handle_), false, timeout_ms);
}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_), local_port_(other.local_port_) {
    other.handle_ = detail::kInvalidSocket;
    other.local_port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        local_port_ = other.local_port_;
        other.handle_ = detail::kInvalidSocket;
        other.local_port_ = 0;
    }
    return *this;
}

void TcpListener::close() {
    if (handle_ != detail::kInvalidSocket) {
        close_native(to_native(handle_));
        handle_ = detail::kInvalidSocket;
    }
    local_port_ = 0;
}

TcpListener TcpListener::bind_to(const std::string& address, std::uint16_t port,
                                 std::string& error) {
    detail::ensure_networking();

    const std::string host = address.empty() ? "127.0.0.1" : address;

    NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalid) {
        error = "cannot create listening socket";
        return TcpListener{};
    }

    const int one = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                 sizeof(one));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &local.sin_addr) != 1) {
        close_native(socket);
        error = "invalid bind address '" + host + "'";
        return TcpListener{};
    }

    if (::bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        close_native(socket);
        error = "cannot bind " + host + ":" + std::to_string(port);
        return TcpListener{};
    }
    if (::listen(socket, 8) != 0) {
        close_native(socket);
        error = "cannot listen on " + host + ":" + std::to_string(port);
        return TcpListener{};
    }

    socklen_t length = sizeof(local);
    ::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length);

    TcpListener listener;
    listener.handle_ = to_handle(socket);
    listener.local_port_ = ntohs(local.sin_port);
    return listener;
}

TcpSocket TcpListener::accept(int timeout_ms) {
    if (!valid()) {
        return TcpSocket{};
    }
    if (!wait_for(to_native(handle_), false, timeout_ms)) {
        return TcpSocket{};
    }
    const NativeSocket client = ::accept(to_native(handle_), nullptr, nullptr);
    if (client == kInvalid) {
        return TcpSocket{};
    }
    set_nodelay(client);
    return TcpSocket{to_handle(client)};
}

}  // namespace scada
