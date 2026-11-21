// SPDX-License-Identifier: GPL-3.0-or-later
// Byte transport under the TN5250 telnet layer: plain TCP, or TLS from the
// first byte (implicit TLS, port 992 by convention; no STARTTLS).
//
// TLS uses the host's own stack, chosen at configure time (TN34XX_TLS):
//   - tls_openssl.cpp: the system libssl/libcrypto (Linux, the BSDs;
//     OpenSSL 3 is Apache-2.0, 1.1.1 OpenSSL/SSLeay). Optional: without the
//     development files the build has no TLS.
//   - tls_schannel.cpp: Windows SChannel (SSPI) and CryptoAPI, so the
//     Windows certificate store is honoured. System DLLs only.
//   - tls_none.cpp: the stub for builds without TLS.
// The certificate decision (system store, insecure, pinned SHA-256) is
// shared: tls_policy.hpp.
#pragma once

#include "host/host_config.hpp"
#include "tn5250/tls_trust.hpp"
#include "util/log.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace tn34xx {
namespace tn5250 {

class Transport {
public:
    using LogFn = std::function<void(LogLevel level, const std::string& line)>;

    virtual ~Transport() = default;
    virtual bool connect(const std::string& host, uint16_t port, std::string& error) = 0;
    // Wait up to timeout_ms. Returns bytes read, 0 on timeout, -1 on
    // error or orderly close.
    virtual int recv(uint8_t* buf, size_t len, int timeout_ms) = 0;
    virtual bool send(const uint8_t* buf, size_t len) = 0;
    virtual void close() = 0;
    virtual std::string describe() const = 0;

    // Where connection diagnostics go (certificate details, fingerprints).
    void set_log(LogFn fn) { log_ = std::move(fn); }
    // Who is asked when the configuration refuses the server certificate
    // (TLS only; see tls_trust.hpp). Unset: the configured decision stands.
    void set_trust_handler(TlsTrustHandler fn) { trust_ = std::move(fn); }
    // The server certificate of the last TLS handshake (connect() may have
    // refused it), or null for plain TCP / before a handshake.
    const PeerCertificate* peer_certificate() const { return have_peer_ ? &peer_cert_ : nullptr; }

protected:
    void note(LogLevel level, const std::string& line) const {
        if (log_) log_(level, line);
    }
    const TlsTrustHandler& trust_handler() const { return trust_; }
    void set_peer_certificate(const PeerCertificate& p) {
        peer_cert_ = p;
        have_peer_ = true;
    }

private:
    LogFn log_;
    TlsTrustHandler trust_;
    PeerCertificate peer_cert_;
    bool have_peer_ = false;
};

// The transport the configuration asks for, or nullptr with `error` set.
std::unique_ptr<Transport> make_transport(const Tn5250Config& cfg, std::string& error);

// The TLS transport of the compiled-in backend (or nullptr with `error`
// when the build has none); make_transport() picks it when cfg.tls is set.
std::unique_ptr<Transport> make_tls_transport(const Tn5250Config& cfg, std::string& error);

} // namespace tn5250
} // namespace tn34xx
