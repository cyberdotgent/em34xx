#include "net/socket.hpp"

#include "util/strings.hpp"

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
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kInvalid = -1;
static void close_sock(sock_t s) { ::close(s); }
static bool ensure_init() { return true; }
#endif

namespace tn34xx {

namespace {

sock_t S(intptr_t fd) { return static_cast<sock_t>(fd); }

// Returns 1 if readable, 0 on timeout, -1 on error.
int wait_readable(sock_t s, int timeout_ms) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(0, &set, nullptr, nullptr, timeout_ms < 0 ? nullptr : &tv);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
#else
    pollfd p{};
    p.fd = s;
    p.events = POLLIN;
    int r = ::poll(&p, 1, timeout_ms);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
#endif
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
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
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
    if (!valid() || wait_readable(S(fd_), timeout_ms) <= 0) return TcpSocket();
    sockaddr_in peer{};
    socklen_t len = sizeof peer;
    sock_t c = ::accept(S(fd_), reinterpret_cast<sockaddr*>(&peer), &len);
    if (c == kInvalid) return TcpSocket();
    int one = 1;
    ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    TcpSocket out(static_cast<intptr_t>(c));
    char buf[64] = {};
    ::inet_ntop(AF_INET, &peer.sin_addr, buf, sizeof buf);
    out.peer_ = format("%s:%u", buf, ntohs(peer.sin_port));
    return out;
}

int TcpSocket::recv(void* buf, size_t len, int timeout_ms) {
    if (!valid()) return -1;
    int w = wait_readable(S(fd_), timeout_ms);
    if (w <= 0) return w;
    int n = int(::recv(S(fd_), static_cast<char*>(buf), int(len), 0));
    return n > 0 ? n : -1;
}

bool TcpSocket::send_all(const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        int n = int(::send(S(fd_), p, int(len), 0));
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

} // namespace tn34xx
