// SPDX-License-Identifier: GPL-3.0-or-later
// Certificate fingerprints as users write them in configuration files.
//
// The canonical form is upper-case hex pairs joined by colons
// ("AB:CD:..."), which is what `openssl x509 -fingerprint -sha256` prints.
// The parser also accepts lower case, no separators, and spaces or dashes
// between pairs, with an optional "SHA256:" / "sha256=" prefix.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tn34xx {

using Sha256Digest = std::array<uint8_t, 32>;

// "AB:CD:...:EF" (95 characters).
std::string format_sha256_fingerprint(const Sha256Digest& digest);

// False unless `text` holds exactly 32 bytes of hex in one of the accepted
// forms.
bool parse_sha256_fingerprint(const std::string& text, Sha256Digest& out);

} // namespace tn34xx
