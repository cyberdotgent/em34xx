// TLS transport, Windows backend: SChannel through SSPI, certificates
// through CryptoAPI. Nothing but system DLLs (secur32, crypt32), so the
// Windows certificate store, including certificates an administrator has
// deployed, is what "system" verification trusts. Works on Windows 7 SP1.
//
// SChannel is asked for manual validation (SCH_CRED_MANUAL_CRED_VALIDATION):
// the handshake completes whatever the certificate, then the leaf is taken
// from the context and checked with CertGetCertificateChain and
// CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL) for the server
// name, and tls_accept_peer() decides (see tls_policy.hpp). That keeps the
// insecure and pinned modes possible. ca_file adds PEM certificates as
// extra trust anchors through a private chain engine.
//
// Protocols come from SCHANNEL_CRED::grbitEnabledProtocols (SSL 3.0 to
// TLS 1.2). TLS 1.3 would need the newer SCH_CREDENTIALS API, which
// Windows 7 lacks, so it is not offered. Windows' own policy (SCHANNEL
// registry keys, Group Policy) can still disable a protocol system-wide;
// Windows 7 needs KB3140245-era updates for TLS 1.1/1.2 to be usable by
// some applications, but SChannel itself offers TLS 1.2 when asked.
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#include "tn5250/tls_policy.hpp"

#include "net/socket.hpp"
#include "util/strings.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace tn34xx {
namespace tn5250 {

namespace {

constexpr int kHandshakeTimeoutMs = 15000;

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

std::string status_text(long code) {
    char* msg = nullptr;
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, DWORD(code), 0, reinterpret_cast<char*>(&msg), 0, nullptr);
    std::string text = n && msg ? std::string(msg, n) : std::string();
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.'))
        text.pop_back();
    return format("%s%s0x%08lX", text.c_str(), text.empty() ? "" : " ", static_cast<unsigned long>(code));
}

// Chain policy errors, named even where FormatMessage has no text.
std::string chain_error_text(DWORD code) {
    const char* what = nullptr;
    switch (code) {
    case DWORD(CERT_E_UNTRUSTEDROOT): what = "the root certificate is not trusted"; break;
    case DWORD(CERT_E_CHAINING): what = "no chain to a trusted root"; break;
    case DWORD(CERT_E_CN_NO_MATCH): what = "the certificate is not for this host name"; break;
    case DWORD(CERT_E_EXPIRED): what = "the certificate has expired or is not yet valid"; break;
    case DWORD(CERT_E_WRONG_USAGE): what = "the certificate is not for server authentication"; break;
    case DWORD(CERT_E_REVOKED): what = "the certificate is revoked"; break;
    case DWORD(TRUST_E_CERT_SIGNATURE): what = "a signature in the chain does not verify"; break;
    default: break;
    }
    return what ? format("%s (0x%08lX)", what, static_cast<unsigned long>(code)) : status_text(long(code));
}

std::string cert_name(const CERT_NAME_BLOB* blob) {
    char buf[1024];
    const DWORD n = CertNameToStrA(X509_ASN_ENCODING, const_cast<CERT_NAME_BLOB*>(blob),
                                   CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG, buf, sizeof buf);
    return n > 1 ? std::string(buf) : std::string("?");
}

std::string filetime_text(const FILETIME& ft) {
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st)) return "?";
    return format("%04u-%02u-%02u %02u:%02u:%02u UTC", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
}

bool sha256_of(const BYTE* data, DWORD len, Sha256Digest& out) {
    HCRYPTPROV prov = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
    bool ok = false;
    HCRYPTHASH hash = 0;
    if (CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        DWORD size = DWORD(out.size());
        ok = CryptHashData(hash, data, len, 0) && CryptGetHashParam(hash, HP_HASHVAL, out.data(), &size, 0) &&
             size == out.size();
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    return ok;
}

// PEM certificates of a ca_file, as DER blobs.
bool read_pem_certificates(const std::string& path, std::vector<std::vector<BYTE>>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "TLS: cannot open ca_file " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    static const char kBegin[] = "-----BEGIN CERTIFICATE-----";
    static const char kEnd[] = "-----END CERTIFICATE-----";
    size_t pos = 0;
    while ((pos = text.find(kBegin, pos)) != std::string::npos) {
        const size_t end = text.find(kEnd, pos);
        if (end == std::string::npos) break;
        const std::string pem = text.substr(pos, end + sizeof kEnd - 1 - pos);
        DWORD len = 0;
        if (CryptStringToBinaryA(pem.c_str(), DWORD(pem.size()), CRYPT_STRING_BASE64HEADER, nullptr, &len, nullptr,
                                 nullptr) &&
            len) {
            std::vector<BYTE> der(len);
            if (CryptStringToBinaryA(pem.c_str(), DWORD(pem.size()), CRYPT_STRING_BASE64HEADER, der.data(), &len,
                                     nullptr, nullptr)) {
                der.resize(len);
                out.push_back(std::move(der));
            }
        }
        pos = end + sizeof kEnd - 1;
    }
    if (out.empty()) {
        error = "TLS: no PEM certificates in ca_file " + path;
        return false;
    }
    return true;
}

DWORD protocol_bits(TlsVersion min, TlsVersion max) {
    struct Entry { TlsVersion v; DWORD bit; };
    static const Entry kTable[] = {
        {TlsVersion::Ssl3, SP_PROT_SSL3_CLIENT},
        {TlsVersion::Tls1_0, SP_PROT_TLS1_0_CLIENT},
        {TlsVersion::Tls1_1, SP_PROT_TLS1_1_CLIENT},
        {TlsVersion::Tls1_2, SP_PROT_TLS1_2_CLIENT},
    };
    DWORD bits = 0;
    for (const Entry& e : kTable)
        if (e.v >= min && (max == TlsVersion::Any || e.v <= max)) bits |= e.bit;
    return bits;
}

const char* protocol_name(DWORD proto) {
    if (proto & SP_PROT_TLS1_2_CLIENT) return "TLSv1.2";
    if (proto & SP_PROT_TLS1_1_CLIENT) return "TLSv1.1";
    if (proto & SP_PROT_TLS1_0_CLIENT) return "TLSv1";
    if (proto & SP_PROT_SSL3_CLIENT) return "SSLv3";
    return "TLS (unknown version)";
}

std::string cipher_text(const SecPkgContext_ConnectionInfo& ci) {
    const char* cipher = "?";
    switch (ci.aiCipher) {
    case CALG_AES_128: cipher = "AES-128"; break;
    case CALG_AES_256: cipher = "AES-256"; break;
    case CALG_3DES: cipher = "3DES"; break;
    case CALG_RC4: cipher = "RC4"; break;
    default: break;
    }
    const char* hash = "?";
    switch (ci.aiHash) {
    case CALG_SHA1: hash = "SHA1"; break;
    case CALG_SHA_256: hash = "SHA256"; break;
    case CALG_SHA_384: hash = "SHA384"; break;
    case CALG_MD5: hash = "MD5"; break;
    default: break;
    }
    const char* kx = "?";
    switch (ci.aiExch) {
    case CALG_RSA_KEYX: kx = "RSA"; break;
    case CALG_DH_EPHEM: kx = "DHE"; break;
#ifdef CALG_ECDH_EPHEM
    case CALG_ECDH_EPHEM: kx = "ECDHE"; break;
#endif
#ifdef CALG_ECDH
    case CALG_ECDH: kx = "ECDH"; break;
#endif
    default: break;
    }
    std::string out = format("%s %s %s", kx, cipher, hash);
    if (ci.dwExchStrength) out += format(" (%lu-bit key exchange)", static_cast<unsigned long>(ci.dwExchStrength));
    return out;
}

class SchannelTransport : public Transport {
public:
    explicit SchannelTransport(const Tn5250Config& cfg) : cfg_(cfg) {}
    ~SchannelTransport() override { close(); }

    bool connect(const std::string& host, uint16_t port, std::string& error) override {
        close();
        if (cfg_.tls_min_version == TlsVersion::Tls1_3) {
            error = "TLS: tls_min_version 1.3 is not available with Windows SChannel in this build (TLS 1.2 is the newest it offers)";
            return false;
        }
        const DWORD protocols = protocol_bits(cfg_.tls_min_version, cfg_.tls_max_version);
        if (!protocols) {
            error = "TLS: no protocol version left between tls_min_version and tls_max_version";
            return false;
        }
        tls_note_legacy(cfg_, [this](LogLevel l, const std::string& s) { note(l, s); });
        if (!cfg_.ca_file.empty() && !read_pem_certificates(cfg_.ca_file, extra_roots_, error)) return false;

        SCHANNEL_CRED sc;
        std::memset(&sc, 0, sizeof sc);
        sc.dwVersion = SCHANNEL_CRED_VERSION;
        sc.grbitEnabledProtocols = protocols;
        sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
        TimeStamp expiry;
        SECURITY_STATUS st = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
                                                       SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr, nullptr,
                                                       &cred_, &expiry);
        if (st != SEC_E_OK) {
            error = "TLS: AcquireCredentialsHandle failed: " + status_text(st);
            return false;
        }
        have_cred_ = true;

        if (!sock_.connect(host, port, error)) {
            close();
            return false;
        }
        whost_ = widen(host);
        port_ = port;
        if (!handshake(error)) {
            error = format("TLS handshake with %s:%u failed: %s", host.c_str(), port, error.c_str());
            if (port == 23) error += " (port 23 is plain telnet; TLS servers usually listen on 992)";
            else if (cfg_.tls_min_version >= TlsVersion::Tls1_2)
                error += " (an old host may need a lower tls_min_version)";
            close();
            return false;
        }
        st = QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_);
        if (st != SEC_E_OK) {
            error = "TLS: cannot query the stream sizes: " + status_text(st);
            close();
            return false;
        }
        SecPkgContext_ConnectionInfo ci;
        std::memset(&ci, 0, sizeof ci);
        if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_CONNECTION_INFO, &ci) == SEC_E_OK)
            summary_ = format("%s %s", protocol_name(ci.dwProtocol), cipher_text(ci).c_str());
        else
            summary_ = "TLS";
        note(LogLevel::Info, "TLS negotiated " + summary_);
        if (!check_peer(host, error)) {
            close();
            return false;
        }
        peer_ = sock_.peer_name();
        return true;
    }

    int recv(uint8_t* buf, size_t len, int timeout_ms) override {
        if (!have_ctx_ || failed_) return -1;
        for (;;) {
            if (!plain_.empty()) {
                const size_t n = len < plain_.size() ? len : plain_.size();
                std::memcpy(buf, plain_.data(), n);
                plain_.erase(plain_.begin(), plain_.begin() + std::ptrdiff_t(n));
                return int(n);
            }
            if (!enc_.empty()) {
                const int r = decrypt();
                if (r < 0) return -1;
                if (r > 0) continue;   // a record was consumed (possibly empty)
            }
            uint8_t tmp[16384];
            const int n = sock_.recv(tmp, sizeof tmp, timeout_ms);
            if (n == 0) return 0;
            if (n < 0) {
                failed_ = true;
                return -1;
            }
            enc_.insert(enc_.end(), tmp, tmp + n);
        }
    }

    bool send(const uint8_t* buf, size_t len) override {
        if (!have_ctx_ || failed_) return false;
        std::vector<uint8_t> msg(size_t(sizes_.cbHeader) + sizes_.cbMaximumMessage + sizes_.cbTrailer);
        while (len > 0) {
            const size_t chunk = len < sizes_.cbMaximumMessage ? len : sizes_.cbMaximumMessage;
            std::memcpy(msg.data() + sizes_.cbHeader, buf, chunk);
            SecBuffer b[4];
            b[0].BufferType = SECBUFFER_STREAM_HEADER;
            b[0].pvBuffer = msg.data();
            b[0].cbBuffer = sizes_.cbHeader;
            b[1].BufferType = SECBUFFER_DATA;
            b[1].pvBuffer = msg.data() + sizes_.cbHeader;
            b[1].cbBuffer = ULONG(chunk);
            b[2].BufferType = SECBUFFER_STREAM_TRAILER;
            b[2].pvBuffer = msg.data() + sizes_.cbHeader + chunk;
            b[2].cbBuffer = sizes_.cbTrailer;
            b[3].BufferType = SECBUFFER_EMPTY;
            b[3].pvBuffer = nullptr;
            b[3].cbBuffer = 0;
            SecBufferDesc desc{SECBUFFER_VERSION, 4, b};
            const SECURITY_STATUS st = EncryptMessage(&ctx_, 0, &desc, 0);
            if (st != SEC_E_OK) {
                note(LogLevel::Debug, "TLS EncryptMessage: " + status_text(st));
                failed_ = true;
                return false;
            }
            if (!sock_.send_all(msg.data(), size_t(b[0].cbBuffer) + b[1].cbBuffer + b[2].cbBuffer)) {
                failed_ = true;
                return false;
            }
            buf += chunk;
            len -= chunk;
        }
        return true;
    }

    void close() override {
        if (have_ctx_) {
            DeleteSecurityContext(&ctx_);
            have_ctx_ = false;
        }
        if (have_cred_) {
            FreeCredentialsHandle(&cred_);
            have_cred_ = false;
        }
        sock_.close();
        enc_.clear();
        plain_.clear();
        extra_roots_.clear();
        failed_ = false;
    }

    std::string describe() const override { return "tls " + peer_ + " " + summary_; }

private:
    static constexpr ULONG kFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                                    ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR |
                                    ISC_REQ_MANUAL_CRED_VALIDATION;

    bool send_token(SecBuffer& out) {
        bool ok = true;
        if (out.pvBuffer && out.cbBuffer) ok = sock_.send_all(out.pvBuffer, out.cbBuffer);
        if (out.pvBuffer) FreeContextBuffer(out.pvBuffer);
        out.pvBuffer = nullptr;
        out.cbBuffer = 0;
        return ok;
    }

    bool handshake(std::string& error) {
        SecBuffer out{0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc out_desc{SECBUFFER_VERSION, 1, &out};
        ULONG attrs = 0;
        SECURITY_STATUS st = InitializeSecurityContextW(&cred_, nullptr, &whost_[0], kFlags, 0, 0, nullptr, 0,
                                                        &ctx_, &out_desc, &attrs, nullptr);
        if (st != SEC_I_CONTINUE_NEEDED) {
            error = "InitializeSecurityContext: " + status_text(st);
            return false;
        }
        have_ctx_ = true;
        if (!send_token(out)) {
            error = "send failed";
            return false;
        }
        bool need_data = true;
        for (;;) {
            if (need_data) {
                uint8_t tmp[16384];
                const int n = sock_.recv(tmp, sizeof tmp, kHandshakeTimeoutMs);
                if (n == 0) {
                    error = format("no answer within %d s", kHandshakeTimeoutMs / 1000);
                    return false;
                }
                if (n < 0) {
                    error = "the server closed the connection";
                    return false;
                }
                enc_.insert(enc_.end(), tmp, tmp + n);
            }
            SecBuffer in[2];
            in[0].BufferType = SECBUFFER_TOKEN;
            in[0].pvBuffer = enc_.data();
            in[0].cbBuffer = ULONG(enc_.size());
            in[1].BufferType = SECBUFFER_EMPTY;
            in[1].pvBuffer = nullptr;
            in[1].cbBuffer = 0;
            SecBufferDesc in_desc{SECBUFFER_VERSION, 2, in};
            out = SecBuffer{0, SECBUFFER_TOKEN, nullptr};
            st = InitializeSecurityContextW(&cred_, &ctx_, &whost_[0], kFlags, 0, 0, &in_desc, 0, nullptr,
                                            &out_desc, &attrs, nullptr);
            if (st == SEC_E_INCOMPLETE_MESSAGE) {
                need_data = true;
                continue;
            }
            const bool token_ok = send_token(out);
            if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || st == SEC_I_INCOMPLETE_CREDENTIALS) {
                if (!token_ok) {
                    error = "send failed";
                    return false;
                }
                if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer > 0) {
                    enc_.erase(enc_.begin(), enc_.end() - std::ptrdiff_t(in[1].cbBuffer));
                    need_data = false;
                } else {
                    enc_.clear();
                    need_data = true;
                }
                if (st == SEC_E_OK) return true;   // leftover enc_ is application data
                continue;
            }
            error = status_text(st);
            return false;
        }
    }

    // Returns 1 when a record was consumed, 0 when more data is needed, -1
    // on error or close.
    int decrypt() {
        SecBuffer b[4];
        b[0].BufferType = SECBUFFER_DATA;
        b[0].pvBuffer = enc_.data();
        b[0].cbBuffer = ULONG(enc_.size());
        for (int i = 1; i < 4; ++i) {
            b[i].BufferType = SECBUFFER_EMPTY;
            b[i].pvBuffer = nullptr;
            b[i].cbBuffer = 0;
        }
        SecBufferDesc desc{SECBUFFER_VERSION, 4, b};
        const SECURITY_STATUS st = DecryptMessage(&ctx_, &desc, 0, nullptr);
        if (st == SEC_E_INCOMPLETE_MESSAGE) return 0;
        if (st == SEC_I_CONTEXT_EXPIRED) {   // close_notify
            failed_ = true;
            return -1;
        }
        if (st != SEC_E_OK) {
            note(LogLevel::Debug, st == SEC_I_RENEGOTIATE ? std::string("TLS: the server asked to renegotiate; not supported")
                                                          : "TLS DecryptMessage: " + status_text(st));
            failed_ = true;
            return -1;
        }
        size_t extra = 0;
        for (const SecBuffer& x : b) {
            if (x.BufferType == SECBUFFER_DATA && x.pvBuffer && x.cbBuffer) {
                const uint8_t* p = static_cast<const uint8_t*>(x.pvBuffer);
                plain_.insert(plain_.end(), p, p + x.cbBuffer);
            } else if (x.BufferType == SECBUFFER_EXTRA) {
                extra = x.cbBuffer;
            }
        }
        if (extra) enc_.erase(enc_.begin(), enc_.end() - std::ptrdiff_t(extra));
        else enc_.clear();
        return 1;
    }

    // Chain + SSL policy for the server name, against the system store,
    // plus ca_file roots through a private chain engine when given.
    void verify_chain(PCCERT_CONTEXT cert, PeerCertificate& peer) {
        HCERTSTORE extra = nullptr;
        HCERTCHAINENGINE engine = nullptr;
        if (!extra_roots_.empty()) {
            extra = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
            for (const auto& der : extra_roots_)
                if (extra) CertAddEncodedCertificateToStore(extra, X509_ASN_ENCODING, der.data(), DWORD(der.size()),
                                                            CERT_STORE_ADD_USE_EXISTING, nullptr);
            CERT_CHAIN_ENGINE_CONFIG ec;
            std::memset(&ec, 0, sizeof ec);
            ec.cbSize = sizeof ec;   // the Windows 7 layout: _WIN32_WINNT is 0x0601
            ec.cAdditionalStore = extra ? 1 : 0;
            ec.rghAdditionalStore = extra ? &extra : nullptr;
            // Extra roots are trusted only through hExclusiveRoot (Win8+);
            // on Windows 7, build the chain with them as additional
            // certificates and accept CERT_E_UNTRUSTEDROOT when the root
            // that ends the chain is one of them.
            if (!CertCreateCertificateChainEngine(&ec, &engine)) engine = nullptr;
        }

        CERT_CHAIN_PARA cp;
        std::memset(&cp, 0, sizeof cp);
        cp.cbSize = sizeof cp;
        static LPSTR kUsages[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH),
                                  const_cast<LPSTR>(szOID_SERVER_GATED_CRYPTO),
                                  const_cast<LPSTR>(szOID_SGC_NETSCAPE)};
        cp.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
        cp.RequestedUsage.Usage.cUsageIdentifier = 3;
        cp.RequestedUsage.Usage.rgpszUsageIdentifier = kUsages;
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        if (!CertGetCertificateChain(engine, cert, nullptr, cert->hCertStore, &cp, 0, nullptr, &chain)) {
            peer.chain_error = "CertGetCertificateChain: " + status_text(long(GetLastError()));
        } else {
            HTTPSPolicyCallbackData https;
            std::memset(&https, 0, sizeof https);
            https.cbStruct = sizeof https;
            https.dwAuthType = AUTHTYPE_SERVER;
            https.pwszServerName = &whost_[0];
            CERT_CHAIN_POLICY_PARA pp;
            std::memset(&pp, 0, sizeof pp);
            pp.cbSize = sizeof pp;
            pp.pvExtraPolicyPara = &https;
            CERT_CHAIN_POLICY_STATUS ps;
            std::memset(&ps, 0, sizeof ps);
            ps.cbSize = sizeof ps;
            if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps)) {
                peer.chain_error = "CertVerifyCertificateChainPolicy: " + status_text(long(GetLastError()));
            } else if (ps.dwError == 0) {
                peer.chain_ok = true;
            } else if (DWORD(ps.dwError) == DWORD(CERT_E_UNTRUSTEDROOT) && root_is_extra(chain)) {
                // Untrusted only because the root is a ca_file one: check the
                // rest of the policy (names, dates) with that error ignored.
                https.fdwChecks = 0x00000100;   // SECURITY_FLAG_IGNORE_UNKNOWN_CA
                std::memset(&ps, 0, sizeof ps);
                ps.cbSize = sizeof ps;
                if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps) && ps.dwError == 0)
                    peer.chain_ok = true;
                else
                    peer.chain_error = chain_error_text(ps.dwError);
            } else {
                peer.chain_error = chain_error_text(ps.dwError);
            }
            CertFreeCertificateChain(chain);
        }
        if (engine) CertFreeCertificateChainEngine(engine);
        if (extra) CertCloseStore(extra, 0);
    }

    bool root_is_extra(PCCERT_CHAIN_CONTEXT chain) const {
        if (!chain || chain->cChain == 0) return false;
        const CERT_SIMPLE_CHAIN* simple = chain->rgpChain[0];
        if (!simple || simple->cElement == 0) return false;
        PCCERT_CONTEXT root = simple->rgpElement[simple->cElement - 1]->pCertContext;
        for (const auto& der : extra_roots_)
            if (der.size() == root->cbCertEncoded && std::memcmp(der.data(), root->pbCertEncoded, der.size()) == 0)
                return true;
        return false;
    }

    bool check_peer(const std::string& host, std::string& error) {
        PCCERT_CONTEXT cert = nullptr;
        const SECURITY_STATUS st = QueryContextAttributesW(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert);
        if (st != SEC_E_OK || !cert) {
            error = "TLS: the server sent no certificate";
            return false;
        }
        PeerCertificate peer;
        peer.have_digest = sha256_of(cert->pbCertEncoded, cert->cbCertEncoded, peer.sha256);
        peer.subject = cert_name(&cert->pCertInfo->Subject);
        peer.issuer = cert_name(&cert->pCertInfo->Issuer);
        peer.valid_from = filetime_text(cert->pCertInfo->NotBefore);
        peer.expires = filetime_text(cert->pCertInfo->NotAfter);
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        peer.expired = CompareFileTime(&cert->pCertInfo->NotAfter, &now) < 0;
        verify_chain(cert, peer);
        CertFreeCertificateContext(cert);
        set_peer_certificate(peer);
        return tls_accept_peer(cfg_, host, peer, [this](LogLevel l, const std::string& s) { note(l, s); }, error,
                               trust_handler(), port_);
    }

    Tn5250Config cfg_;
    TcpSocket sock_;
    CredHandle cred_{};
    CtxtHandle ctx_{};
    bool have_cred_ = false;
    bool have_ctx_ = false;
    bool failed_ = false;
    std::wstring whost_;
    uint16_t port_ = 0;
    SecPkgContext_StreamSizes sizes_{};
    std::vector<uint8_t> enc_;     // received, not yet decrypted
    std::vector<uint8_t> plain_;   // decrypted, not yet returned
    std::vector<std::vector<BYTE>> extra_roots_;
    std::string peer_;
    std::string summary_;
};

} // namespace

const char* tls_backend_name() { return "schannel"; }

std::unique_ptr<Transport> make_tls_transport(const Tn5250Config& cfg, std::string& error) {
    const std::string invalid = cfg.validate();
    if (!invalid.empty()) {
        error = invalid;
        return nullptr;
    }
    return std::unique_ptr<Transport>(new SchannelTransport(cfg));
}

} // namespace tn5250
} // namespace tn34xx
