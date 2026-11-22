// SPDX-License-Identifier: GPL-3.0-or-later
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
// Two questions are kept apart. Trust: does the certificate chain to a
// trusted root and name the host (chain_ok), or is it the pinned one? A
// failure there is what self-signed and private-CA servers produce, and
// the answer may be remembered (a pin). Validity: is the certificate
// inside its validity period and not revoked (validity_errors)? Accepting
// a failure there is an override for one connection only: it is asked for
// every time and never remembered, whatever the trust answer is.
//
// Without a handler the behaviour is the configured one (the SDL front end,
// the monitor and the headless build keep working unchanged).
#pragma once

#include "util/fingerprint.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace em34xx {
namespace tn5250 {

struct PeerCertificate {
    bool have_digest = false;
    Sha256Digest sha256{};      // of the leaf certificate's DER encoding
    std::string subject;
    std::string issuer;
    std::string valid_from;     // "YYYY-MM-DD HH:MM:SS UTC"
    std::string expires;        // "YYYY-MM-DD HH:MM:SS UTC"
    // Validity, apart from trust: the validity period against this
    // machine's clock, and revocation.
    bool expired = false;
    bool not_yet_valid = false;
    bool revoked = false;
    // The revocation status was determined. False (a CRL or OCSP server out
    // of reach, no revocation information in the certificate, a backend
    // that cannot check) is not an error; revocation_note says why.
    bool revocation_checked = false;
    std::string revocation_note;
    // One exact line per validity failure, with the platform's own code or
    // text ("The certificate expired on 2024-03-01 12:00:00 UTC (...)").
    // Includes failures of the other certificates in the chain.
    std::vector<std::string> validity_errors;
    // Trust only: validity failures are not folded into these.
    bool chain_ok = false;      // chains to a trusted root and matches the host name
    std::string chain_error;    // why not, when !chain_ok
};

// The validity failures of a certificate: validity_errors, or a plain line
// per flag when a backend set only the flags. Empty: the certificate is
// inside its validity period and not known to be revoked.
std::vector<std::string> tls_validity_errors(const PeerCertificate& peer);

// Why the configuration refused a certificate.
enum class TlsTrustProblem {
    Unverified,      // system verification failed (chain_error says why)
    PinMismatch,     // tls_cert_sha256 is set and the certificate differs
    // Expired, not yet valid or revoked (possibly untrusted as well; the
    // request's `errors` lists everything). Only Reject and AcceptOnce are
    // meaningful answers: the decision is never recorded or remembered.
    Invalid,
};

struct TlsTrustRequest {
    std::string host;
    uint16_t port = 0;
    unsigned station = 0;       // Twinax station address of the session (bridge sessions)
    PeerCertificate peer;
    TlsTrustProblem problem = TlsTrustProblem::Unverified;
    std::string reason;         // one line for the user
    // Every failure, one line each. Invalid: the validity failures, then
    // the trust or pin failure if there is one too. Otherwise just `reason`.
    std::vector<std::string> errors;
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
} // namespace em34xx
