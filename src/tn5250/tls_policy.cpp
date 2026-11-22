// SPDX-License-Identifier: GPL-3.0-or-later
#include "tn5250/tls_policy.hpp"

#include "util/strings.hpp"

namespace em34xx {
namespace tn5250 {

std::string tls_fingerprint_line(const Sha256Digest& digest) {
    return "TLS server certificate SHA-256 fingerprint: " + format_sha256_fingerprint(digest) +
           "  (tls_cert_sha256 / --tn5250-tls-pin)";
}

void tls_note_legacy(const Tn5250Config& cfg, const Transport::LogFn& note) {
    if (cfg.tls_min_version >= TlsVersion::Tls1_2 || !note) return;
    note(LogLevel::Warn, format("TLS: legacy protocol versions enabled (tls_min_version = %s); these are "
                                "considered broken, use only for hosts that offer nothing newer",
                                tls_version_name(cfg.tls_min_version)));
}

const char* tls_trust_decision_name(TlsTrustDecision d) {
    switch (d) {
    case TlsTrustDecision::Reject: return "reject";
    case TlsTrustDecision::AcceptOnce: return "accept once";
    case TlsTrustDecision::AcceptPermanently: return "accept permanently";
    }
    return "?";
}

std::vector<std::string> tls_validity_errors(const PeerCertificate& peer) {
    std::vector<std::string> out = peer.validity_errors;
    if (!out.empty()) return out;
    if (peer.expired) out.push_back("The certificate expired on " + peer.expires);
    if (peer.not_yet_valid) out.push_back("The certificate is not valid before " + peer.valid_from);
    if (peer.revoked) out.push_back("The certificate has been revoked by its issuer");
    return out;
}

namespace {

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& l : lines) out += (out.empty() ? "" : "; ") + l;
    return out;
}

} // namespace

bool tls_accept_peer(const Tn5250Config& cfg, const std::string& host, const PeerCertificate& peer,
                     const Transport::LogFn& note_fn, std::string& error, const TlsTrustHandler& ask,
                     uint16_t port) {
    auto note = [&](LogLevel level, const std::string& line) {
        if (note_fn) note_fn(level, line);
    };
    const std::string fp = peer.have_digest ? format_sha256_fingerprint(peer.sha256) : std::string("?");
    const std::string details =
        format("TLS server certificate: subject %s; issuer %s; expires %s%s%s%s", peer.subject.c_str(),
               peer.issuer.c_str(), peer.expires.c_str(), peer.expired ? " (expired)" : "",
               peer.not_yet_valid ? " (not yet valid)" : "", peer.revoked ? " (revoked)" : "");
    const std::string chain_text = peer.chain_ok ? "it verifies against the trusted CAs for " + host
                                                 : "it does not verify: " + peer.chain_error;
    // A revocation status that could not be determined is not an error
    // (CRL and OCSP servers are often out of reach): one line, and on.
    auto log_details = [&](LogLevel level) {
        note(level, details);
        if (!peer.revoked && !peer.revocation_checked)
            note(LogLevel::Info, "TLS certificate revocation status not checked" +
                                     (peer.revocation_note.empty() ? std::string() : ": " + peer.revocation_note));
    };
    auto log_fingerprint = [&](LogLevel level) {
        log_details(level);
        note(level, peer.have_digest ? tls_fingerprint_line(peer.sha256)
                                     : std::string("TLS server certificate SHA-256 fingerprint: unavailable"));
    };
    // The configuration refuses the certificate: ask the handler, if any.
    auto refuse = [&](TlsTrustProblem problem, const std::string& reason, const std::string& message) {
        if (ask && peer.have_digest) {
            TlsTrustRequest req;
            req.host = host;
            req.port = port;
            req.peer = peer;
            req.problem = problem;
            req.reason = reason;
            req.errors.push_back(reason);
            const TlsTrustDecision d = ask(req);
            if (d != TlsTrustDecision::Reject) {
                note(LogLevel::Warn, "TLS certificate " + fp + " accepted by the user (" +
                                         tls_trust_decision_name(d) + "): " + reason);
                return true;
            }
            note(LogLevel::Warn, "TLS certificate " + fp + " rejected by the user");
        }
        error = message;
        return false;
    };

    const bool pinned = !cfg.tls_cert_sha256.empty();
    Sha256Digest pin{};
    if (pinned && !parse_sha256_fingerprint(cfg.tls_cert_sha256, pin)) {
        error = "TLS: tls_cert_sha256 is not a SHA-256 fingerprint: " + cfg.tls_cert_sha256;
        return false;
    }
    const bool pin_match = pinned && peer.have_digest && pin == peer.sha256;
    const std::vector<std::string> invalid = tls_validity_errors(peer);

    if (!pinned && cfg.tls_verify == TlsVerify::None) {
        log_fingerprint(LogLevel::Warn);
        for (const std::string& e : invalid) note(LogLevel::Warn, "TLS certificate is not valid: " + e);
        note(LogLevel::Warn, "TLS certificate verification is OFF (tls_verify = \"none\"); " + chain_text);
        return true;
    }
    if (!invalid.empty()) {
        // Expired, not yet valid or revoked. Whatever the trust answer (a
        // pin match included), only the user can let this connection
        // through, and nothing about that answer is recorded.
        std::vector<std::string> errors = invalid;
        if (pinned && !pin_match)
            errors.push_back("The certificate does not match the trusted SHA-256 fingerprint (" +
                             format_sha256_fingerprint(pin) + ")");
        else if (!pinned && !peer.chain_ok)
            errors.push_back("The certificate is not trusted: " + peer.chain_error);
        const std::string all = join_lines(errors);
        log_fingerprint(LogLevel::Warn);
        for (const std::string& e : invalid) note(LogLevel::Warn, "TLS certificate is not valid: " + e);
        if (ask && peer.have_digest) {
            TlsTrustRequest req;
            req.host = host;
            req.port = port;
            req.peer = peer;
            req.problem = TlsTrustProblem::Invalid;
            req.reason = all;
            req.errors = errors;
            // AcceptPermanently has no meaning here: it counts as once.
            if (ask(req) != TlsTrustDecision::Reject) {
                note(LogLevel::Warn, "TLS certificate " + fp +
                                         " is not valid; accepted by the user for this connection only: " + all);
                return true;
            }
            note(LogLevel::Warn, "TLS certificate " + fp + " rejected by the user");
        } else if (pin_match && !peer.revoked) {
            // Nobody to ask: the pin names exactly this certificate.
            note(LogLevel::Warn, "TLS certificate matches the pinned SHA-256 " + fp +
                                     " and is accepted although it is not valid: " + all);
            return true;
        }
        error = "TLS: the server certificate for " + host + " was rejected: " + all;
        return false;
    }

    if (pinned) {
        if (pin_match) {
            log_details(LogLevel::Info);
            note(LogLevel::Info, "TLS certificate matches the pinned SHA-256 " + fp + "; " + chain_text);
            return true;
        }
        log_fingerprint(LogLevel::Warn);
        return refuse(TlsTrustProblem::PinMismatch,
                      "the certificate does not match the trusted SHA-256 fingerprint (" +
                          format_sha256_fingerprint(pin) + ")" + (peer.chain_ok ? "" : "; " + chain_text),
                      "TLS: the server certificate does not match the pinned SHA-256 (tls_cert_sha256); "
                      "its fingerprint is in the log");
    }
    if (!peer.chain_ok) {
        log_fingerprint(LogLevel::Warn);
        return refuse(TlsTrustProblem::Unverified, peer.chain_error,
                      "TLS: the server certificate for " + host + " was rejected: " + peer.chain_error +
                          ". If you trust it, pin its fingerprint (in the log) or add its CA with ca_file");
    }
    log_details(LogLevel::Info);
    note(LogLevel::Info, "TLS certificate verified for " + host + "; SHA-256 " + fp);
    return true;
}

bool tls_fetch_certificate(const Tn5250Config& cfg_in, PeerCertificate& out, std::string& error) {
    Tn5250Config cfg = cfg_in;
    cfg.tls = true;
    cfg.tls_verify = TlsVerify::None;   // take whatever is presented; nothing is sent
    cfg.tls_cert_sha256.clear();
    std::unique_ptr<Transport> t = make_tls_transport(cfg, error);
    if (!t) return false;
    const bool ok = t->connect(cfg.host, cfg.port, error);
    const PeerCertificate* peer = t->peer_certificate();
    t->close();
    if (!peer) {
        if (ok) error = "TLS: the server sent no certificate";
        return false;
    }
    out = *peer;
    error.clear();
    return true;
}

} // namespace tn5250
} // namespace em34xx
