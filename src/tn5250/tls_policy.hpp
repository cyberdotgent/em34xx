// SPDX-License-Identifier: GPL-3.0-or-later
// Backend-neutral half of the TLS transport: what a backend learns about
// the server certificate, and the decision Tn5250Config asks for.
//
// The backends (tls_openssl.cpp on Linux and the BSDs, tls_schannel.cpp on
// Windows) never fail the handshake on a certificate problem. They finish
// it, describe the certificate in a PeerCertificate and call
// tls_accept_peer(), so that every mode logs the same lines, including the
// SHA-256 fingerprint in the form tls_cert_sha256 / --tn5250-tls-pin take.
#pragma once

#include "host/host_config.hpp"
#include "tn5250/tls_trust.hpp"
#include "tn5250/transport.hpp"
#include "util/fingerprint.hpp"

#include <string>

namespace em34xx {
namespace tn5250 {

// Applies tls_verify / tls_cert_sha256. Logs through `note`; returns false
// with `error` set when the connection must be dropped. When the
// configuration refuses the certificate and `ask` is set, `ask` decides
// instead (tls_trust.hpp); `port` goes into its request.
bool tls_accept_peer(const Tn5250Config& cfg, const std::string& host, const PeerCertificate& peer,
                     const Transport::LogFn& note, std::string& error, const TlsTrustHandler& ask = nullptr,
                     uint16_t port = 0);

// Connect to cfg.host:cfg.port with TLS, take the server certificate and
// close again: what a "fetch certificate" button needs. The certificate is
// returned whether or not it verifies (chain_ok / chain_error say), and no
// trust handler is asked. Blocks for the connection and the handshake.
bool tls_fetch_certificate(const Tn5250Config& cfg, PeerCertificate& out, std::string& error);

// "TLS server certificate SHA-256 fingerprint: AB:CD:..." and the like.
std::string tls_fingerprint_line(const Sha256Digest& digest);

// Logs the legacy-protocol warning when cfg allows versions before 1.2.
void tls_note_legacy(const Tn5250Config& cfg, const Transport::LogFn& note);

// The TLS backend compiled in: "openssl", "schannel", or nullptr when this
// build has no TLS (make_tls_transport then always fails).
const char* tls_backend_name();

} // namespace tn5250
} // namespace em34xx
