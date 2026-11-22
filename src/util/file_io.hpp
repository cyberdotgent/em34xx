// SPDX-License-Identifier: GPL-3.0-or-later
// Small files that must survive a crash or a full disk: battery RAM and
// EEPROM images, imported ROMs. Paths are UTF-8, also on Windows.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace em34xx {

// The native path for a UTF-8 string. Never throws: on Windows a string
// that is not valid UTF-8 is taken to be in the ANSI code page (a console
// argument).
std::filesystem::path path_from_utf8(const std::string& utf8);

// The size of a regular file; false if there is none or it cannot be asked.
bool file_size(const std::string& path, uint64_t& size);

// Move `from` over `to` in one step, replacing `to` if it exists: at any
// moment `to` is either the old file or the new one. On failure `from` is
// left where it was.
bool replace_file(const std::string& from, const std::string& to, std::string& error);

// Write `path` through a temporary file (path + ".tmp") that replaces it
// only once it is completely written. On failure the previous `path` is
// untouched and the temporary file is removed.
bool write_file_atomic(const std::string& path, const uint8_t* data, size_t size, std::string& error);

} // namespace em34xx
