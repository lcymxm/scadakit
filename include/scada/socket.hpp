// Minimal RAII TCP layer. The handle is kept as an integer so that no platform
// socket header leaks into the rest of the project.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace scada {

namespace detail {
// Winsock needs a process-wide start-up on Windows; harmless elsewhere.
void ensure_networking();
constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(-1);
}  // namespace detail

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool valid() const { return handle_ != detail::kInvalidSocket; }
    void close();

    // Connects with a timeout on the connect itself, which matters when the peer
    // is a device that has been switched off.
    static TcpSocket connect_to(const std::string& host, std::uint16_t port, int timeout_ms,
                                std::string& error);

    // Bytes sent, or -1.
    int send_bytes(const std::uint8_t* data, std::size_t size);
    // >0 bytes read, 0 when the peer closed, -1 on error or timeout.
    int receive(std::uint8_t* buffer, std::size_t size, int timeout_ms);

    // Reads exactly `size` bytes; false on timeout or shutdown.
    bool receive_exactly(std::uint8_t* buffer, std::size_t size, int timeout_ms);

    bool wait_readable(int timeout_ms) const;

    std::uintptr_t native_handle() const { return handle_; }

private:
    friend class TcpListener;
    explicit TcpSocket(std::uintptr_t handle) : handle_(handle) {}
    std::uintptr_t handle_ = detail::kInvalidSocket;
};

class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();

    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // Binds to `address` (empty means loopback). Port 0 asks the OS for a free
    // one, which is what the tests use.
    static TcpListener bind_to(const std::string& address, std::uint16_t port, std::string& error);

    // Blocks until a client arrives or the timeout expires. Returns an invalid
    // socket on timeout.
    TcpSocket accept(int timeout_ms);

    std::uint16_t local_port() const { return local_port_; }
    bool valid() const { return handle_ != detail::kInvalidSocket; }
    void close();

private:
    std::uintptr_t handle_ = detail::kInvalidSocket;
    std::uint16_t local_port_ = 0;
};

}  // namespace scada
