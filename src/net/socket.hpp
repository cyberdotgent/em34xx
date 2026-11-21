// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal blocking TCP sockets for the GDB stub and the TN5250 client
// (POSIX and Winsock).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tn34xx {

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
    // Connect to host:port (name or address, IPv4 or IPv6). Blocking.
    bool connect(const std::string& host, uint16_t port, std::string& error);

    // Wait up to timeout_ms for data. Returns >0 bytes read, 0 on timeout,
    // -1 on error or disconnect.
    int recv(void* buf, size_t len, int timeout_ms);
    bool send_all(const void* buf, size_t len);

    std::string peer_name() const { return peer_; }

private:
    explicit TcpSocket(intptr_t fd) : fd_(fd) {}
    intptr_t fd_ = -1;
    std::string peer_;
};

} // namespace tn34xx
