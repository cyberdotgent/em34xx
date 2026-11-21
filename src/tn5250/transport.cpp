// SPDX-License-Identifier: GPL-3.0-or-later
#include "tn5250/transport.hpp"

#include "net/socket.hpp"

namespace em34xx {
namespace tn5250 {

namespace {

class PlainTcpTransport : public Transport {
public:
    bool connect(const std::string& host, uint16_t port, std::string& error) override {
        return sock_.connect(host, port, error);
    }
    int recv(uint8_t* buf, size_t len, int timeout_ms) override { return sock_.recv(buf, len, timeout_ms); }
    bool send(const uint8_t* buf, size_t len) override { return sock_.send_all(buf, len); }
    void close() override { sock_.close(); }
    std::string describe() const override { return "tcp " + sock_.peer_name(); }

private:
    TcpSocket sock_;
};

} // namespace

std::unique_ptr<Transport> make_transport(const Tn5250Config& cfg, std::string& error) {
    if (cfg.tls) return make_tls_transport(cfg, error);
    return std::unique_ptr<Transport>(new PlainTcpTransport());
}

} // namespace tn5250
} // namespace em34xx
