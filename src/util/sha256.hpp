// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace em34xx {

// SHA-256 of a buffer, as a lowercase hex string.
std::string sha256_hex(const uint8_t* data, size_t len);

} // namespace em34xx
