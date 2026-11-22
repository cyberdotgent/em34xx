// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal TCP sockets for the GDB stub and the TN5250 client (POSIX and
// Winsock). The calls block (up to their timeout); underneath, connected
// sockets are non-blocking so that connect and send can be bounded.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace em34xx {

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(TcpSocket&& o) noexcept;
    TcpSocket& operator=(TcpSocket&& o) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool valid() const;
    void close();

    // Listen on 127.0.0.1:port (or all interfaces when loopback_only=false).
    bool listen(uint16_t port, bool loopback_only, std::string& error);
    // Wait up to timeout_ms for a connection; returns an invalid socket on
    // timeout.
    TcpSocket accept(int timeout_ms);
    // Connect to host:port (name or address, IPv4 or IPv6). Waits up to
    // timeout_ms per resolved address; a `cancel` flag that becomes true
    // abandons the attempt within about 100 ms. Name resolution itself is
    // neither bounded nor cancellable.
    static constexpr int kConnectTimeoutMs = 10000;
    bool connect(const std::string& host, uint16_t port, std::string& error,
                 const std::atomic<bool>* cancel = nullptr, int timeout_ms = kConnectTimeoutMs);

    // Wait up to timeout_ms for data. Returns >0 bytes read, 0 on timeout,
    // -1 on error or disconnect.
    int recv(void* buf, size_t len, int timeout_ms);
    // Send everything, or fail: on error, or when the peer accepts nothing
    // for kSendStallTimeoutMs.
    static constexpr int kSendStallTimeoutMs = 15000;
    bool send_all(const void* buf, size_t len);

    std::string peer_name() const { return peer_; }

private:
    explicit TcpSocket(intptr_t fd) : fd_(fd) {}
    intptr_t fd_ = -1;
    std::string peer_;
};

} // namespace em34xx
