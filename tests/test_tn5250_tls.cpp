// SPDX-License-Identifier: GPL-3.0-or-later
// TLS for the TN5250 client: fingerprints, configuration, the certificate
// decision, and (with the OpenSSL backend) a real handshake against an
// in-process server using a certificate generated at test time.
#include "app/host_link.hpp"
#include "config/config.hpp"
#include "host/host_config.hpp"
#include "net/socket.hpp"
#include "tn5250/tls_policy.hpp"
#include "tn5250/transport.hpp"
#include "util/fingerprint.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace em34xx;
using namespace em34xx::tn5250;

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
        const std::string path = write_temp("em34xx_test_tls1.toml",
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
        const std::string again = write_temp("em34xx_test_tls1b.toml", config_to_toml(cfg));
        AppConfig cfg2;
        REQUIRE(load_config_file(again, cfg2, err));
        CHECK(cfg2.tn5250.tls_verify == TlsVerify::None);
        CHECK(cfg2.tn5250.tls_min_version == TlsVersion::Tls1_0);
        CHECK(cfg2.tn5250.tls_max_version == TlsVersion::Tls1_2);
    }
    {
        const std::string path =
            write_temp("em34xx_test_tls2.toml", "[tn5250]\nhost = \"h\"\ntls = true\ntls_verify = false\n");
        AppConfig cfg;
        std::string err;
        REQUIRE(load_config_file(path, cfg, err));
        CHECK(cfg.tn5250.tls_verify == TlsVerify::None);   // the old boolean form
    }
    {
        const std::string path = write_temp("em34xx_test_tls3.toml",
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
        const std::string path = write_temp("em34xx_test_tls4.toml", "[tn5250]\ntls_verify = \"maybe\"\n");
        AppConfig cfg;
        std::string err;
        CHECK_FALSE(load_config_file(path, cfg, err));
        CHECK(err.find("tls_verify") != std::string::npos);
    }
    {
        const std::string path = write_temp("em34xx_test_tls5.toml", "[tn5250]\ntls_min_version = \"9\"\n");
        AppConfig cfg;
        std::string err;
        CHECK_FALSE(load_config_file(path, cfg, err));
    }
    {
        std::string bare;
        for (char ch : pin)
            if (ch != ':') bare += char(std::tolower(static_cast<unsigned char>(ch)));
        const char* argv[] = {"em34xx", "--tn5250-host", "h", "--tn5250-port", "992", "--tn5250-tls",
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
        const char* argv[] = {"em34xx", "--tn5250-tls", "--tn5250-tls-insecure"};
        AppConfig cfg;
        std::string err;
        REQUIRE(parse_command_line(3, const_cast<char**>(argv), cfg, err));
        CHECK(cfg.tn5250.tls_verify == TlsVerify::None);
    }
    {
        const char* argv[] = {"em34xx", "--tn5250-tls-pin", "abc"};
        AppConfig cfg;
        std::string err;
        CHECK_FALSE(parse_command_line(3, const_cast<char**>(argv), cfg, err));
        const char* argv2[] = {"em34xx", "--tn5250-tls-min", "any"};
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

TEST_CASE("TLS validity: expired, not yet valid and revoked certificates are their own class") {
    Tn5250Config cfg;
    cfg.host = "h";
    cfg.tls = true;
    PeerCertificate good;
    good.have_digest = true;
    good.sha256 = sample_digest();
    good.subject = "CN=h";
    good.issuer = "CN=Test CA";
    good.valid_from = "2020-01-01 00:00:00 UTC";
    good.expires = "2024-03-01 12:00:00 UTC";
    good.chain_ok = true;
    good.revocation_checked = true;
    PeerCertificate expired = good;
    expired.expired = true;
    expired.validity_errors = {"The certificate expired on 2024-03-01 12:00:00 UTC (test code 10)"};
    PeerCertificate revoked = good;
    revoked.revoked = true;
    revoked.validity_errors = {"The certificate has been revoked by its issuer (test code 23)"};
    std::string err;
    std::vector<TlsTrustRequest> asked;
    TlsTrustDecision answer = TlsTrustDecision::Reject;
    TlsTrustHandler ask = [&](const TlsTrustRequest& r) {
        asked.push_back(r);
        return answer;
    };
    const std::string pin = format_sha256_fingerprint(good.sha256);

    SUBCASE("expired but trusted, nobody to ask: refused with the exact error") {
        LogCapture log;
        CHECK_FALSE(tls_accept_peer(cfg, "h", expired, log.fn(), err));
        CHECK(err.find("expired on 2024-03-01 12:00:00 UTC (test code 10)") != std::string::npos);
        CHECK(log.has("TLS certificate is not valid: The certificate expired on"));
        CHECK(log.has("(expired)"));
    }
    SUBCASE("expired, with a handler: asked as Invalid; rejecting fails, accepting connects") {
        LogCapture log;
        CHECK_FALSE(tls_accept_peer(cfg, "h", expired, log.fn(), err, ask, 992));
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].problem == TlsTrustProblem::Invalid);
        CHECK(asked[0].host == "h");
        CHECK(asked[0].port == 992);
        CHECK(asked[0].errors == expired.validity_errors);   // trusted: nothing else to say
        CHECK(asked[0].reason == expired.validity_errors[0]);
        CHECK(asked[0].peer.chain_ok);
        CHECK(err.find("expired on") != std::string::npos);
        CHECK(log.has("rejected by the user"));
        answer = TlsTrustDecision::AcceptOnce;
        LogCapture log2;
        CHECK(tls_accept_peer(cfg, "h", expired, log2.fn(), err, ask, 992));
        CHECK(asked.size() == 2);
        CHECK(log2.has("for this connection only"));
    }
    SUBCASE("accept permanently counts as once: nothing says permanent") {
        answer = TlsTrustDecision::AcceptPermanently;
        LogCapture log;
        CHECK(tls_accept_peer(cfg, "h", expired, log.fn(), err, ask, 992));
        REQUIRE(asked.size() == 1);
        CHECK(log.has("for this connection only"));
        CHECK_FALSE(log.has("permanently"));
    }
    SUBCASE("expired and untrusted: one Invalid request with both errors") {
        PeerCertificate both = expired;
        both.chain_ok = false;
        both.chain_error = "self-signed certificate (depth 0)";
        CHECK_FALSE(tls_accept_peer(cfg, "h", both, nullptr, err, ask, 992));
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].problem == TlsTrustProblem::Invalid);
        REQUIRE(asked[0].errors.size() == 2);
        CHECK(asked[0].errors[0] == expired.validity_errors[0]);
        CHECK(asked[0].errors[1].find("not trusted: self-signed") != std::string::npos);
        CHECK(err.find("expired on") != std::string::npos);
        CHECK(err.find("self-signed") != std::string::npos);
    }
    SUBCASE("a pin match on an expired certificate still asks, every time") {
        cfg.tls_cert_sha256 = pin;
        answer = TlsTrustDecision::AcceptOnce;
        CHECK(tls_accept_peer(cfg, "h", expired, nullptr, err, ask, 992));
        CHECK(tls_accept_peer(cfg, "h", expired, nullptr, err, ask, 992));
        REQUIRE(asked.size() == 2);
        CHECK(asked[1].problem == TlsTrustProblem::Invalid);
        CHECK(asked[1].errors == expired.validity_errors);   // the pin matches: no trust error
        answer = TlsTrustDecision::Reject;
        CHECK_FALSE(tls_accept_peer(cfg, "h", expired, nullptr, err, ask, 992));
    }
    SUBCASE("a pin mismatch on an expired certificate: Invalid, with the mismatch listed") {
        Sha256Digest other = good.sha256;
        other[0] ^= 1;
        cfg.tls_cert_sha256 = format_sha256_fingerprint(other);
        CHECK_FALSE(tls_accept_peer(cfg, "h", expired, nullptr, err, ask, 992));
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].problem == TlsTrustProblem::Invalid);
        REQUIRE(asked[0].errors.size() == 2);
        CHECK(asked[0].errors[1].find("does not match the trusted SHA-256") != std::string::npos);
        CHECK_FALSE(tls_accept_peer(cfg, "h", expired, nullptr, err));   // and nobody to ask: refused
    }
    SUBCASE("a pin match on an expired certificate, nobody to ask: accepted with a warning") {
        cfg.tls_cert_sha256 = pin;
        LogCapture log;
        CHECK(tls_accept_peer(cfg, "h", expired, log.fn(), err));
        CHECK(log.has("matches the pinned"));
        CHECK(log.has("although it is not valid: The certificate expired on"));
        bool warned = false;
        for (LogLevel l : log.levels) warned = warned || l == LogLevel::Warn;
        CHECK(warned);
    }
    SUBCASE("revoked: refused without a handler, pin or not; asked with one") {
        CHECK_FALSE(tls_accept_peer(cfg, "h", revoked, nullptr, err));
        CHECK(err.find("revoked by its issuer") != std::string::npos);
        cfg.tls_cert_sha256 = pin;
        LogCapture log;
        CHECK_FALSE(tls_accept_peer(cfg, "h", revoked, log.fn(), err));
        CHECK(err.find("revoked by its issuer") != std::string::npos);
        CHECK(log.has("(revoked)"));
        answer = TlsTrustDecision::AcceptOnce;
        CHECK(tls_accept_peer(cfg, "h", revoked, nullptr, err, ask, 992));
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].problem == TlsTrustProblem::Invalid);
    }
    SUBCASE("not yet valid: the same class") {
        PeerCertificate early = good;
        early.not_yet_valid = true;   // flags alone: the policy words the line
        CHECK_FALSE(tls_accept_peer(cfg, "h", early, nullptr, err, ask, 992));
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].problem == TlsTrustProblem::Invalid);
        REQUIRE(asked[0].errors.size() == 1);
        CHECK(asked[0].errors[0] == "The certificate is not valid before 2020-01-01 00:00:00 UTC");
    }
    SUBCASE("revocation status unknown: accepted, not asked, one info line") {
        PeerCertificate unknown = good;
        unknown.revocation_checked = false;
        unknown.revocation_note = "the revocation server could not be reached";
        LogCapture log;
        CHECK(tls_accept_peer(cfg, "h", unknown, log.fn(), err, ask, 992));
        CHECK(asked.empty());
        CHECK(log.has("revocation status not checked: the revocation server could not be reached"));
        for (LogLevel l : log.levels) CHECK(l == LogLevel::Info);
        cfg.tls_cert_sha256 = pin;
        CHECK(tls_accept_peer(cfg, "h", unknown, nullptr, err, ask, 992));
        CHECK(asked.empty());
    }
    SUBCASE("verification off: accepted without asking, the errors logged") {
        cfg.tls_verify = TlsVerify::None;
        LogCapture log;
        CHECK(tls_accept_peer(cfg, "h", expired, log.fn(), err, ask, 992));
        CHECK(tls_accept_peer(cfg, "h", revoked, nullptr, err, ask, 992));
        CHECK(asked.empty());
        CHECK(log.has("verification is OFF"));
        CHECK(log.has("TLS certificate is not valid: The certificate expired on"));
    }
    SUBCASE("no validity error: the trust decisions are what they were") {
        PeerCertificate untrusted = good;
        untrusted.chain_ok = false;
        untrusted.chain_error = "self-signed certificate (depth 0)";
        CHECK(tls_accept_peer(cfg, "h", good, nullptr, err, ask, 992));
        CHECK(asked.empty());
        answer = TlsTrustDecision::AcceptPermanently;
        LogCapture log;
        CHECK(tls_accept_peer(cfg, "h", untrusted, log.fn(), err, ask, 992));
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].problem == TlsTrustProblem::Unverified);
        CHECK(asked[0].reason == untrusted.chain_error);
        REQUIRE(asked[0].errors.size() == 1);
        CHECK(log.has("accept permanently"));
        cfg.tls_cert_sha256 = pin;
        CHECK(tls_accept_peer(cfg, "h", untrusted, nullptr, err, ask, 992));   // the pin: no question
        CHECK(asked.size() == 1);
    }
}

TEST_CASE("TLS trust broker: a certificate that is not valid is asked about every time") {
    TlsTrustBroker broker;
    std::vector<TlsTrustProblem> calls;
    TlsTrustDecision answer = TlsTrustDecision::AcceptOnce;
    broker.set_handler([&](const TlsTrustRequest& r) {
        calls.push_back(r.problem);
        return answer;
    });
    TlsTrustRequest invalid;
    invalid.host = "h";
    invalid.port = 992;
    invalid.peer.have_digest = true;
    invalid.peer.sha256 = sample_digest();
    invalid.problem = TlsTrustProblem::Invalid;
    TlsTrustRequest unverified = invalid;   // the same host, port and certificate
    unverified.problem = TlsTrustProblem::Unverified;

    TlsTrustHandler bridge = broker.for_new_bridge();
    REQUIRE(bridge);
    CHECK(bridge(invalid) == TlsTrustDecision::AcceptOnce);
    CHECK(bridge(invalid) == TlsTrustDecision::AcceptOnce);      // a reconnect: asked again
    CHECK(calls.size() == 2);
    // Accepting an invalid certificate put nothing in the memory...
    CHECK(bridge(unverified) == TlsTrustDecision::AcceptOnce);
    CHECK(calls.size() == 3);
    CHECK(bridge(unverified) == TlsTrustDecision::AcceptOnce);   // ...an ordinary decision is remembered...
    CHECK(calls.size() == 3);
    // ...and does not answer for the certificate once it is not valid.
    answer = TlsTrustDecision::Reject;
    CHECK(bridge(invalid) == TlsTrustDecision::Reject);
    CHECK(calls.size() == 4);
    CHECK(calls.back() == TlsTrustProblem::Invalid);
    answer = TlsTrustDecision::AcceptPermanently;
    CHECK(bridge(invalid) == TlsTrustDecision::AcceptPermanently);   // passed through; the policy treats it as once
    CHECK(bridge(invalid) == TlsTrustDecision::AcceptPermanently);
    CHECK(calls.size() == 6);
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

TEST_CASE("TCP socket: loopback connect, and a cancel flag abandons the attempt") {
    // No ephemeral-port query in TcpSocket: try a few fixed ports.
    TcpSocket listener;
    uint16_t port = 0;
    std::string err;
    for (uint16_t p = 47361; p < 47369 && !port; ++p)
        if (listener.listen(p, true, err)) port = p;
    REQUIRE_MESSAGE(port != 0, err);

    TcpSocket client;
    REQUIRE_MESSAGE(client.connect("127.0.0.1", port, err), err);
    CHECK(err.empty());
    TcpSocket served = listener.accept(5000);
    REQUIRE(served.valid());
    REQUIRE(client.send_all("ping", 4));
    char buf[8] = {};
    size_t got = 0;
    while (got < 4) {
        const int n = served.recv(buf + got, sizeof buf - got, 5000);
        REQUIRE(n > 0);
        got += size_t(n);
    }
    CHECK(std::string(buf, got) == "ping");
    CHECK(client.recv(buf, sizeof buf, 0) == 0);   // nothing to read: a timeout, not an error
    served.close();
    CHECK(client.recv(buf, sizeof buf, 5000) == -1);

    // A flag that is already set: no attempt is made, the listener sees
    // nobody.
    const std::atomic<bool> cancel{true};
    TcpSocket refused;
    CHECK_FALSE(refused.connect("127.0.0.1", port, err, &cancel));
    CHECK_FALSE(refused.valid());
    CHECK(err.find("cancelled") != std::string::npos);
    CHECK_FALSE(listener.accept(0).valid());

    // The plain transport hands its flag to the socket.
    Tn5250Config cfg;
    cfg.host = "127.0.0.1";
    auto t = make_transport(cfg, err);
    REQUIRE(t);
    t->set_cancel(&cancel);
    CHECK_FALSE(t->connect("127.0.0.1", port, err));
    CHECK(err.find("cancelled") != std::string::npos);
    CHECK_FALSE(listener.accept(0).valid());
}

#ifdef EM34XX_TLS_OPENSSL
// --- live handshake against an in-process OpenSSL server ------------------

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ocsp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <csignal>
#include <cstring>
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

    // Self-signed, or signed by `ca`; valid from now + `from_s` to now +
    // `until_s` seconds. `san` null: a CA certificate ("em34xx test CA").
    explicit TestCert(const char* san, long from_s = -3600, long until_s = 86400, const TestCert* ca = nullptr) {
        key = EVP_EC_gen("P-256");
        cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), san ? (ca ? 2 : 1) : 100);
        X509_gmtime_adj(X509_getm_notBefore(cert), from_s);
        X509_gmtime_adj(X509_getm_notAfter(cert), until_s);
        X509_set_pubkey(cert, key);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(san ? "em34xx test" : "em34xx test CA"),
                                   -1, -1, 0);
        X509_set_issuer_name(cert, ca ? X509_get_subject_name(ca->cert) : name);
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, ca ? ca->cert : cert, cert, nullptr, nullptr, 0);
        X509_EXTENSION* ext = san ? X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, san)
                                  : X509V3_EXT_conf_nid(nullptr, &ctx, NID_basic_constraints, "critical,CA:TRUE");
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
        X509_sign(cert, ca ? ca->key : key, EVP_sha256());
        unsigned int len = 0;
        X509_digest(cert, EVP_sha256(), sha256.data(), &len);
        static int roots = 0;   // each CA keeps its own file
        pem_path = (std::filesystem::temp_directory_path() /
                    (san ? std::string("em34xx_test_tls_ca.pem") : "em34xx_test_tls_root" + std::to_string(++roots) + ".pem"))
                       .string();
        FILE* f = std::fopen(pem_path.c_str(), "wb");
        PEM_write_X509(f, cert);
        std::fclose(f);
    }
    ~TestCert() {
        X509_free(cert);
        EVP_PKEY_free(key);
    }
};

// A DER OCSP response about `leaf`, signed by its issuer `ca`: `status` is
// V_OCSP_CERTSTATUS_GOOD or _REVOKED.
std::vector<unsigned char> ocsp_response(const TestCert& leaf, const TestCert& ca, int status) {
    OCSP_BASICRESP* basic = OCSP_BASICRESP_new();
    OCSP_CERTID* id = OCSP_cert_to_id(EVP_sha256(), leaf.cert, ca.cert);
    ASN1_TIME* revoked_at = X509_gmtime_adj(nullptr, -600);
    ASN1_TIME* this_update = X509_gmtime_adj(nullptr, -60);
    ASN1_TIME* next_update = X509_gmtime_adj(nullptr, 3600);
    OCSP_basic_add1_status(basic, id, status, OCSP_REVOKED_STATUS_KEYCOMPROMISE,
                           status == V_OCSP_CERTSTATUS_REVOKED ? revoked_at : nullptr, this_update, next_update);
    OCSP_basic_sign(basic, ca.cert, ca.key, EVP_sha256(), nullptr, 0);
    OCSP_RESPONSE* resp = OCSP_response_create(OCSP_RESPONSE_STATUS_SUCCESSFUL, basic);
    unsigned char* der = nullptr;
    const int len = i2d_OCSP_RESPONSE(resp, &der);
    std::vector<unsigned char> out(der, der + (len > 0 ? len : 0));
    OPENSSL_free(der);
    OCSP_RESPONSE_free(resp);
    OCSP_BASICRESP_free(basic);
    OCSP_CERTID_free(id);
    ASN1_TIME_free(revoked_at);
    ASN1_TIME_free(this_update);
    ASN1_TIME_free(next_update);
    return out;
}

int staple_callback(SSL* ssl, void* arg) {
    const auto* der = static_cast<const std::vector<unsigned char>*>(arg);
    auto* copy = static_cast<unsigned char*>(OPENSSL_malloc(der->size()));
    std::memcpy(copy, der->data(), der->size());
    SSL_set_tlsext_status_ocsp_resp(ssl, copy, long(der->size()));   // takes the copy
    return SSL_TLSEXT_ERR_OK;
}

// Accepts one connection, completes the handshake, sends "hello", then
// waits for the client to go away. `staple`: an OCSP response to staple.
struct TestServer {
    int listen_fd = -1;
    uint16_t port = 0;
    std::thread thread;
    bool handshake_ok = false;
    std::vector<unsigned char> staple_;

    TestServer(const TestCert& tc, int min_version, int max_version, std::vector<unsigned char> staple = {})
        : staple_(std::move(staple)) {
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
        if (!staple_.empty()) {
            SSL_CTX_set_tlsext_status_cb(ctx, staple_callback);
            SSL_CTX_set_tlsext_status_arg(ctx, &staple_);
        }
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
        CHECK(asked[0].peer.subject.find("em34xx test") != std::string::npos);
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

namespace {

// Like run_client(), with a trust handler; returns what it was asked.
struct Asked {
    bool connected = false;
    std::string error;
    std::vector<TlsTrustRequest> requests;
    LogCapture log;
    PeerCertificate peer;
};

Asked run_client_asking(Tn5250Config cfg, const std::string& host, uint16_t port, TlsTrustDecision answer) {
    Asked a;
    cfg.host = host;
    cfg.port = port;
    cfg.tls = true;
    std::string err;
    auto t = make_transport(cfg, err);
    REQUIRE(t);
    t->set_log(a.log.fn());
    t->set_trust_handler([&](const TlsTrustRequest& r) {
        a.requests.push_back(r);
        return answer;
    });
    a.connected = t->connect(host, port, a.error);
    if (t->peer_certificate()) a.peer = *t->peer_certificate();
    t->close();
    return a;
}

} // namespace

TEST_CASE("TLS transport (OpenSSL): validity period errors are reported apart from trust") {
    std::signal(SIGPIPE, SIG_IGN);
    Tn5250Config base;

    SUBCASE("expired and self-signed: both errors; refused without a handler") {
        const TestCert tc("DNS:localhost", -7200, -3600);
        {
            TestServer srv(tc, TLS1_2_VERSION, 0);
            Outcome o = run_client(base, "127.0.0.1", srv.port);
            CHECK_FALSE(o.connected);
            CHECK(o.error.find("The certificate expired on") != std::string::npos);
            CHECK(o.error.find("not trusted") != std::string::npos);
        }
        {
            TestServer srv(tc, TLS1_2_VERSION, 0);
            Asked a = run_client_asking(base, "127.0.0.1", srv.port, TlsTrustDecision::AcceptOnce);
            CHECK(a.connected);
            REQUIRE(a.requests.size() == 1);
            const TlsTrustRequest& r = a.requests[0];
            CHECK(r.problem == TlsTrustProblem::Invalid);
            CHECK(r.peer.expired);
            CHECK_FALSE(r.peer.not_yet_valid);
            CHECK_FALSE(r.peer.chain_ok);
            CHECK(r.peer.chain_error.find("expired") == std::string::npos);   // trust only
            REQUIRE(r.peer.validity_errors.size() == 1);
            CHECK(r.peer.validity_errors[0].find("The certificate expired on 20") == 0);
            CHECK(r.peer.validity_errors[0].find("X509 error 10") != std::string::npos);
            CHECK(r.errors.size() == 2);
        }
    }
    SUBCASE("expired but from a trusted CA: trust is fine, validity is not") {
        const TestCert ca(nullptr);
        const TestCert leaf("DNS:localhost", -7200, -3600, &ca);
        Tn5250Config c = base;
        c.ca_file = ca.pem_path;
        {
            TestServer srv(leaf, TLS1_2_VERSION, 0);
            Asked a = run_client_asking(c, "localhost", srv.port, TlsTrustDecision::Reject);
            CHECK_FALSE(a.connected);
            REQUIRE(a.requests.size() == 1);
            CHECK(a.requests[0].problem == TlsTrustProblem::Invalid);
            CHECK(a.requests[0].peer.chain_ok);
            CHECK(a.requests[0].peer.chain_error.empty());
            CHECK(a.requests[0].errors.size() == 1);
        }
        {   // pinned as well: still asked
            TestServer srv(leaf, TLS1_2_VERSION, 0);
            Tn5250Config p = c;
            p.tls_cert_sha256 = format_sha256_fingerprint(leaf.sha256);
            Asked a = run_client_asking(p, "localhost", srv.port, TlsTrustDecision::AcceptOnce);
            CHECK(a.connected);
            REQUIRE(a.requests.size() == 1);
            CHECK(a.requests[0].problem == TlsTrustProblem::Invalid);
        }
        {   // pinned, nobody to ask: accepted with the warning
            TestServer srv(leaf, TLS1_2_VERSION, 0);
            Tn5250Config p = c;
            p.tls_cert_sha256 = format_sha256_fingerprint(leaf.sha256);
            Outcome o = run_client(p, "localhost", srv.port);
            CHECK(o.connected);
            CHECK(o.log.has("although it is not valid"));
        }
    }
    SUBCASE("not yet valid") {
        const TestCert ca(nullptr);
        const TestCert leaf("DNS:localhost", 3600, 86400, &ca);
        Tn5250Config c = base;
        c.ca_file = ca.pem_path;
        TestServer srv(leaf, TLS1_2_VERSION, 0);
        Asked a = run_client_asking(c, "localhost", srv.port, TlsTrustDecision::Reject);
        CHECK_FALSE(a.connected);
        REQUIRE(a.requests.size() == 1);
        CHECK(a.requests[0].peer.not_yet_valid);
        CHECK_FALSE(a.requests[0].peer.expired);
        CHECK(a.requests[0].peer.chain_ok);
        REQUIRE(a.requests[0].errors.size() == 1);
        CHECK(a.requests[0].errors[0].find("The certificate is not valid before 20") == 0);
    }
    SUBCASE("fetch certificate: the validity errors come with it") {
        const TestCert tc("DNS:localhost", -7200, -3600);
        TestServer srv(tc, TLS1_2_VERSION, 0);
        Tn5250Config c = base;
        c.host = "127.0.0.1";
        c.port = srv.port;
        PeerCertificate peer;
        std::string err;
        REQUIRE(tls_fetch_certificate(c, peer, err));
        CHECK(peer.expired);
        CHECK(peer.validity_errors.size() == 1);
        CHECK_FALSE(peer.chain_ok);
    }
}

TEST_CASE("TLS transport (OpenSSL): revocation from a stapled OCSP response") {
    std::signal(SIGPIPE, SIG_IGN);
    const TestCert ca(nullptr);
    const TestCert leaf("DNS:localhost", -3600, 86400, &ca);
    Tn5250Config c;
    c.ca_file = ca.pem_path;

    SUBCASE("no staple: not checked, and that is not an error") {
        TestServer srv(leaf, TLS1_2_VERSION, 0);
        Asked a = run_client_asking(c, "localhost", srv.port, TlsTrustDecision::Reject);
        CHECK(a.connected);
        CHECK(a.requests.empty());
        CHECK_FALSE(a.peer.revocation_checked);
        CHECK_FALSE(a.peer.revoked);
        CHECK(a.log.has("revocation status not checked: the server stapled no OCSP response"));
    }
    SUBCASE("good staple: checked") {
        for (int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
            TestServer srv(leaf, version, version, ocsp_response(leaf, ca, V_OCSP_CERTSTATUS_GOOD));
            Asked a = run_client_asking(c, "localhost", srv.port, TlsTrustDecision::Reject);
            CHECK(a.connected);
            CHECK(a.requests.empty());
            CHECK(a.peer.revocation_checked);
            CHECK_FALSE(a.peer.revoked);
            CHECK_FALSE(a.log.has("revocation status not checked"));
        }
    }
    SUBCASE("revoked staple: a validity error; refused without a handler even when pinned") {
        for (int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
            {
                TestServer srv(leaf, version, version, ocsp_response(leaf, ca, V_OCSP_CERTSTATUS_REVOKED));
                Asked a = run_client_asking(c, "localhost", srv.port, TlsTrustDecision::AcceptOnce);
                CHECK(a.connected);
                REQUIRE(a.requests.size() == 1);
                CHECK(a.requests[0].problem == TlsTrustProblem::Invalid);
                CHECK(a.requests[0].peer.revoked);
                CHECK(a.requests[0].peer.chain_ok);
                REQUIRE(a.requests[0].errors.size() == 1);
                CHECK(a.requests[0].errors[0].find("revoked by its issuer on 20") != std::string::npos);
                CHECK(a.requests[0].errors[0].find("keyCompromise") != std::string::npos);
            }
            {
                TestServer srv(leaf, version, version, ocsp_response(leaf, ca, V_OCSP_CERTSTATUS_REVOKED));
                Tn5250Config p = c;
                p.tls_cert_sha256 = format_sha256_fingerprint(leaf.sha256);
                Outcome o = run_client(p, "localhost", srv.port);
                CHECK_FALSE(o.connected);
                CHECK(o.error.find("revoked") != std::string::npos);
            }
        }
    }
    SUBCASE("a staple signed by someone else: not usable, so not checked") {
        const TestCert other_ca(nullptr);   // same name, another key
        TestServer srv(leaf, TLS1_2_VERSION, 0, ocsp_response(leaf, other_ca, V_OCSP_CERTSTATUS_REVOKED));
        Asked a = run_client_asking(c, "localhost", srv.port, TlsTrustDecision::Reject);
        CHECK(a.connected);
        CHECK(a.requests.empty());
        CHECK_FALSE(a.peer.revocation_checked);
        CHECK_FALSE(a.peer.revoked);
    }
}
#endif
