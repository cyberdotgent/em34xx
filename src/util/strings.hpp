#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tn34xx {

std::string trim(const std::string& s);
std::string to_lower(std::string s);
std::vector<std::string> split(const std::string& s, char sep);
// Split on whitespace, honouring "double quoted" words.
std::vector<std::string> split_words(const std::string& s);
bool starts_with(const std::string& s, const std::string& prefix);

// Parse an unsigned integer. Accepts 0x / h suffix for hex and a leading '#'
// for decimal; otherwise uses default_base.
bool parse_uint(const std::string& text, uint64_t& out, int default_base = 10);

std::string format(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

std::string hex_bytes(const uint8_t* data, size_t len);

} // namespace tn34xx
