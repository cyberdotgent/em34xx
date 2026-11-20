#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tn34xx {

// SHA-256 of a buffer, as a lowercase hex string.
std::string sha256_hex(const uint8_t* data, size_t len);

} // namespace tn34xx
