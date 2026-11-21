// The TLS trust decision hook: what a TLS backend learned about the server
// certificate, and how a front end is asked whether to trust one that did
// not verify.
//
// A TLS backend finishes the handshake whatever the certificate, fills a
// PeerCertificate and hands it to tls_accept_peer() (tls_policy.hpp). When
// the configuration refuses the certificate (system verification failed,
// or it does not match the pinned SHA-256) and a TlsTrustHandler is set,
// the handler is asked instead of failing at once. It runs on the session's
// I/O thread and may block (a GUI shows a dialog on its own thread and
// waits for the answer); the handshake is complete and idle meanwhile.
//
// Without a handler the behaviour is the configured one (the SDL front end,
// the monitor and the headless build keep working unchanged).
#pragma once

#include "util/fingerprint.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace tn34xx {
namespace tn5250 {

struct PeerCertificate {
    bool have_digest = false;
    Sha256Digest sha256{};      // of the leaf certificate's DER encoding
    std::string subject;
    std::string issuer;
    std::string valid_from;     // "YYYY-MM-DD HH:MM:SS UTC"
    std::string expires;        // "YYYY-MM-DD HH:MM:SS UTC"
    bool expired = false;
    bool chain_ok = false;      // chains to a trusted root and matches the host name
    std::string chain_error;    // why not, when !chain_ok
};

// Why the configuration refused a certificate.
enum class TlsTrustProblem {
    Unverified,      // system verification failed (chain_error says why)
    PinMismatch,     // tls_cert_sha256 is set and the certificate differs
};

struct TlsTrustRequest {
    std::string host;
    uint16_t port = 0;
    unsigned station = 0;       // Twinax station address of the session (bridge sessions)
    PeerCertificate peer;
    TlsTrustProblem problem = TlsTrustProblem::Unverified;
    std::string reason;         // one line for the user
};

enum class TlsTrustDecision {
    Reject,            // drop the connection (the configured behaviour)
    AcceptOnce,        // use this connection; the configuration is unchanged
    AcceptPermanently, // use it; the caller records the fingerprint (pins it)
};

const char* tls_trust_decision_name(TlsTrustDecision d);

// Called on the session's I/O thread; may block until the user decides.
using TlsTrustHandler = std::function<TlsTrustDecision(const TlsTrustRequest&)>;

} // namespace tn5250
} // namespace tn34xx
