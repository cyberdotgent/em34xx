// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace em34xx {

std::string trim(const std::string& s);
std::string to_lower(std::string s);
std::string to_upper(std::string s);
std::vector<std::string> split(const std::string& s, char sep);
// Split on whitespace, honouring "double quoted" words.
std::vector<std::string> split_words(const std::string& s);
bool starts_with(const std::string& s, const std::string& prefix);

// Parse an unsigned integer. Accepts 0x / h suffix for hex and a leading '#'
// for decimal; otherwise uses default_base.
bool parse_uint(const std::string& text, uint64_t& out, int default_base = 10);

// On MinGW the plain "printf" archetype means msvcrt's printf, which lacks
// %zu and %llu; format() uses MinGW's C99 vsnprintf (__USE_MINGW_ANSI_STDIO,
// set by the build), so check against the GNU rules instead.
std::string format(const char* fmt, ...)
#if defined(__MINGW32__)
    __attribute__((format(gnu_printf, 1, 2)))
#elif defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

std::string hex_bytes(const uint8_t* data, size_t len);

} // namespace em34xx
