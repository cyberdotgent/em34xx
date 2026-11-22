// SPDX-License-Identifier: GPL-3.0-or-later
#include "util/file_io.hpp"

#include "util/strings.hpp"

#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace em34xx {

namespace fs = std::filesystem;

fs::path path_from_utf8(const std::string& utf8) {
#ifdef _WIN32
    // Not fs::u8path: it throws on a string that is not UTF-8.
    for (int pass = 0; pass < 2 && !utf8.empty(); ++pass) {
        const UINT cp = pass == 0 ? CP_UTF8 : CP_ACP;
        const DWORD flags = pass == 0 ? MB_ERR_INVALID_CHARS : 0;
        const int n = MultiByteToWideChar(cp, flags, utf8.data(), int(utf8.size()), nullptr, 0);
        if (n <= 0) continue;
        std::wstring w(size_t(n), L'\0');
        if (MultiByteToWideChar(cp, flags, utf8.data(), int(utf8.size()), &w[0], n) != n) continue;
        return fs::path(w);
    }
    return fs::path();
#else
    return fs::path(utf8);
#endif
}

bool file_size(const std::string& path, uint64_t& size) {
    const fs::path p = path_from_utf8(path);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return false;
    const auto n = fs::file_size(p, ec);
    if (ec) return false;
    size = uint64_t(n);
    return true;
}

bool replace_file(const std::string& from, const std::string& to, std::string& error) {
    const fs::path src = path_from_utf8(from), dst = path_from_utf8(to);
#ifdef _WIN32
    // rename() does not replace on Windows; MoveFileEx does, in one step.
    if (!MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = format("cannot replace %s with %s (Windows error %lu)", to.c_str(), from.c_str(),
                       static_cast<unsigned long>(GetLastError()));
        return false;
    }
#else
    std::error_code ec;
    fs::rename(src, dst, ec);   // rename(2) replaces
    if (ec) {
        error = "cannot replace " + to + " with " + from + ": " + ec.message();
        return false;
    }
#endif
    return true;
}

bool write_file_atomic(const std::string& path, const uint8_t* data, size_t size, std::string& error) {
    const std::string tmp = path + ".tmp";
    const fs::path tmp_path = path_from_utf8(tmp);
    bool ok;
    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        ok = f.is_open();
        if (ok && size) f.write(reinterpret_cast<const char*>(data), std::streamsize(size));
        // A small image sits in the stream's buffer until here: a full disk
        // shows only at flush or close.
        if (ok) {
            f.flush();
            f.close();
            ok = !f.fail();
        }
    }
    if (!ok) error = "cannot write " + tmp;
    else if (replace_file(tmp, path, error)) return true;
    std::error_code ec;
    fs::remove(tmp_path, ec);
    return false;
}

} // namespace em34xx
