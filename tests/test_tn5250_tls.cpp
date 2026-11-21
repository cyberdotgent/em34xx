// TLS for the TN5250 client: fingerprints, configuration, the certificate
// decision, and (with the OpenSSL backend) a real handshake against an
// in-process server using a certificate generated at test time.
#include "app/host_link.hpp"
#include "config/config.hpp"
#include "host/host_config.hpp"
#include "tn5250/tls_policy.hpp"
#include "tn5250/transport.hpp"
#include "util/fingerprint.hpp"

#include <doctest/doctest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace tn34xx;
using namespace tn34xx::tn5250;

namespace {

Sha256Digest sample_digest() {
    Sha256Digest d{};
    for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(0xA0 + i * 7);
    return d;
}

std::string write_temp(const std::string& name, const std::string& text) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path, std::ios::binary) << text;
    return path.string();
}

// The fingerprint from a "... SHA-256 fingerprint: XX:..." log line.
bool fingerprint_from_log(const std::vector<std::string>& log, Sha256Digest& out) {
    static const std::string kKey = "SHA-256 fingerprint: ";
    for (const std::string& line : log) {
        const size_t at = line.find(kKey);
        if (at == std::string::npos) continue;
        const std::string rest = line.substr(at + kKey.size());
        return parse_sha256_fingerprint(rest.substr(0, rest.find(' ')), out);
    }
    return false;
}

struct LogCapture {
    std::vector<std::string> lines;
    std::vector<LogLevel> levels;
    Transport::LogFn fn() {
        return [this](LogLevel l, const std::string& s) {
            levels.push_back(l);
            lines.push_back(s);
        };
    }
    bool has(const std::string& text) const {
        for (const auto& l : lines)
            if (l.find(text) != std::string::npos) return true;
        return false;
    }
};

} // namespace

TEST_CASE("TLS fingerprint: canonical format and accepted spellings") {
    const Sha256Digest d = sample_digest();
    const std::string canon = format_sha256_fingerprint(d);
    CHECK(canon.size() == 95);
    CHECK(canon.substr(0, 9) == "A0:A7:AE:");

    Sha256Digest back{};
    REQUIRE(parse_sha256_fingerprint(canon, back));
    CHECK(back == d);

    std::string bare;
    for (char c : canon)
        if (c != ':') bare += char(std::tolower(static_cast<unsigned char>(c)));
    back = {};
    REQUIRE(parse_sha256_fingerprint(bare, back));
    CHECK(back == d);

    std::string spaced = canon, dashed = canon;
    for (char& c : spaced) if (c == ':') c = ' ';
    for (char& c : dashed) if (c == ':') c = '-';
    CHECK(parse_sha256_fingerprint(spaced, back));
    CHECK(parse_sha256_fingerprint(dashed, back));
    CHECK(parse_sha256_fingerprint("SHA256:" + canon, back));
    CHECK(parse_sha256_fingerprint("sha256=" + bare, back));
    CHECK(parse_sha256_fingerprint("  " + canon + "  ", back));
    CHECK(back == d);

    CHECK_FALSE(parse_sha256_fingerprint("", back));
    CHECK_FALSE(parse_sha256_fingerprint(canon.substr(0, 92), back));          // 31 bytes
    CHECK_FALSE(parse_sha256_fingerprint(canon + ":00", back));                // 33 bytes
    CHECK_FALSE(parse_sha256_fingerprint(canon + ":", back));                  // trailing separator
    CHECK_FALSE(parse_sha256_fingerprint(":" + canon, back));
    CHECK_FALSE(parse_sha256_fingerprint("A:0A7" + canon.substr(5), back));   // split pair
    CHECK_FALSE(parse_sha256_fingerprint("G0" + canon.substr(2), back));
    CHECK_FALSE(parse_sha256_fingerprint(canon.substr(0, 2) + "::" + canon.substr(3), back));
}

TEST_CASE("TLS config: verify modes, versions and validation") {
    TlsVerify v;
    CHECK((parse_tls_verify("system", v) && v == TlsVerify::System));
    CHECK((parse_tls_verify("NONE", v) && v == TlsVerify::None));
    CHECK((parse_tls_verify("insecure", v) && v == TlsVerify::None));
    CHECK_FALSE(parse_tls_verify("pin", v));

    TlsVersion tv;
    CHECK((parse_tls_version("ssl3", tv) && tv == TlsVersion::Ssl3));
    CHECK((parse_tls_version("1.0", tv) && tv == TlsVersion::Tls1_0));
    CHECK((parse_tls_version("TLSv1.1", tv) && tv == TlsVersion::Tls1_1));
    CHECK((parse_tls_version("tls1.2", tv) && tv == TlsVersion::Tls1_2));
    CHECK((parse_tls_version("1.3", tv) && tv == TlsVersion::Tls1_3));
    CHECK((parse_tls_version("any", tv) && tv == TlsVersion::Any));
    CHECK_FALSE(parse_tls_version("1.4", tv));
    CHECK_FALSE(parse_tls_version("2", tv));
    CHECK(std::string(tls_version_name(TlsVersion::Tls1_0)) == "1.0");

    Tn5250Config c;
    c.host = "h";
    c.port = 992;
    c.tls = true;
    CHECK(c.validate().empty());
    CHECK(c.tls_min_version == TlsVersion::Tls1_2);   // the default
    c.tls_min_version = TlsVersion::Tls1_3;
    c.tls_max_version = TlsVersion::Tls1_2;
    CHECK(c.validate().find("newer than") != std::string::npos);
    c.tls_min_version = TlsVersion::Tls1_0;
    CHECK(c.validate().empty());
    c.tls_min_version = TlsVersion::Any;
    CHECK_FALSE(c.validate().empty());
    c.tls_min_version = TlsVersion::Tls1_2;
    c.tls_max_version = TlsVersion::Any;

    c.tls_cert_sha256 = "not a hash";
    CHECK(c.validate().find("tls_cert_sha256") != std::string::npos);
    c.tls_cert_sha256 = format_sha256_fingerprint(sample_digest());
    CHECK(c.validate().empty());
    c.tls_verify = TlsVerify::None;
    CHECK(c.validate().find("contradict") != std::string::npos);
    c.tls_verify = TlsVerify::System;
    c.tls = false;
    CHECK_FALSE(c.validate().empty());
}

TEST_CASE("TLS config: TOML [tn5250], station overrides and the command line") {
    const std::string pin = format_sha256_fingerprint(sample_digest());
    {
        const std::string path = write_temp("tn34xx_test_tls1.toml",
                                            "[tn5250]\nhost = \"h\"\nport = 992\ntls = true\n"
                                            "tls_verify = \"none\"\ntls_min_version = \"1.0\"\n"
                                            "tls_max_version = \"1.2\"\n");
        AppConfig cfg;
        std::string err;
        REQUIRE(load_config_file(path, cfg, err));
        CHECK(cfg.tn5250.tls);
        CHECK(cfg.tn5250.tls_verify == TlsVerify::None);
        CHECK(cfg.tn5250.tls_min_version == TlsVersion::Tls1_0);
        CHECK(cfg.tn5250.tls_max_version == TlsVersion::Tls1_2);
        CHECK(cfg.tn5250.validate().empty());
        // round trip through --dump-config
        const std::string again = write_temp("tn34xx_test_tls1b.toml", config_to_toml(cfg));
        AppConfig cfg2;
        REQUIRE(load_config_file(again, cfg2, err));
        CHECK(cfg2.tn5250.tls_verify == TlsVerify::None);
        CHECK(cfg2.tn5250.tls_min_version == TlsVersion::Tls1_0);
        CHECK(cfg2.tn5250.tls_max_version == TlsVersion::Tls1_2);
    }
    {
        const std::string path =
            write_temp("tn34xx_test_tls2.toml", "[tn5250]\nhost = \"h\"\ntls = true\ntls_verify = false\n");
        AppConfig cfg;
        std::string err;
        REQUIRE(load_config_file(path, cfg, err));
        CHECK(cfg.tn5250.tls_verify == TlsVerify::None);   // the old boolean form
    }
    {
        const std::string path = write_temp("tn34xx_test_tls3.toml",
                                            "[tn5250]\nhost = \"h\"\ntls = true\ntls_cert_sha256 = \"" + pin +
                                                "\"\n[[twinax.station]]\naddress = 0\n"
                                                "[[twinax.station]]\naddress = 1\ntls_min_version = \"1.1\"\n");
        AppConfig cfg;
        std::string err;
        REQUIRE(load_config_file(path, cfg, err));
        CHECK(cfg.tn5250.tls_verify == TlsVerify::System);
        CHECK(cfg.tn5250.tls_cert_sha256 == pin);
        CHECK(cfg.tn5250.validate().empty());
        REQUIRE(cfg.twinax.stations.size() == 2);
        CHECK(cfg.twinax.validate().empty());
        Tn5250Config s0 = cfg.tn5250, s1 = cfg.tn5250;
        apply_station_tls(s0, cfg.twinax.stations[0]);
        apply_station_tls(s1, cfg.twinax.stations[1]);
        CHECK(s0.tls_min_version == TlsVersion::Tls1_2);
        CHECK(s1.tls_min_version == TlsVersion::Tls1_1);
        cfg.twinax.stations[1].tls_min_version = "1.7";
        CHECK(cfg.twinax.validate().find("tls_min_version") != std::string::npos);
    }
    {
        const std::string path = write_temp("tn34xx_test_tls4.toml", "[tn5250]\ntls_verify = \"maybe\"\n");
        AppConfig cfg;
        std::string err;
        CHECK_FALSE(load_config_file(path, cfg, err));
        CHECK(err.find("tls_verify") != std::string::npos);
    }
    {
        const std::string path = write_temp("tn34xx_test_tls5.toml", "[tn5250]\ntls_min_version = \"9\"\n");
        AppConfig cfg;
        std::string err;
        CHECK_FALSE(load_config_file(path, cfg, err));
    }
    {
        std::string bare;
        for (char ch : pin)
            if (ch != ':') bare += char(std::tolower(static_cast<unsigned char>(ch)));
        const char* argv[] = {"tn34xx", "--tn5250-host", "h", "--tn5250-port", "992", "--tn5250-tls",
                              "--tn5250-tls-pin", bare.c_str(), "--tn5250-tls-min", "1.0", "--tn5250-tls-max=1.2"};
        AppConfig cfg;
        std::string err;
        REQUIRE(parse_command_line(int(sizeof argv / sizeof *argv), const_cast<char**>(argv), cfg, err));
        CHECK(cfg.tn5250.tls);
        CHECK(cfg.tn5250.tls_cert_sha256 == bare);
        CHECK(cfg.tn5250.tls_min_version == TlsVersion::Tls1_0);
        CHECK(cfg.tn5250.tls_max_version == TlsVersion::Tls1_2);
        CHECK(cfg.tn5250.validate().empty());
    }
    {
        const char* argv[] = {"tn34xx", "--tn5250-tls", "--tn5250-tls-insecure"};
        AppConfig cfg;
        std::string err;
        REQUIRE(parse_command_line(3, const_cast<char**>(argv), cfg, err));
        CHECK(cfg.tn5250.tls_verify == TlsVerify::None);
    }
    {
        const char* argv[] = {"tn34xx", "--tn5250-tls-pin", "abc"};
        AppConfig cfg;
        std::string err;
        CHECK_FALSE(parse_command_line(3, const_cast<char**>(argv), cfg, err));
        const char* argv2[] = {"tn34xx", "--tn5250-tls-min", "any"};
        CHECK_FALSE(parse_command_line(3, const_cast<char**>(argv2), cfg, err));
    }
}

TEST_CASE("TLS certificate decision: system, insecure and pinned modes") {
    Tn5250Config cfg;
    cfg.host = "h";
    cfg.tls = true;
    PeerCertificate good;
    good.have_digest = true;
    good.sha256 = sample_digest();
    good.subject = "CN=h";
    good.issuer = "CN=Test CA";
    good.expires = "2030-01-01 00:00:00 UTC";
    good.chain_ok = true;
    PeerCertificate bad = good;
    bad.chain_ok = false;
    bad.chain_error = "self-signed certificate (depth 0)";
    std::string err;

    SUBCASE("system: a verified chain is accepted quietly") {
        LogCapture log;
        CHECK(tls_accept_peer(cfg, "h", good, log.fn(), err));
        for (LogLevel l : log.levels) CHECK(l == LogLevel::Info);
    }
    SUBCASE("system: a failed chain is refused and the fingerprint logged as a warning") {
        LogCapture log;
        CHECK_FALSE(tls_accept_peer(cfg, "h", bad, log.fn(), err));
        CHECK(err.find("self-signed") != std::string::npos);
        Sha256Digest fp{};
        REQUIRE(fingerprint_from_log(log.lines, fp));
        CHECK(fp == good.sha256);
        CHECK(log.has("subject CN=h; issuer CN=Test CA; expires 2030-01-01"));
    }
    SUBCASE("insecure: accepted, with the fingerprint and the chain status") {
        cfg.tls_verify = TlsVerify::None;
        LogCapture log;
        CHECK(tls_accept_peer(cfg, "h", bad, log.fn(), err));
        Sha256Digest fp{};
        REQUIRE(fingerprint_from_log(log.lines, fp));
        CHECK(fp == good.sha256);
        CHECK(log.has("verification is OFF"));
        CHECK(log.has("does not verify"));
    }
    SUBCASE("pinned: a match wins over a failed chain, a mismatch loses to a good one") {
        cfg.tls_cert_sha256 = format_sha256_fingerprint(good.sha256);
        LogCapture log;
        CHECK(tls_accept_peer(cfg, "h", bad, log.fn(), err));
        CHECK(log.has("matches the pinned"));
        Sha256Digest other = good.sha256;
        other[0] ^= 1;
        cfg.tls_cert_sha256 = format_sha256_fingerprint(other);
        LogCapture log2;
        CHECK_FALSE(tls_accept_peer(cfg, "h", good, log2.fn(), err));
        Sha256Digest fp{};
        REQUIRE(fingerprint_from_log(log2.lines, fp));
        CHECK(fp == good.sha256);
    }
    SUBCASE("legacy protocols are announced") {
        LogCapture log;
        tls_note_legacy(cfg, log.fn());
        CHECK(log.lines.empty());
        cfg.tls_min_version = TlsVersion::Tls1_0;
        tls_note_legacy(cfg, log.fn());
        REQUIRE(log.lines.size() == 1);
        CHECK(log.levels[0] == LogLevel::Warn);
    }
}

TEST_CASE("TLS trust hook: the handler decides what the configuration refuses") {
    Tn5250Config cfg;
    cfg.host = "h";
    cfg.tls = true;
    PeerCertificate bad;
    bad.have_digest = true;
    bad.sha256 = sample_digest();
    bad.subject = "CN=h";
    bad.chain_ok = false;
    bad.chain_error = "self-signed certificate (depth 0)";
    std::string err;
    std::vector<TlsTrustRequest> asked;
    TlsTrustDecision answer = TlsTrustDecision::Reject;
    TlsTrustHandler ask = [&](const TlsTrustRequest& r) {
        asked.push_back(r);
        return answer;
    };

    SUBCASE("rejecting keeps the configured error") {
        LogCapture log;
        CHECK_FALSE(tls_accept_peer(cfg, "h", bad, log.fn(), err, ask, 992));
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].host == "h");
        CHECK(asked[0].port == 992);
        CHECK(asked[0].problem == TlsTrustProblem::Unverified);
        CHECK(asked[0].reason == bad.chain_error);
        CHECK(asked[0].peer.sha256 == bad.sha256);
        CHECK(err.find("rejected") != std::string::npos);
        CHECK(log.has("rejected by the user"));
    }
    SUBCASE("accepting connects, once or permanently") {
        for (TlsTrustDecision d : {TlsTrustDecision::AcceptOnce, TlsTrustDecision::AcceptPermanently}) {
            answer = d;
            LogCapture log;
            CHECK(tls_accept_peer(cfg, "h", bad, log.fn(), err, ask, 992));
            CHECK(log.has("accepted by the user"));
        }
        CHECK(asked.size() == 2);
    }
    SUBCASE("a verified certificate, or verification off, never asks") {
        PeerCertificate good = bad;
        good.chain_ok = true;
        CHECK(tls_accept_peer(cfg, "h", good, nullptr, err, ask, 992));
        cfg.tls_verify = TlsVerify::None;
        CHECK(tls_accept_peer(cfg, "h", bad, nullptr, err, ask, 992));
        CHECK(asked.empty());
    }
    SUBCASE("a pin mismatch asks too; a pin match does not") {
        Sha256Digest other = bad.sha256;
        other[0] ^= 1;
        cfg.tls_cert_sha256 = format_sha256_fingerprint(other);
        answer = TlsTrustDecision::AcceptPermanently;
        CHECK(tls_accept_peer(cfg, "h", bad, nullptr, err, ask, 992));
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].problem == TlsTrustProblem::PinMismatch);
        cfg.tls_cert_sha256 = format_sha256_fingerprint(bad.sha256);
        CHECK(tls_accept_peer(cfg, "h", bad, nullptr, err, ask, 992));
        CHECK(asked.size() == 1);
    }
}

TEST_CASE("TLS trust broker: one handler per bridge, accepted certificates remembered") {
    TlsTrustBroker broker;
    CHECK_FALSE(broker.for_new_bridge());   // no handler: the configuration decides
    int calls = 0;
    TlsTrustDecision answer = TlsTrustDecision::AcceptOnce;
    broker.set_handler([&](const TlsTrustRequest&) {
        ++calls;
        return answer;
    });
    TlsTrustRequest req;
    req.host = "h";
    req.port = 992;
    req.peer.have_digest = true;
    req.peer.sha256 = sample_digest();

    TlsTrustHandler bridge1 = broker.for_new_bridge();
    REQUIRE(bridge1);
    CHECK(bridge1(req) == TlsTrustDecision::AcceptOnce);
    CHECK(bridge1(req) == TlsTrustDecision::AcceptOnce);   // a reconnect of the same bridge
    CHECK(calls == 1);
    TlsTrustRequest other = req;
    other.peer.sha256[5] ^= 1;
    answer = TlsTrustDecision::Reject;
    CHECK(bridge1(other) == TlsTrustDecision::Reject);
    CHECK(bridge1(other) == TlsTrustDecision::Reject);     // rejections are not remembered
    CHECK(calls == 3);

    TlsTrustHandler bridge2 = broker.for_new_bridge();      // a new connect asks again
    answer = TlsTrustDecision::AcceptOnce;
    CHECK(bridge2(req) == TlsTrustDecision::AcceptOnce);
    CHECK(calls == 4);
}

TEST_CASE("TLS transport: plain TCP stays the default") {
    Tn5250Config cfg;
    cfg.host = "h";
    std::string err;
    auto t = make_transport(cfg, err);
    REQUIRE(t);
    CHECK(t->describe().rfind("tcp", 0) == 0);
    if (!tls_backend_name()) {
        cfg.tls = true;
        CHECK(make_transport(cfg, err) == nullptr);
        CHECK(err.find("no TLS support") != std::string::npos);
    }
}

#ifdef TN34XX_TLS_OPENSSL
// --- live handshake against an in-process OpenSSL server ------------------

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

struct TestCert {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    Sha256Digest sha256{};
    std::string pem_path;

    explicit TestCert(const char* san) {
        key = EVP_EC_gen("P-256");
        cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
        X509_gmtime_adj(X509_getm_notAfter(cert), 86400);
        X509_set_pubkey(cert, key);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("tn34xx test"),
                                   -1, -1, 0);
        X509_set_issuer_name(cert, name);
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, san);
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
        X509_sign(cert, key, EVP_sha256());
        unsigned int len = 0;
        X509_digest(cert, EVP_sha256(), sha256.data(), &len);
        pem_path = (std::filesystem::temp_directory_path() / "tn34xx_test_tls_ca.pem").string();
        FILE* f = std::fopen(pem_path.c_str(), "wb");
        PEM_write_X509(f, cert);
        std::fclose(f);
    }
    ~TestCert() {
        X509_free(cert);
        EVP_PKEY_free(key);
    }
};

// Accepts one connection, completes the handshake, sends "hello", then
// waits for the client to go away.
struct TestServer {
    int listen_fd = -1;
    uint16_t port = 0;
    std::thread thread;
    bool handshake_ok = false;

    TestServer(const TestCert& tc, int min_version, int max_version) {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listen_fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(listen_fd, 1);
        socklen_t len = sizeof a;
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
        SSL_CTX_use_certificate(ctx, tc.cert);
        SSL_CTX_use_PrivateKey(ctx, tc.key);
        SSL_CTX_set_min_proto_version(ctx, min_version);
        SSL_CTX_set_max_proto_version(ctx, max_version);
        if (min_version < TLS1_2_VERSION) SSL_CTX_set_security_level(ctx, 0);
        thread = std::thread([this, ctx] {
            pollfd p{listen_fd, POLLIN, 0};
            if (::poll(&p, 1, 5000) == 1) {
                const int fd = ::accept(listen_fd, nullptr, nullptr);
                SSL* ssl = SSL_new(ctx);
                SSL_set_fd(ssl, fd);
                if (SSL_accept(ssl) == 1) {
                    handshake_ok = true;
                    SSL_write(ssl, "hello", 5);
                    char buf[64];
                    while (SSL_read(ssl, buf, sizeof buf) > 0) {}
                }
                SSL_free(ssl);
                ::close(fd);
            }
            SSL_CTX_free(ctx);
            ERR_clear_error();
        });
    }
    ~TestServer() {
        thread.join();
        ::close(listen_fd);
    }
};

struct Outcome {
    bool connected = false;
    std::string data;
    std::string error;
    LogCapture log;
};

Outcome run_client(Tn5250Config cfg, const std::string& host, uint16_t port) {
    Outcome o;
    cfg.host = host;
    cfg.port = port;
    cfg.tls = true;
    std::string err;
    auto t = make_transport(cfg, err);
    REQUIRE(t);
    t->set_log(o.log.fn());
    o.connected = t->connect(host, port, o.error);
    if (o.connected) {
        uint8_t buf[16];
        for (int i = 0; i < 50 && o.data.size() < 5; ++i) {
            const int n = t->recv(buf, sizeof buf, 100);
            if (n < 0) break;
            o.data.append(reinterpret_cast<char*>(buf), size_t(n));
        }
        CHECK(t->describe().find("TLSv1") != std::string::npos);
    }
    t->close();
    return o;
}

} // namespace

TEST_CASE("TLS transport (OpenSSL): handshake with a self-signed server certificate") {
    std::signal(SIGPIPE, SIG_IGN);
    REQUIRE(std::string(tls_backend_name()) == "openssl");
    const TestCert tc("DNS:localhost");
    Tn5250Config base;

    SUBCASE("system store only: refused, fingerprint logged") {
        TestServer srv(tc, TLS1_2_VERSION, 0);
        Outcome o = run_client(base, "127.0.0.1", srv.port);
        CHECK_FALSE(o.connected);
        CHECK(o.error.find("rejected") != std::string::npos);
        Sha256Digest fp{};
        REQUIRE(fingerprint_from_log(o.log.lines, fp));
        CHECK(fp == tc.sha256);
    }
    SUBCASE("insecure: connected, fingerprint logged") {
        TestServer srv(tc, TLS1_2_VERSION, 0);
        Tn5250Config c = base;
        c.tls_verify = TlsVerify::None;
        Outcome o = run_client(c, "127.0.0.1", srv.port);
        CHECK(o.connected);
        CHECK(o.data == "hello");
        Sha256Digest fp{};
        REQUIRE(fingerprint_from_log(o.log.lines, fp));
        CHECK(fp == tc.sha256);
        CHECK(o.log.has("TLS negotiated TLSv1."));
    }
    SUBCASE("pinned (lower case, no colons): connected despite the chain") {
        TestServer srv(tc, TLS1_2_VERSION, 0);
        Tn5250Config c = base;
        std::string bare;
        for (char ch : format_sha256_fingerprint(tc.sha256))
            if (ch != ':') bare += char(std::tolower(static_cast<unsigned char>(ch)));
        c.tls_cert_sha256 = bare;
        Outcome o = run_client(c, "127.0.0.1", srv.port);
        CHECK(o.connected);
        CHECK(o.data == "hello");
        CHECK(o.log.has("matches the pinned"));
    }
    SUBCASE("wrong pin: refused") {
        TestServer srv(tc, TLS1_2_VERSION, 0);
        Tn5250Config c = base;
        Sha256Digest other = tc.sha256;
        other[31] ^= 0x80;
        c.tls_cert_sha256 = format_sha256_fingerprint(other);
        Outcome o = run_client(c, "127.0.0.1", srv.port);
        CHECK_FALSE(o.connected);
        CHECK(o.error.find("pinned") != std::string::npos);
    }
    SUBCASE("ca_file as trust anchor, host name checked") {
        Tn5250Config c = base;
        c.ca_file = tc.pem_path;
        {
            TestServer srv(tc, TLS1_2_VERSION, 0);
            Outcome o = run_client(c, "localhost", srv.port);
            CHECK(o.connected);
            CHECK(o.data == "hello");
            CHECK(o.log.has("verified for localhost"));
        }
        {
            TestServer srv(tc, TLS1_2_VERSION, 0);   // the certificate names localhost, not the address
            Outcome o = run_client(c, "127.0.0.1", srv.port);
            CHECK_FALSE(o.connected);
            CHECK(o.error.find("mismatch") != std::string::npos);
        }
    }
    SUBCASE("trust handler: asked with the certificate details; accepting connects") {
        TestServer srv(tc, TLS1_2_VERSION, 0);
        Tn5250Config c = base;
        c.host = "127.0.0.1";
        c.port = srv.port;
        c.tls = true;
        std::string err;
        auto t = make_transport(c, err);
        REQUIRE(t);
        std::vector<TlsTrustRequest> asked;
        t->set_trust_handler([&](const TlsTrustRequest& r) {
            asked.push_back(r);
            return TlsTrustDecision::AcceptOnce;
        });
        CHECK(t->connect(c.host, c.port, err));
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].port == srv.port);
        CHECK(asked[0].peer.sha256 == tc.sha256);
        CHECK(asked[0].peer.subject.find("tn34xx test") != std::string::npos);
        CHECK_FALSE(asked[0].peer.valid_from.empty());
        CHECK_FALSE(asked[0].peer.expires.empty());
        CHECK_FALSE(asked[0].reason.empty());
        REQUIRE(t->peer_certificate());
        CHECK(t->peer_certificate()->sha256 == tc.sha256);
        t->close();
    }
    SUBCASE("trust handler: rejecting refuses the connection") {
        TestServer srv(tc, TLS1_2_VERSION, 0);
        Tn5250Config c = base;
        c.host = "127.0.0.1";
        c.port = srv.port;
        c.tls = true;
        std::string err;
        auto t = make_transport(c, err);
        REQUIRE(t);
        t->set_trust_handler([](const TlsTrustRequest&) { return TlsTrustDecision::Reject; });
        CHECK_FALSE(t->connect(c.host, c.port, err));
        CHECK(err.find("rejected") != std::string::npos);
        t->close();
    }
    SUBCASE("fetch certificate: the fingerprint of an untrusted server") {
        TestServer srv(tc, TLS1_2_VERSION, 0);
        Tn5250Config c = base;
        c.host = "127.0.0.1";
        c.port = srv.port;
        PeerCertificate peer;
        std::string err;
        REQUIRE(tls_fetch_certificate(c, peer, err));
        CHECK(peer.have_digest);
        CHECK(peer.sha256 == tc.sha256);
        CHECK_FALSE(peer.chain_ok);
    }
    SUBCASE("protocol bounds: a TLS 1.2-only server and a 1.3 minimum do not meet") {
        TestServer srv(tc, TLS1_2_VERSION, TLS1_2_VERSION);
        Tn5250Config c = base;
        c.tls_verify = TlsVerify::None;
        c.tls_min_version = TlsVersion::Tls1_3;
        Outcome o = run_client(c, "127.0.0.1", srv.port);
        CHECK_FALSE(o.connected);
        CHECK(o.error.find("handshake") != std::string::npos);
    }
    SUBCASE("protocol bounds: a TLS 1.0 server needs a lowered minimum") {
        {
            TestServer srv(tc, TLS1_VERSION, TLS1_VERSION);
            Tn5250Config c = base;
            c.tls_verify = TlsVerify::None;
            Outcome o = run_client(c, "127.0.0.1", srv.port);
            CHECK_FALSE(o.connected);
            CHECK(o.error.find("tls_min_version") != std::string::npos);
        }
        {
            TestServer srv(tc, TLS1_VERSION, TLS1_VERSION);
            Tn5250Config c = base;
            c.tls_verify = TlsVerify::None;
            c.tls_min_version = TlsVersion::Tls1_0;
            Outcome o = run_client(c, "127.0.0.1", srv.port);
            CHECK(o.connected);
            CHECK(o.data == "hello");
            CHECK(o.log.has("legacy protocol"));
            CHECK(o.log.has("TLS negotiated TLSv1 "));
        }
    }
}
#endif
