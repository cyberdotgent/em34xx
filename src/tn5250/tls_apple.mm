// SPDX-License-Identifier: GPL-3.0-or-later
// TLS transport, Apple backend (macOS): the system's own stack, so a
// universal build needs no third-party library. Network.framework makes the
// TCP connection and runs TLS (TLS 1.0 to 1.3); Security.framework judges
// the certificate (SecTrust) and describes it. Implicit TLS from the first
// byte, port 992 by convention. Objective-C++ with ARC.
//
// Network.framework is asynchronous: the connection's callbacks run on a
// private serial queue and hand their results to the calling thread through
// a mutex and a condition variable (Link). Transport's blocking calls wait
// on that with their timeouts; at most one receive is outstanding, and what
// it delivers is buffered, so recv() can time out without losing data.
//
// The handshake never fails over the certificate: the verify block records
// the server's SecTrust and says yes. Once the connection is ready (no
// application data has moved), the certificate is judged on the calling
// thread, as with the other backends, and tls_accept_peer() decides; the
// trust handler may block there for as long as the user takes.
//
// Trust: a fresh SecTrust from the certificates the server sent, with the
// SSL policy for the host name (an IP address is checked against the
// certificate's IP entries), the system's trust settings (keychains,
// configuration profiles) and Tn5250Config::ca_file as extra anchors.
// Validity periods are judged apart: each certificate of the built chain
// has its dates compared with the clock, and when macOS's answer was a date
// error, trust is asked again at a moment every certificate is valid (or,
// failing one, the leaf), so that the dates are not part of the trust
// answer.
//
// Revocation: a second evaluation with the basic X.509 policy and
// SecPolicyCreateRevocation(any available method, positive response
// required), network access allowed: macOS's own OCSP / CRL checking (and
// its caches) decides. "Revoked" is a validity error; anything short of a
// definite answer leaves the status undetermined, which is not an error.
// It runs after the handshake on the session's I/O thread, so a slow
// revocation server delays the connect.
#include "tn5250/tls_policy.hpp"

#include "util/strings.hpp"

#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>
#import <Network/Network.h>
#import <Security/Security.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace em34xx {
namespace tn5250 {

namespace {

using steady = std::chrono::steady_clock;

constexpr int kConnectTimeoutMs = 10000;      // as TcpSocket::kConnectTimeoutMs
constexpr int kHandshakeDeadlineMs = 30000;   // the whole handshake, or one send()
constexpr int kCloseWaitMs = 2000;

// What the connection's callbacks report, shared with them (they may
// outlive the transport by a little).
struct Link {
    std::mutex mu;
    std::condition_variable cv;
    bool ready = false;
    bool ended = false;            // failed, or waiting (could not connect)
    bool cancelled = false;
    nw_error_t error = nil;        // why it ended
    SecTrustRef trust = nullptr;   // from the verify block (owned)
    // Receiving.
    std::deque<uint8_t> in;
    bool receiving = false;
    bool eof = false;
    bool read_failed = false;
    std::string read_error;

    ~Link() {
        if (trust) CFRelease(trust);
    }
};

template <class T>
struct CfRef {
    T ref = nullptr;
    CfRef() = default;
    explicit CfRef(T r) : ref(r) {}
    ~CfRef() {
        if (ref) CFRelease(ref);
    }
    CfRef(const CfRef&) = delete;
    CfRef& operator=(const CfRef&) = delete;
    T get() const { return ref; }
    explicit operator bool() const { return ref != nullptr; }
};

std::string to_utf8(CFStringRef s) {
    if (!s) return std::string();
    if (const char* p = CFStringGetCStringPtr(s, kCFStringEncodingUTF8)) return p;
    const CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::vector<char> buf(size_t(max > 0 ? max : 1));
    if (!CFStringGetCString(s, buf.data(), max, kCFStringEncodingUTF8)) return std::string();
    return buf.data();
}

// The system's text for an OSStatus, with the number.
std::string status_text(OSStatus st) {
    CfRef<CFStringRef> msg(SecCopyErrorMessageString(st, nullptr));
    const std::string text = to_utf8(msg.get());
    return text.empty() ? format("OSStatus %d", int(st)) : format("%s (OSStatus %d)", text.c_str(), int(st));
}

std::string cf_error_text(CFErrorRef e) {
    if (!e) return "unknown error";
    CfRef<CFStringRef> desc(CFErrorCopyDescription(e));
    std::string text = to_utf8(desc.get());
    if (text.empty()) text = "error";
    return format("%s (OSStatus %ld)", text.c_str(), long(CFErrorGetCode(e)));
}

// "YYYY-MM-DD HH:MM:SS UTC".
std::string time_text(CFAbsoluteTime t) {
    const time_t tt = time_t(t + kCFAbsoluteTimeIntervalSince1970);
    struct tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", &tm);
    return buf;
}

// A distinguished name from SecCertificateCopyValues, most specific first
// ("CN=host,O=Org,C=BE"), as OpenSSL prints RFC 2253.
std::string name_text(NSArray* parts) {
    static NSDictionary* const kShort = @{
        @"2.5.4.3" : @"CN", @"2.5.4.6" : @"C", @"2.5.4.7" : @"L", @"2.5.4.8" : @"ST", @"2.5.4.10" : @"O",
        @"2.5.4.11" : @"OU", @"2.5.4.5" : @"serialNumber", @"2.5.4.9" : @"street", @"2.5.4.17" : @"postalCode",
        @"0.9.2342.19200300.100.1.25" : @"DC", @"1.2.840.113549.1.9.1" : @"emailAddress",
    };
    std::string out;
    if (![parts isKindOfClass:[NSArray class]]) return "?";
    for (NSDictionary* part in [parts reverseObjectEnumerator]) {
        if (![part isKindOfClass:[NSDictionary class]]) continue;
        id label = part[(__bridge NSString*)kSecPropertyKeyLabel];
        id value = part[(__bridge NSString*)kSecPropertyKeyValue];
        if (![label isKindOfClass:[NSString class]] || ![value isKindOfClass:[NSString class]]) continue;
        NSString* key = kShort[label] ? kShort[label] : label;
        if (!out.empty()) out += ",";
        out += std::string([key UTF8String]) + "=" + [value UTF8String];
    }
    return out.empty() ? "?" : out;
}

struct LeafFacts {
    std::string subject = "?", issuer = "?";
    bool have_dates = false;
    CFAbsoluteTime not_before = 0, not_after = 0;
};

LeafFacts leaf_facts(SecCertificateRef cert) {
    LeafFacts f;
    NSArray* keys = @[
        (__bridge NSString*)kSecOIDX509V1SubjectName, (__bridge NSString*)kSecOIDX509V1IssuerName,
        (__bridge NSString*)kSecOIDX509V1ValidityNotBefore, (__bridge NSString*)kSecOIDX509V1ValidityNotAfter
    ];
    NSDictionary* values =
        CFBridgingRelease(SecCertificateCopyValues(cert, (__bridge CFArrayRef)keys, nullptr));
    if (!values) {
        CfRef<CFStringRef> summary(SecCertificateCopySubjectSummary(cert));
        if (summary) f.subject = "CN=" + to_utf8(summary.get());
        return f;
    }
    auto value_of = [&](CFStringRef oid) -> id {
        NSDictionary* entry = values[(__bridge NSString*)oid];
        return [entry isKindOfClass:[NSDictionary class]] ? entry[(__bridge NSString*)kSecPropertyKeyValue] : nil;
    };
    f.subject = name_text(value_of(kSecOIDX509V1SubjectName));
    f.issuer = name_text(value_of(kSecOIDX509V1IssuerName));
    id nb = value_of(kSecOIDX509V1ValidityNotBefore), na = value_of(kSecOIDX509V1ValidityNotAfter);
    if ([nb isKindOfClass:[NSNumber class]] && [na isKindOfClass:[NSNumber class]]) {
        f.not_before = [nb doubleValue];
        f.not_after = [na doubleValue];
        f.have_dates = true;
    }
    return f;
}

// The certificates the server sent, leaf first.
NSArray* sent_certificates(SecTrustRef trust) {
    if (@available(macOS 12.0, *)) {
        NSArray* chain = CFBridgingRelease(SecTrustCopyCertificateChain(trust));
        return chain ? chain : @[];
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSMutableArray* out = [NSMutableArray array];
    const CFIndex n = SecTrustGetCertificateCount(trust);
    for (CFIndex i = 0; i < n; ++i)
        if (SecCertificateRef c = SecTrustGetCertificateAtIndex(trust, i)) [out addObject:(__bridge id)c];
    return out;
#pragma clang diagnostic pop
}

// PEM certificates of a ca_file.
bool read_ca_file(const std::string& path, NSMutableArray* out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "TLS: cannot open ca_file " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    const std::string begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----";
    for (size_t pos = text.find(begin); pos != std::string::npos; pos = text.find(begin, pos)) {
        const size_t body = pos + begin.size();
        const size_t stop = text.find(end, body);
        if (stop == std::string::npos) break;
        NSString* b64 = [[NSString alloc] initWithBytes:text.data() + body
                                                 length:stop - body
                                               encoding:NSASCIIStringEncoding];
        NSData* der = b64 ? [[NSData alloc] initWithBase64EncodedString:b64
                                                                options:NSDataBase64DecodingIgnoreUnknownCharacters]
                          : nil;
        if (der) {
            SecCertificateRef c = SecCertificateCreateWithData(nullptr, (__bridge CFDataRef)der);
            if (c) [out addObject:CFBridgingRelease(c)];
        }
        pos = stop + end.size();
    }
    if (out.count == 0) {
        error = "TLS: no PEM certificates in ca_file " + path;
        return false;
    }
    return true;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
tls_protocol_version_t protocol_number(TlsVersion v) {
    switch (v) {
    case TlsVersion::Any:
    case TlsVersion::Ssl3:
    case TlsVersion::Tls1_0: return tls_protocol_version_TLSv10;
    case TlsVersion::Tls1_1: return tls_protocol_version_TLSv11;
    case TlsVersion::Tls1_2: return tls_protocol_version_TLSv12;
    case TlsVersion::Tls1_3: return tls_protocol_version_TLSv13;
    }
    return tls_protocol_version_TLSv12;
}

const char* protocol_name(tls_protocol_version_t v) {
    switch (v) {
    case tls_protocol_version_TLSv10: return "TLSv1";
    case tls_protocol_version_TLSv11: return "TLSv1.1";
    case tls_protocol_version_TLSv12: return "TLSv1.2";
    case tls_protocol_version_TLSv13: return "TLSv1.3";
    default: return "TLS (unknown version)";
    }
}
#pragma clang diagnostic pop

std::string suite_name(uint16_t s) {
    switch (s) {
    case 0x1301: return "TLS_AES_128_GCM_SHA256";
    case 0x1302: return "TLS_AES_256_GCM_SHA384";
    case 0x1303: return "TLS_CHACHA20_POLY1305_SHA256";
    case 0xC02B: return "ECDHE-ECDSA-AES128-GCM-SHA256";
    case 0xC02C: return "ECDHE-ECDSA-AES256-GCM-SHA384";
    case 0xC02F: return "ECDHE-RSA-AES128-GCM-SHA256";
    case 0xC030: return "ECDHE-RSA-AES256-GCM-SHA384";
    case 0xCCA8: return "ECDHE-RSA-CHACHA20-POLY1305";
    case 0xCCA9: return "ECDHE-ECDSA-CHACHA20-POLY1305";
    case 0xC013: return "ECDHE-RSA-AES128-SHA";
    case 0xC014: return "ECDHE-RSA-AES256-SHA";
    case 0x002F: return "AES128-SHA";
    case 0x0035: return "AES256-SHA";
    case 0x009C: return "AES128-GCM-SHA256";
    case 0x009D: return "AES256-GCM-SHA384";
    default: return format("cipher suite 0x%04X", unsigned(s));
    }
}

// Why a connection ended, in the words the other backends use.
std::string end_reason(nw_error_t e, const std::string& host, uint16_t port, bool handshake_started,
                       const Tn5250Config& cfg) {
    if (!e) return format("cannot connect to %s:%u", host.c_str(), port);
    const nw_error_domain_t domain = nw_error_get_error_domain(e);
    const int code = nw_error_get_error_code(e);
    if (domain == nw_error_domain_dns) return format("cannot resolve %s", host.c_str());
    if (domain == nw_error_domain_tls) {
        std::string error =
            format("TLS handshake with %s:%u failed: %s", host.c_str(), port, status_text(OSStatus(code)).c_str());
        if (port == 23) error += " (port 23 is plain telnet; TLS servers usually listen on 992)";
        else if (cfg.tls_min_version >= TlsVersion::Tls1_2) error += " (an old host may need a lower tls_min_version)";
        return error;
    }
    if (domain == nw_error_domain_posix) {
        if (handshake_started)
            return format("TLS handshake with %s:%u failed: %s", host.c_str(), port, std::strerror(code));
        return format("cannot connect to %s:%u (%s)", host.c_str(), port, std::strerror(code));
    }
    CFErrorRef cf = nw_error_copy_cf_error(e);
    const std::string text = cf_error_text(cf);
    if (cf) CFRelease(cf);
    return format("cannot connect to %s:%u (%s)", host.c_str(), port, text.c_str());
}

bool is_date_error(CFIndex code) { return code == errSecCertificateExpired || code == errSecCertificateNotValidYet; }

// Why macOS gave no revocation answer, in plain words (its own text for
// errSecIncompleteCertRevocationCheck speaks of "pinning requirements").
std::string revocation_note_text(CFErrorRef e) {
    const CFIndex code = CFErrorGetCode(e);
    if (code == errSecIncompleteCertRevocationCheck)
        return format("macOS had no OCSP or CRL answer for it (errSecIncompleteCertRevocationCheck, %ld)", long(code));
    if (code == errSecNetworkFailure)
        return format("the revocation server could not be reached (errSecNetworkFailure, %ld)", long(code));
    return "macOS could not determine it: " + cf_error_text(e);
}

// SecTrust evaluation of `certs` with `policies` at `when`, with the
// ca_file anchors on top of the system's, network fetches allowed (missing
// intermediates, OCSP / CRL). Null error: it passed. `built` (optional)
// receives the chain the evaluation built, leaf first.
CFErrorRef evaluate(NSArray* certs, NSArray* policies, NSArray* anchors, CFAbsoluteTime when, NSArray** built) {
    SecTrustRef raw = nullptr;
    if (SecTrustCreateWithCertificates((__bridge CFArrayRef)certs, (__bridge CFArrayRef)policies, &raw) !=
            errSecSuccess ||
        !raw) {
        return CFErrorCreate(nullptr, kCFErrorDomainOSStatus, errSecInternalComponent, nullptr);
    }
    CfRef<SecTrustRef> trust(raw);
    if (anchors.count) {
        SecTrustSetAnchorCertificates(trust.get(), (__bridge CFArrayRef)anchors);
        SecTrustSetAnchorCertificatesOnly(trust.get(), false);
    }
    CfRef<CFDateRef> date(CFDateCreate(nullptr, when));
    SecTrustSetVerifyDate(trust.get(), date.get());
    SecTrustSetNetworkFetchAllowed(trust.get(), true);
    CFErrorRef error = nullptr;
    const bool ok = SecTrustEvaluateWithError(trust.get(), &error);
    if (built) *built = sent_certificates(trust.get());
    if (ok) {
        if (error) CFRelease(error);
        return nullptr;
    }
    return error ? error : CFErrorCreate(nullptr, kCFErrorDomainOSStatus, errSecNotTrusted, nullptr);
}

class AppleTransport : public Transport {
public:
    explicit AppleTransport(const Tn5250Config& cfg) : cfg_(cfg) {}
    ~AppleTransport() override { close(); }

    bool connect(const std::string& host, uint16_t port, std::string& error) override {
        close();
        const std::atomic<bool>* cancel = cancel_flag();
        if (cancel && cancel->load()) {
            error = "connection attempt cancelled";
            return false;
        }
        anchors_ = [NSMutableArray array];
        if (!cfg_.ca_file.empty()) {
            if (!read_ca_file(cfg_.ca_file, anchors_, error)) return false;
            note(LogLevel::Debug, "TLS: added the CA certificates in " + cfg_.ca_file);
        }
        if (cfg_.tls_max_version != TlsVersion::Any && cfg_.tls_max_version < TlsVersion::Tls1_0) {
            error = "TLS: Network.framework offers TLS 1.0 at the oldest; tls_max_version = \"ssl3\" leaves nothing";
            return false;
        }
        if (cfg_.tls_min_version == TlsVersion::Ssl3)
            note(LogLevel::Warn, "TLS: Network.framework has no SSL 3.0; TLS 1.0 is the oldest version offered");
        if (cfg_.tls_min_version < TlsVersion::Tls1_2)
            tls_note_legacy(cfg_, [this](LogLevel l, const std::string& s) { note(l, s); });

        link_ = std::make_shared<Link>();
        queue_ = dispatch_queue_create("em34xx.tn5250.tls", DISPATCH_QUEUE_SERIAL);
        const std::shared_ptr<Link> link = link_;
        dispatch_queue_t queue = queue_;
        const tls_protocol_version_t min_version = protocol_number(cfg_.tls_min_version);
        const bool have_max = cfg_.tls_max_version != TlsVersion::Any;
        const tls_protocol_version_t max_version = protocol_number(cfg_.tls_max_version);

        nw_parameters_t params = nw_parameters_create_secure_tcp(
            ^(nw_protocol_options_t tls_options) {
                sec_protocol_options_t sec = nw_tls_copy_sec_protocol_options(tls_options);
                sec_protocol_options_set_min_tls_protocol_version(sec, min_version);
                if (have_max) sec_protocol_options_set_max_tls_protocol_version(sec, max_version);
                // Record the server's trust and carry on: the decision is
                // taken after the handshake (tls_accept_peer()).
                sec_protocol_options_set_verify_block(
                    sec,
                    ^(sec_protocol_metadata_t, sec_trust_t trust, sec_protocol_verify_complete_t complete) {
                        {
                            std::lock_guard<std::mutex> g(link->mu);
                            if (link->trust) CFRelease(link->trust);
                            link->trust = sec_trust_copy_ref(trust);
                        }
                        complete(true);
                    },
                    queue);
            },
            NW_PARAMETERS_DEFAULT_CONFIGURATION);
        nw_endpoint_t endpoint = nw_endpoint_create_host(host.c_str(), std::to_string(port).c_str());
        conn_ = nw_connection_create(endpoint, params);
        nw_connection_set_queue(conn_, queue_);
        nw_connection_set_state_changed_handler(conn_, ^(nw_connection_state_t state, nw_error_t err) {
            std::lock_guard<std::mutex> g(link->mu);
            switch (state) {
            case nw_connection_state_ready: link->ready = true; break;
            case nw_connection_state_waiting:   // no route, refused...: give up, as a socket would
            case nw_connection_state_failed:
                link->ended = true;
                if (err) link->error = err;
                if (link->ready) {
                    link->read_failed = true;
                    if (link->read_error.empty() && err) {
                        CFErrorRef cf = nw_error_copy_cf_error(err);
                        link->read_error = cf_error_text(cf);
                        if (cf) CFRelease(cf);
                    }
                }
                break;
            case nw_connection_state_cancelled: link->cancelled = true; break;
            default: break;
            }
            link->cv.notify_all();
        });
        nw_connection_start(conn_);

        // Connect and handshake, polling the cancel flag.
        const auto deadline = steady::now() + std::chrono::milliseconds(kConnectTimeoutMs + kHandshakeDeadlineMs);
        {
            std::unique_lock<std::mutex> lock(link->mu);
            while (!link->ready && !link->ended && !link->cancelled) {
                if (cancel && cancel->load()) {
                    lock.unlock();
                    close();
                    error = "connection attempt cancelled";
                    return false;
                }
                if (steady::now() >= deadline) {
                    lock.unlock();
                    close();
                    error = format("TLS handshake with %s:%u failed: no TLS session within %d s", host.c_str(), port,
                                   (kConnectTimeoutMs + kHandshakeDeadlineMs) / 1000);
                    return false;
                }
                link->cv.wait_for(lock, std::chrono::milliseconds(100));
            }
            if (!link->ready) {
                // A TLS alert or failure is reported in the TLS domain; a
                // verify block that ran means TCP had connected.
                error = end_reason(link->error, host, port, link->trust != nullptr, cfg_);
                lock.unlock();
                close();
                return false;
            }
        }
        port_ = port;
        summary_ = negotiated();
        note(LogLevel::Info, "TLS negotiated " + summary_);
        peer_ = remote_address(host, port);
        if (!check_peer(host, error)) {
            close();
            return false;
        }
        return true;
    }

    int recv(uint8_t* buf, size_t len, int timeout_ms) override {
        if (!conn_ || !link_ || failed_) return -1;
        std::unique_lock<std::mutex> lock(link_->mu);
        if (link_->in.empty()) {
            if (link_->eof || link_->read_failed) return finish_read(lock);
            start_receive();
            if (timeout_ms > 0)
                link_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                   [&] { return !link_->in.empty() || link_->eof || link_->read_failed; });
            if (link_->in.empty()) return link_->eof || link_->read_failed ? finish_read(lock) : 0;
        }
        const size_t n = std::min(len, link_->in.size());
        std::copy(link_->in.begin(), link_->in.begin() + std::ptrdiff_t(n), buf);
        link_->in.erase(link_->in.begin(), link_->in.begin() + std::ptrdiff_t(n));
        return int(n);
    }

    bool send(const uint8_t* buf, size_t len) override {
        if (!conn_ || !link_ || failed_) return false;
        if (len == 0) return true;
        struct Sent {
            bool done = false;
            bool ok = false;
        };
        auto sent = std::make_shared<Sent>();
        const std::shared_ptr<Link> link = link_;
        dispatch_data_t data = dispatch_data_create(buf, len, queue_, DISPATCH_DATA_DESTRUCTOR_DEFAULT);   // copies
        nw_connection_send(conn_, data, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, false, ^(nw_error_t err) {
            std::lock_guard<std::mutex> g(link->mu);
            sent->done = true;
            sent->ok = err == nil;
            link->cv.notify_all();
        });
        std::unique_lock<std::mutex> lock(link->mu);
        // The deadline keeps a silent peer from holding send() for ever.
        if (!link->cv.wait_for(lock, std::chrono::milliseconds(kHandshakeDeadlineMs), [&] { return sent->done; })) {
            lock.unlock();
            note(LogLevel::Debug,
                 format("TLS write: the peer did not let it finish within %d s", kHandshakeDeadlineMs / 1000));
            failed_ = true;
            return false;
        }
        if (!sent->ok) failed_ = true;
        return sent->ok;
    }

    void close() override {
        if (conn_) {
            nw_connection_cancel(conn_);
            if (link_) {
                std::unique_lock<std::mutex> lock(link_->mu);
                link_->cv.wait_for(lock, std::chrono::milliseconds(kCloseWaitMs), [&] { return link_->cancelled; });
            }
            conn_ = nil;
        }
        link_.reset();
        queue_ = nil;
        anchors_ = nil;
        failed_ = false;
    }

    std::string describe() const override { return "tls " + peer_ + " " + summary_; }

private:
    // With link_->mu held: one receive at a time, and only while the buffer
    // is empty, so an idle reader keeps TCP's flow control.
    void start_receive() {
        if (link_->receiving || link_->eof || link_->read_failed) return;
        link_->receiving = true;
        const std::shared_ptr<Link> link = link_;
        nw_connection_receive(conn_, 1, 65536,
                              ^(dispatch_data_t content, nw_content_context_t, bool is_complete, nw_error_t err) {
                                  std::lock_guard<std::mutex> g(link->mu);
                                  if (content) {
                                      dispatch_data_apply(content, ^bool(dispatch_data_t, size_t, const void* p,
                                                                         size_t n) {
                                          const uint8_t* b = static_cast<const uint8_t*>(p);
                                          link->in.insert(link->in.end(), b, b + n);
                                          return true;
                                      });
                                  }
                                  if (err) {
                                      link->read_failed = true;
                                      CFErrorRef cf = nw_error_copy_cf_error(err);
                                      link->read_error = cf_error_text(cf);
                                      if (cf) CFRelease(cf);
                                  } else if (is_complete) {
                                      link->eof = true;   // the peer closed its side
                                  }
                                  link->receiving = false;
                                  link->cv.notify_all();
                              });
    }

    int finish_read(std::unique_lock<std::mutex>& lock) {
        const std::string why = link_->read_failed ? link_->read_error : std::string();
        lock.unlock();
        failed_ = true;
        if (!why.empty()) note(LogLevel::Debug, "TLS read: " + why);
        return -1;
    }

    std::string negotiated() {
        nw_protocol_definition_t def = nw_protocol_copy_tls_definition();
        nw_protocol_metadata_t meta = nw_connection_copy_protocol_metadata(conn_, def);
        if (!meta) return "TLS";
        sec_protocol_metadata_t sec = nw_tls_copy_sec_protocol_metadata(meta);
        if (!sec) return "TLS";
        const tls_protocol_version_t v = sec_protocol_metadata_get_negotiated_tls_protocol_version(sec);
        const tls_ciphersuite_t s = sec_protocol_metadata_get_negotiated_tls_ciphersuite(sec);
        return format("%s %s", protocol_name(v), suite_name(uint16_t(s)).c_str());
    }

    std::string remote_address(const std::string& host, uint16_t port) {
        nw_path_t path = nw_connection_copy_current_path(conn_);
        nw_endpoint_t remote = path ? nw_path_copy_effective_remote_endpoint(path) : nil;
        const sockaddr* sa = remote && nw_endpoint_get_type(remote) == nw_endpoint_type_address
                                 ? nw_endpoint_get_address(remote)
                                 : nullptr;
        char buf[64] = {};
        if (sa && sa->sa_family == AF_INET)
            inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, buf, sizeof buf);
        else if (sa && sa->sa_family == AF_INET6)
            inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, buf, sizeof buf);
        return format("%s:%u", buf[0] ? buf : host.c_str(), port);
    }

    bool check_peer(const std::string& host, std::string& error) {
        NSArray* certs = nil;
        {
            std::lock_guard<std::mutex> g(link_->mu);
            if (link_->trust) certs = sent_certificates(link_->trust);
        }
        if (certs.count == 0) {
            error = "TLS: the server sent no certificate";
            return false;
        }
        SecCertificateRef leaf = (__bridge SecCertificateRef)certs[0];
        PeerCertificate peer;
        NSData* der = CFBridgingRelease(SecCertificateCopyData(leaf));
        if (der && peer.sha256.size() == CC_SHA256_DIGEST_LENGTH) {
            CC_SHA256(der.bytes, CC_LONG(der.length), peer.sha256.data());
            peer.have_digest = true;
        }
        const LeafFacts facts = leaf_facts(leaf);
        peer.subject = facts.subject;
        peer.issuer = facts.issuer;
        const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        if (facts.have_dates) {
            peer.valid_from = time_text(facts.not_before);
            peer.expires = time_text(facts.not_after);
            peer.expired = now > facts.not_after;
            peer.not_yet_valid = now < facts.not_before;
            if (peer.expired)
                peer.validity_errors.push_back(format("The certificate expired on %s (errSecCertificateExpired, %d)",
                                                      peer.expires.c_str(), int(errSecCertificateExpired)));
            if (peer.not_yet_valid)
                peer.validity_errors.push_back(
                    format("The certificate is not valid before %s (errSecCertificateNotValidYet, %d)",
                           peer.valid_from.c_str(), int(errSecCertificateNotValidYet)));
        } else {
            peer.valid_from = peer.expires = "?";
        }

        // Trust: chain, anchors, host name, usage; first as of now.
        NSArray* ssl = @[ CFBridgingRelease(SecPolicyCreateSSL(true, (__bridge CFStringRef)@(host.c_str()))) ];
        NSArray* built = nil;
        CFErrorRef trust_error = evaluate(certs, ssl, anchors_, now, &built);
        if (built.count == 0) built = certs;

        // Validity: each certificate's own dates against the clock (the
        // leaf above, the rest of the built chain here), and the span in
        // which all of them are valid, if there is one.
        bool dates_bad = peer.expired || peer.not_yet_valid;
        CFAbsoluteTime common_from = facts.not_before, common_until = facts.not_after;
        for (NSUInteger i = 1; i < built.count; ++i) {
            const LeafFacts f = leaf_facts((__bridge SecCertificateRef)built[i]);
            if (!f.have_dates) continue;
            if (f.not_before > common_from) common_from = f.not_before;
            if (f.not_after < common_until) common_until = f.not_after;
            if (now > f.not_after) {
                dates_bad = true;
                peer.validity_errors.push_back(
                    format("A certificate in the chain (depth %u, %s) expired on %s (errSecCertificateExpired, %d)",
                           unsigned(i), f.subject.c_str(), time_text(f.not_after).c_str(),
                           int(errSecCertificateExpired)));
            } else if (now < f.not_before) {
                dates_bad = true;
                peer.validity_errors.push_back(format("A certificate in the chain (depth %u, %s) is not valid before "
                                                      "%s (errSecCertificateNotValidYet, %d)",
                                                      unsigned(i), f.subject.c_str(),
                                                      time_text(f.not_before).c_str(),
                                                      int(errSecCertificateNotValidYet)));
            }
        }
        // The trust answer must not depend on the dates: ask again at a
        // moment every certificate is valid (else one the leaf is), and
        // leave a date error that remains to the validity lines above.
        CFAbsoluteTime when = now;
        if (trust_error && is_date_error(CFErrorGetCode(trust_error))) {
            if (dates_bad && facts.have_dates) {
                const bool common = common_from < common_until;
                const CFAbsoluteTime from = common ? common_from : facts.not_before;
                const CFAbsoluteTime until = common ? common_until : facts.not_after;
                when = now < from ? from + 1 : now > until ? until - 1 : now;
                CFRelease(trust_error);
                trust_error = evaluate(certs, ssl, anchors_, when, nullptr);
                if (trust_error && is_date_error(CFErrorGetCode(trust_error))) {
                    CFRelease(trust_error);
                    trust_error = nullptr;
                }
            } else {
                // A date problem the dates above do not show.
                peer.validity_errors.push_back("A certificate in the chain is outside its validity period (" +
                                               cf_error_text(trust_error) + ")");
                CFRelease(trust_error);
                trust_error = nullptr;
            }
        }
        if (!trust_error) {
            peer.chain_ok = true;
        } else if (CFErrorGetCode(trust_error) == errSecCertificateRevoked) {
            // The SSL policy checked revocation itself: a validity answer
            // (only a trusted chain gets that far).
            peer.chain_ok = true;
            peer.revoked = true;
            peer.revocation_checked = true;
            peer.validity_errors.push_back("The certificate has been revoked by its issuer (" +
                                           cf_error_text(trust_error) + ")");
        } else {
            peer.chain_error = cf_error_text(trust_error);
        }
        if (trust_error) CFRelease(trust_error);
        check_revocation(certs, when, peer);

        set_peer_certificate(peer);
        return tls_accept_peer(cfg_, host, peer, [this](LogLevel l, const std::string& s) { note(l, s); }, error,
                               trust_handler(), port_);
    }

    void check_revocation(NSArray* certs, CFAbsoluteTime when, PeerCertificate& peer) {
        if (peer.revocation_checked) return;
        if (!peer.chain_ok) {
            peer.revocation_note = certs.count == 1 && peer.subject == peer.issuer
                                       ? "a self-signed certificate has no issuer to revoke it"
                                       : "the chain is not trusted, so its revocation status was not asked";
            return;
        }
        NSArray* policies = @[
            CFBridgingRelease(SecPolicyCreateBasicX509()),
            CFBridgingRelease(SecPolicyCreateRevocation(kSecRevocationUseAnyAvailableMethod |
                                                        kSecRevocationRequirePositiveResponse))
        ];
        CFErrorRef e = evaluate(certs, policies, anchors_, when, nullptr);
        if (!e) {
            peer.revocation_checked = true;
            return;
        }
        if (CFErrorGetCode(e) == errSecCertificateRevoked) {
            peer.revoked = true;
            peer.revocation_checked = true;
            peer.validity_errors.push_back("The certificate has been revoked by its issuer (" + cf_error_text(e) +
                                           ")");
        } else {
            peer.revocation_note = revocation_note_text(e);
        }
        CFRelease(e);
    }

    Tn5250Config cfg_;
    uint16_t port_ = 0;
    nw_connection_t conn_ = nil;
    dispatch_queue_t queue_ = nil;
    std::shared_ptr<Link> link_;
    NSMutableArray* anchors_ = nil;
    bool failed_ = false;
    std::string peer_;
    std::string summary_;
};

} // namespace

const char* tls_backend_name() { return "Network.framework"; }

std::unique_ptr<Transport> make_tls_transport(const Tn5250Config& cfg, std::string& error) {
    const std::string invalid = cfg.validate();
    if (!invalid.empty()) {
        error = invalid;
        return nullptr;
    }
    return std::unique_ptr<Transport>(new AppleTransport(cfg));
}

} // namespace tn5250
} // namespace em34xx
