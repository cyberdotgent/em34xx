// Byte transport under the TN5250 telnet layer.
//
// Only plain TCP exists today. TLS (port 992) is meant to slot in as a
// second Transport. Candidates, both available in vcpkg and buildable for
// Windows 7 x86:
//   - OpenSSL 3.x: Apache License 2.0. Compatible with our code, but the
//     Apache 2.0 patent/NOTICE terms then apply to binary distributions,
//     and it is incompatible with GPLv2-only code (none here today).
//   - Mbed TLS 3.x: Apache 2.0 or GPL-2.0-or-later, at our choice; much
//     smaller, but needs its own CA bundle handling on Windows.
// Whichever is chosen, Tn5250Config::{tls, tls_verify, ca_file} are the
// knobs, and make_transport() is the only place that changes.
#pragma once

#include "host/host_config.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace tn34xx {
namespace tn5250 {

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool connect(const std::string& host, uint16_t port, std::string& error) = 0;
    // Wait up to timeout_ms. Returns bytes read, 0 on timeout, -1 on
    // error or orderly close.
    virtual int recv(uint8_t* buf, size_t len, int timeout_ms) = 0;
    virtual bool send(const uint8_t* buf, size_t len) = 0;
    virtual void close() = 0;
    virtual std::string describe() const = 0;
};

// The transport the configuration asks for, or nullptr with `error` set
// (for example when TLS is requested; it is not built in yet).
std::unique_ptr<Transport> make_transport(const Tn5250Config& cfg, std::string& error);

} // namespace tn5250
} // namespace tn34xx
