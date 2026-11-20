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
#include "tn5250/transport.hpp"
#include "util/fingerprint.hpp"

#include <string>

namespace tn34xx {
namespace tn5250 {

struct PeerCertificate {
    bool have_digest = false;
    Sha256Digest sha256{};      // of the leaf certificate's DER encoding
    std::string subject;
    std::string issuer;
    std::string expires;        // "YYYY-MM-DD HH:MM:SS UTC"
    bool expired = false;
    bool chain_ok = false;      // chains to a trusted root and matches the host name
    std::string chain_error;    // why not, when !chain_ok
};

// Applies tls_verify / tls_cert_sha256. Logs through `note`; returns false
// with `error` set when the connection must be dropped.
bool tls_accept_peer(const Tn5250Config& cfg, const std::string& host, const PeerCertificate& peer,
                     const Transport::LogFn& note, std::string& error);

// "TLS server certificate SHA-256 fingerprint: AB:CD:..." and the like.
std::string tls_fingerprint_line(const Sha256Digest& digest);

// Logs the legacy-protocol warning when cfg allows versions before 1.2.
void tls_note_legacy(const Tn5250Config& cfg, const Transport::LogFn& note);

// The TLS backend compiled in: "openssl", "schannel", or nullptr when this
// build has no TLS (make_tls_transport then always fails).
const char* tls_backend_name();

} // namespace tn5250
} // namespace tn34xx
