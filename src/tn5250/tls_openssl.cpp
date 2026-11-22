// SPDX-License-Identifier: GPL-3.0-or-later
// TLS transport, OpenSSL backend (Linux and the BSDs): the host's own
// libssl/libcrypto, found by CMake outside vcpkg (EM34XX_TLS). Implicit TLS
// from the first byte, port 992 by convention.
//
// The socket stays in net/socket.cpp; OpenSSL reaches it through a small
// source/sink BIO, so recv() keeps its timeout semantics.
//
// Trust: OpenSSL's default verify paths (SSL_CERT_FILE / SSL_CERT_DIR, else
// the library's OPENSSLDIR, which for a distribution build is the
// distribution CA store), then the first well-known distribution bundle
// found, plus Tn5250Config::ca_file. The host name (or IP address) is
// checked against the certificate and sent as SNI.
//
// The verify callback never aborts the handshake; tls_accept_peer() decides
// afterwards with the certificate in hand (see tls_policy.hpp). No
// application data is read or sent before that decision.
#include "tn5250/tls_policy.hpp"

#include "net/socket.hpp"
#include "util/strings.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#if OPENSSL_VERSION_NUMBER < 0x10101000L
#error "the TLS transport needs OpenSSL 1.1.1 or newer"
#endif

namespace em34xx {
namespace tn5250 {

namespace {

constexpr int kHandshakeTimeoutMs = 15000;    // one read
constexpr int kHandshakeDeadlineMs = 30000;   // the whole handshake, or one send()

// --- socket BIO -------------------------------------------------------------

struct SocketBio {
    TcpSocket* sock = nullptr;
    int timeout_ms = 0;   // for the next read
    bool eof = false;     // the peer closed or the socket failed
    // While bounded, reads also stop at the deadline: a peer sending a few
    // bytes now and then cannot keep a handshake going for ever.
    bool bounded = false;
    std::chrono::steady_clock::time_point deadline;

    void bound(int ms) {
        bounded = true;
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    }
    bool expired() const { return bounded && std::chrono::steady_clock::now() >= deadline; }
};

int sock_bio_write(BIO* b, const char* in, int len) {
    auto* s = static_cast<SocketBio*>(BIO_get_data(b));
    BIO_clear_retry_flags(b);
    if (len <= 0) return 0;
    if (!s->sock->send_all(in, size_t(len))) {
        s->eof = true;
        return -1;
    }
    return len;
}

int sock_bio_read(BIO* b, char* out, int len) {
    auto* s = static_cast<SocketBio*>(BIO_get_data(b));
    BIO_clear_retry_flags(b);
    if (len <= 0) return 0;
    int timeout_ms = s->timeout_ms;
    if (s->bounded) {
        const long long left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   s->deadline - std::chrono::steady_clock::now())
                                   .count();
        if (left <= 0) {   // past the deadline: like a timeout
            BIO_set_retry_read(b);
            return -1;
        }
        if (left < timeout_ms) timeout_ms = int(left);
    }
    const int n = s->sock->recv(out, size_t(len), timeout_ms);
    if (n == 0) {   // timeout: SSL_read reports WANT_READ
        BIO_set_retry_read(b);
        return -1;
    }
    if (n < 0) {
        s->eof = true;
        return 0;
    }
    return n;
}

long sock_bio_ctrl(BIO*, int cmd, long, void*) { return cmd == BIO_CTRL_FLUSH ? 1 : 0; }

BIO_METHOD* socket_bio_method() {
    static BIO_METHOD* method = [] {
        BIO_METHOD* m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "em34xx socket");
        if (m) {
            BIO_meth_set_write(m, sock_bio_write);
            BIO_meth_set_read(m, sock_bio_read);
            BIO_meth_set_ctrl(m, sock_bio_ctrl);
        }
        return m;
    }();
    return method;
}

// --- helpers ----------------------------------------------------------------

std::string openssl_errors() {
    std::string out;
    while (unsigned long e = ERR_get_error()) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof buf);
        if (!out.empty()) out += "; ";
        out += buf;
    }
    return out;
}

std::string name_text(const X509_NAME* name) {
    BIO* m = BIO_new(BIO_s_mem());
    if (!m) return "?";
    X509_NAME_print_ex(m, name, 0, XN_FLAG_RFC2253 & ~ASN1_STRFLGS_ESC_MSB);
    char* p = nullptr;
    const long len = BIO_get_mem_data(m, &p);
    std::string s(p ? p : "", len > 0 ? size_t(len) : 0);
    BIO_free(m);
    return s;
}

std::string time_text(const ASN1_TIME* t) {
    struct tm tm{};
    if (!t || ASN1_TIME_to_tm(t, &tm) != 1) return "?";
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", &tm);
    return buf;
}

int protocol_number(TlsVersion v) {
    switch (v) {
    case TlsVersion::Any: return 0;
    case TlsVersion::Ssl3: return SSL3_VERSION;
    case TlsVersion::Tls1_0: return TLS1_VERSION;
    case TlsVersion::Tls1_1: return TLS1_1_VERSION;
    case TlsVersion::Tls1_2: return TLS1_2_VERSION;
    case TlsVersion::Tls1_3: return TLS1_3_VERSION;
    }
    return 0;
}

// Records the first chain or host name error; never fails the handshake.
struct VerifyState {
    int error = X509_V_OK;
    int depth = -1;
};

int verify_callback(int ok, X509_STORE_CTX* store) {
    SSL* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
    auto* st = ssl ? static_cast<VerifyState*>(SSL_get_app_data(ssl)) : nullptr;
    if (!ok && st && st->error == X509_V_OK) {
        st->error = X509_STORE_CTX_get_error(store);
        st->depth = X509_STORE_CTX_get_error_depth(store);
    }
    return 1;
}

bool file_exists(const char* path) {
    if (FILE* f = std::fopen(path, "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}

class OpenSslTransport : public Transport {
public:
    explicit OpenSslTransport(const Tn5250Config& cfg) : cfg_(cfg) { bio_.sock = &sock_; }
    ~OpenSslTransport() override { close(); }

    bool connect(const std::string& host, uint16_t port, std::string& error) override {
        close();
        if (!socket_bio_method()) {
            error = "TLS: cannot create the socket BIO";
            return false;
        }
        if (!setup_context(error)) {
            close();
            return false;
        }
        if (!sock_.connect(host, port, error, cancel_flag())) {
            close();
            return false;
        }
        port_ = port;
        ssl_ = SSL_new(ctx_);
        if (!ssl_) {
            error = "TLS: SSL_new failed: " + openssl_errors();
            close();
            return false;
        }
        SSL_set_app_data(ssl_, &verify_);
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl_);
        if (X509_VERIFY_PARAM_set1_ip_asc(param, host.c_str()) != 1) {
            // Not an IP address: a host name, for SNI and the name check.
            // Without either the certificate would be checked against no
            // name at all, so a failure ends the connection.
            ERR_clear_error();
            X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            if (host.empty() || SSL_set_tlsext_host_name(ssl_, host.c_str()) != 1 ||
                X509_VERIFY_PARAM_set1_host(param, host.c_str(), host.size()) != 1) {
                const std::string why = openssl_errors();
                error = "TLS: cannot set the host name to verify (" + host + ")" + (why.empty() ? "" : ": " + why);
                close();
                return false;
            }
        }
        BIO* b = BIO_new(socket_bio_method());
        if (!b) {
            error = "TLS: BIO_new failed";
            close();
            return false;
        }
        BIO_set_data(b, &bio_);
        BIO_set_init(b, 1);
        SSL_set_bio(ssl_, b, b);

        bio_.timeout_ms = kHandshakeTimeoutMs;
        bio_.bound(kHandshakeDeadlineMs);
        ERR_clear_error();
        const int r = SSL_connect(ssl_);
        const bool late = bio_.expired();
        bio_.bounded = false;
        if (r != 1) {
            const int e = SSL_get_error(ssl_, r);
            std::string why;
            if (e == SSL_ERROR_WANT_READ && late)
                why = format("the handshake did not complete within %d s", kHandshakeDeadlineMs / 1000);
            else if (e == SSL_ERROR_WANT_READ) why = format("no answer within %d s", kHandshakeTimeoutMs / 1000);
            else if (bio_.eof && ERR_peek_error() == 0) why = "the server closed the connection";
            else why = openssl_errors();
            if (why.empty()) why = format("SSL error %d", e);
            error = format("TLS handshake with %s:%u failed: %s", host.c_str(), port, why.c_str());
            if (port == 23) error += " (port 23 is plain telnet; TLS servers usually listen on 992)";
            else if (cfg_.tls_min_version >= TlsVersion::Tls1_2)
                error += " (an old host may need a lower tls_min_version)";
            close();
            return false;
        }
        summary_ = format("%s %s", SSL_get_version(ssl_), SSL_get_cipher_name(ssl_));
        note(LogLevel::Info, "TLS negotiated " + summary_);
        if (!check_peer(host, error)) {
            close();
            return false;
        }
        peer_ = sock_.peer_name();
        return true;
    }

    int recv(uint8_t* buf, size_t len, int timeout_ms) override {
        if (!ssl_ || failed_) return -1;
        bio_.timeout_ms = timeout_ms;
        ERR_clear_error();
        const int n = SSL_read(ssl_, buf, len > size_t(INT_MAX) ? INT_MAX : int(len));
        if (n > 0) return n;
        const int e = SSL_get_error(ssl_, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return 0;
        failed_ = true;
        if (e != SSL_ERROR_ZERO_RETURN) {
            const std::string why = openssl_errors();
            if (!why.empty()) note(LogLevel::Debug, "TLS read: " + why);
        }
        return -1;
    }

    bool send(const uint8_t* buf, size_t len) override {
        if (!ssl_ || failed_) return false;
        bio_.timeout_ms = kHandshakeTimeoutMs;
        // A write can wait for the peer (a renegotiation, say); the deadline
        // keeps a silent or trickling peer from holding send() for ever.
        bio_.bound(kHandshakeDeadlineMs);
        while (len > 0) {
            const int chunk = len > size_t(INT_MAX) ? INT_MAX : int(len);
            ERR_clear_error();
            const int n = SSL_write(ssl_, buf, chunk);
            if (n <= 0) {
                const int e = SSL_get_error(ssl_, n);
                if ((e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) && !bio_.expired()) continue;
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
                    note(LogLevel::Debug, format("TLS write: the peer did not let it finish within %d s",
                                                 kHandshakeDeadlineMs / 1000));
                bio_.bounded = false;
                failed_ = true;
                return false;
            }
            buf += n;
            len -= size_t(n);
        }
        bio_.bounded = false;
        return true;
    }

    void close() override {
        if (ssl_) {
            // close_notify only on a healthy connection: writing to a socket
            // the peer has reset could raise SIGPIPE.
            if (!failed_ && !bio_.eof) {
                bio_.timeout_ms = 0;
                SSL_shutdown(ssl_);
            }
            SSL_free(ssl_);   // frees the BIO too
            ssl_ = nullptr;
        }
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
        sock_.close();
        ERR_clear_error();
        bio_.eof = false;
        bio_.bounded = false;
        failed_ = false;
        verify_ = VerifyState{};
    }

    std::string describe() const override { return "tls " + peer_ + " " + summary_; }

private:
    bool setup_context(std::string& error) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) {
            error = "TLS: SSL_CTX_new failed: " + openssl_errors();
            return false;
        }
        if (!set_versions(error)) return false;
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, verify_callback);
        // Trust is loaded in every mode: with verification off or a pin it
        // still tells the user whether the chain would have verified.
        load_system_trust();
        if (!cfg_.ca_file.empty()) {
            if (SSL_CTX_load_verify_locations(ctx_, cfg_.ca_file.c_str(), nullptr) != 1) {
                error = "TLS: cannot load ca_file " + cfg_.ca_file + ": " + openssl_errors();
                return false;
            }
            note(LogLevel::Debug, "TLS: added the CA certificates in " + cfg_.ca_file);
        }
        return true;
    }

    bool set_versions(std::string& error) {
        if (cfg_.tls_min_version == TlsVersion::Ssl3) {
#ifdef OPENSSL_NO_SSL3
            error = "TLS: tls_min_version = \"ssl3\", but this system's OpenSSL was built without SSLv3";
            return false;
#endif
        }
        if (SSL_CTX_set_min_proto_version(ctx_, protocol_number(cfg_.tls_min_version)) != 1 ||
            SSL_CTX_set_max_proto_version(ctx_, protocol_number(cfg_.tls_max_version)) != 1) {
            error = format("TLS: this system's OpenSSL cannot offer protocol versions %s to %s: %s",
                           tls_version_name(cfg_.tls_min_version),
                           cfg_.tls_max_version == TlsVersion::Any ? "newest" : tls_version_name(cfg_.tls_max_version),
                           openssl_errors().c_str());
            return false;
        }
        if (cfg_.tls_min_version < TlsVersion::Tls1_2) {
            // OpenSSL's default security level refuses TLS 1.0/1.1, SSLv3
            // and SHA-1 signatures; lower it for this connection only.
            SSL_CTX_set_security_level(ctx_, 0);
            SSL_CTX_set_cipher_list(ctx_, "DEFAULT:@SECLEVEL=0");
            ERR_clear_error();
            tls_note_legacy(cfg_, [this](LogLevel l, const std::string& s) { note(l, s); });
        }
        return true;
    }

    void load_system_trust() {
        if (SSL_CTX_set_default_verify_paths(ctx_) != 1) ERR_clear_error();
        // Belt and braces for a libssl whose OPENSSLDIR is not where this
        // distribution keeps its bundle.
        if (std::getenv("SSL_CERT_FILE")) return;
        static const char* const kBundles[] = {
            "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Arch, Gentoo
            "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL
            "/etc/ssl/ca-bundle.pem",                             // openSUSE
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // RHEL family
            "/etc/ssl/cert.pem",                                  // Alpine, BSDs
            "/usr/local/share/certs/ca-root-nss.crt",             // FreeBSD
        };
        for (const char* path : kBundles) {
            if (!file_exists(path)) continue;
            if (SSL_CTX_load_verify_locations(ctx_, path, nullptr) == 1) {
                note(LogLevel::Debug, std::string("TLS: system CA bundle ") + path);
                return;
            }
            ERR_clear_error();
        }
    }

    bool check_peer(const std::string& host, std::string& error) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        X509* cert = SSL_get1_peer_certificate(ssl_);
#else
        X509* cert = SSL_get_peer_certificate(ssl_);
#endif
        if (!cert) {
            error = "TLS: the server sent no certificate";
            return false;
        }
        PeerCertificate peer;
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int md_len = 0;
        if (X509_digest(cert, EVP_sha256(), md, &md_len) == 1 && md_len == peer.sha256.size()) {
            for (size_t i = 0; i < peer.sha256.size(); ++i) peer.sha256[i] = md[i];
            peer.have_digest = true;
        }
        peer.subject = name_text(X509_get_subject_name(cert));
        peer.issuer = name_text(X509_get_issuer_name(cert));
        peer.valid_from = time_text(X509_get0_notBefore(cert));
        const ASN1_TIME* not_after = X509_get0_notAfter(cert);
        peer.expires = time_text(not_after);
        peer.expired = X509_cmp_current_time(not_after) < 0;
        X509_free(cert);

        const long result = SSL_get_verify_result(ssl_);
        const int chain_error = verify_.error != X509_V_OK ? verify_.error : int(result);
        peer.chain_ok = chain_error == X509_V_OK;
        if (!peer.chain_ok)
            peer.chain_error = format("%s (depth %d)", X509_verify_cert_error_string(chain_error), verify_.depth);
        set_peer_certificate(peer);
        return tls_accept_peer(cfg_, host, peer, [this](LogLevel l, const std::string& s) { note(l, s); }, error,
                               trust_handler(), port_);
    }

    Tn5250Config cfg_;
    uint16_t port_ = 0;
    TcpSocket sock_;
    SocketBio bio_;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    VerifyState verify_;
    bool failed_ = false;
    std::string peer_;
    std::string summary_;
};

} // namespace

const char* tls_backend_name() { return "openssl"; }

std::unique_ptr<Transport> make_tls_transport(const Tn5250Config& cfg, std::string& error) {
    const std::string invalid = cfg.validate();
    if (!invalid.empty()) {
        error = invalid;
        return nullptr;
    }
    return std::unique_ptr<Transport>(new OpenSslTransport(cfg));
}

} // namespace tn5250
} // namespace em34xx
