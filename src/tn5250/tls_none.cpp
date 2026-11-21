// SPDX-License-Identifier: GPL-3.0-or-later
// Built when no TLS stack was found at configure time (EM34XX_TLS=AUTO
// without the OpenSSL development files, or EM34XX_TLS=OFF).
#include "tn5250/tls_policy.hpp"

namespace em34xx {
namespace tn5250 {

const char* tls_backend_name() { return nullptr; }

std::unique_ptr<Transport> make_tls_transport(const Tn5250Config&, std::string& error) {
    error = "this build has no TLS support; install the OpenSSL development files (libssl-dev / "
            "openssl-devel) and rebuild, or connect without tls (port 23)";
    return nullptr;
}

} // namespace tn5250
} // namespace em34xx
