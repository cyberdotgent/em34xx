// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/socket.hpp"

#include "util/strings.hpp"

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static const sock_t kInvalid = INVALID_SOCKET;
static void close_sock(sock_t s) { closesocket(s); }
static bool ensure_init() {
    static bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    return ok;
}
static int last_error() { return WSAGetLastError(); }
static bool would_block(int e) { return e == WSAEWOULDBLOCK; }
static bool connect_pending(int e) { return e == WSAEWOULDBLOCK; }
static bool interrupted(int) { return false; }
static bool set_nonblocking(sock_t s) {
    u_long on = 1;
    return ioctlsocket(s, static_cast<long>(FIONBIO), &on) == 0;
}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kInvalid = -1;
static void close_sock(sock_t s) { ::close(s); }
static bool ensure_init() { return true; }
static int last_error() { return errno; }
static bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
// An interrupted connect() carries on in the background, like EINPROGRESS.
static bool connect_pending(int e) { return e == EINPROGRESS || e == EINTR; }
static bool interrupted(int e) { return e == EINTR; }
static bool set_nonblocking(sock_t s) {
    const int fl = ::fcntl(s, F_GETFL, 0);
    return fl >= 0 && ::fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
}
#endif

// A peer that went away must fail the send, not raise SIGPIPE.
#ifdef MSG_NOSIGNAL
static const int kSendFlags = MSG_NOSIGNAL;
#else
static const int kSendFlags = 0;
#endif

namespace em34xx {

namespace {

sock_t S(intptr_t fd) { return static_cast<sock_t>(fd); }

// Wait for the socket to become readable (or writable, when `for_write`; that
// includes a connect that finished or failed). Returns 1 when ready, 0 on
// timeout, -1 on error. A negative timeout waits forever.
int wait_ready(sock_t s, bool for_write, int timeout_ms) {
#ifdef _WIN32
    fd_set set, failed;
    FD_ZERO(&set);
    FD_SET(s, &set);
    // Winsock reports a failed connect in the exception set only.
    FD_ZERO(&failed);
    FD_SET(s, &failed);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(0, for_write ? nullptr : &set, for_write ? &set : nullptr, for_write ? &failed : nullptr,
                   timeout_ms < 0 ? nullptr : &tv);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
#else
    using steady = std::chrono::steady_clock;
    const auto deadline = steady::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        pollfd p{};
        p.fd = s;
        p.events = for_write ? POLLOUT : POLLIN;
        int r = ::poll(&p, 1, timeout_ms);
        if (r >= 0) return r > 0 ? 1 : 0;
        if (errno != EINTR) return -1;
        // Interrupted by a signal: wait for what is left of the timeout.
        if (timeout_ms < 0) continue;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - steady::now()).count();
        timeout_ms = left > 0 ? int(left) : 0;
    }
#endif
}

void prepare_stream(sock_t s) {
    int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_NOSIGPIPE
    ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&one), sizeof one);
#endif
}

enum class ConnectResult { Connected, Failed, TimedOut, Cancelled };

// Connect a non-blocking socket, polling `cancel` every 100 ms.
ConnectResult connect_bounded(sock_t s, const sockaddr* addr, socklen_t len, const std::atomic<bool>* cancel,
                              int timeout_ms) {
    if (::connect(s, addr, len) == 0) return ConnectResult::Connected;
    if (!connect_pending(last_error())) return ConnectResult::Failed;
    using steady = std::chrono::steady_clock;
    const auto deadline = steady::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (cancel && cancel->load()) return ConnectResult::Cancelled;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - steady::now()).count();
        if (left <= 0) return ConnectResult::TimedOut;
        const int w = wait_ready(s, true, int(std::min<long long>(left, 100)));
        if (w < 0) return ConnectResult::Failed;
        if (w == 0) continue;
        int soerr = 0;
        socklen_t n = sizeof soerr;
        if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &n) != 0 || soerr != 0)
            return ConnectResult::Failed;
        return ConnectResult::Connected;
    }
}

} // namespace

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& o) noexcept : fd_(o.fd_), peer_(std::move(o.peer_)) { o.fd_ = -1; }

TcpSocket& TcpSocket::operator=(TcpSocket&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        peer_ = std::move(o.peer_);
        o.fd_ = -1;
    }
    return *this;
}

bool TcpSocket::valid() const { return fd_ != -1 && S(fd_) != kInvalid; }

void TcpSocket::close() {
    if (valid()) close_sock(S(fd_));
    fd_ = -1;
}

bool TcpSocket::listen(uint16_t port, bool loopback_only, std::string& error) {
    if (!ensure_init()) { error = "socket library initialisation failed"; return false; }
    sock_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalid) { error = "socket() failed"; return false; }
    int one = 1;
#ifdef _WIN32
    // Winsock's SO_REUSEADDR would let a second listener take a port that
    // is in use; a closed listener's port can be bound again without it.
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one);
#else
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close_sock(s);
        error = format("cannot bind TCP port %u", port);
        return false;
    }
    if (::listen(s, 1) != 0) {
        close_sock(s);
        error = "listen() failed";
        return false;
    }
    close();
    fd_ = static_cast<intptr_t>(s);
    return true;
}

TcpSocket TcpSocket::accept(int timeout_ms) {
    if (!valid() || wait_ready(S(fd_), false, timeout_ms) <= 0) return TcpSocket();
    sockaddr_in peer{};
    socklen_t len = sizeof peer;
    sock_t c = ::accept(S(fd_), reinterpret_cast<sockaddr*>(&peer), &len);
    if (c == kInvalid) return TcpSocket();
    if (!set_nonblocking(c)) {
        close_sock(c);
        return TcpSocket();
    }
    prepare_stream(c);
    TcpSocket out(static_cast<intptr_t>(c));
    char buf[64] = {};
    ::inet_ntop(AF_INET, &peer.sin_addr, buf, sizeof buf);
    out.peer_ = format("%s:%u", buf, ntohs(peer.sin_port));
    return out;
}

bool TcpSocket::connect(const std::string& host, uint16_t port, std::string& error, const std::atomic<bool>* cancel,
                        int timeout_ms) {
    if (!ensure_init()) { error = "socket library initialisation failed"; return false; }
    const auto cancelled = [&] { return cancel && cancel->load(); };
    if (cancelled()) { error = "connection attempt cancelled"; return false; }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const std::string service = format("%u", port);
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        if (res) ::freeaddrinfo(res);
        error = format("cannot resolve %s", host.c_str());
        return false;
    }
    error = format("cannot connect to %s:%u", host.c_str(), port);
    bool connected = false;
    for (addrinfo* ai = res; ai && !connected; ai = ai->ai_next) {
        if (cancelled()) { error = "connection attempt cancelled"; break; }
        sock_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kInvalid) continue;
        ConnectResult r = ConnectResult::Failed;
        if (set_nonblocking(s))
            r = connect_bounded(s, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen), cancel, timeout_ms);
        if (r != ConnectResult::Connected) {
            close_sock(s);
            if (r == ConnectResult::Cancelled) { error = "connection attempt cancelled"; break; }
            if (r == ConnectResult::TimedOut)
                error = format("cannot connect to %s:%u (timed out)", host.c_str(), port);
            continue;
        }
        prepare_stream(s);
        char buf[64] = {};
        if (ai->ai_family == AF_INET)
            ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr, buf, sizeof buf);
        else if (ai->ai_family == AF_INET6)
            ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(ai->ai_addr)->sin6_addr, buf, sizeof buf);
        close();
        fd_ = static_cast<intptr_t>(s);
        peer_ = format("%s:%u", buf, port);
        error.clear();
        connected = true;
    }
    ::freeaddrinfo(res);
    return connected;
}

int TcpSocket::recv(void* buf, size_t len, int timeout_ms) {
    if (!valid()) return -1;
    int w = wait_ready(S(fd_), false, timeout_ms);
    if (w <= 0) return w;
    for (;;) {
        int n = int(::recv(S(fd_), static_cast<char*>(buf), int(len), 0));
        if (n > 0) return n;
        if (n == 0) return -1;
        const int e = last_error();
        if (interrupted(e)) continue;
        // Readable but nothing to read after all: as good as a timeout.
        return would_block(e) ? 0 : -1;
    }
}

bool TcpSocket::send_all(const void* buf, size_t len) {
    if (!valid()) return false;
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        int n = int(::send(S(fd_), p, int(len), kSendFlags));
        if (n > 0) {
            p += n;
            len -= size_t(n);
            continue;
        }
        const int e = last_error();
        if (n < 0 && interrupted(e)) continue;
        // The send buffer is full: give the peer a bounded time to read.
        if (n < 0 && would_block(e) && wait_ready(S(fd_), true, kSendStallTimeoutMs) > 0) continue;
        return false;
    }
    return true;
}

} // namespace em34xx
