// SPDX-License-Identifier: GPL-3.0-or-later
#include "rom/romset.hpp"

#include "util/file_io.hpp"
#include "util/sha256.hpp"
#include "util/strings.hpp"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace em34xx {

namespace {
// fopen with a UTF-8 path, also on Windows (where fopen takes the ANSI
// code page).
FILE* open_utf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    auto widen = [](const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(size_t(n > 0 ? n : 1), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    };
    FILE* f = _wfopen(widen(path).c_str(), widen(mode).c_str());
    return f ? f : std::fopen(path.c_str(), mode);
#else
    return std::fopen(path.c_str(), mode);
#endif
}
} // namespace

const char* rom_verdict_name(LoadedRom::Verdict v) {
    switch (v) {
    case LoadedRom::Verdict::Good: return "good";
    case LoadedRom::Verdict::UnknownHash: return "unknown";
    case LoadedRom::Verdict::KnownBad: return "KNOWN BAD";
    }
    return "?";
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = open_utf8(path, "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

bool load_roms(const std::string& dir, const std::vector<RomSpec>& specs,
               const std::vector<KnownBadRom>& known_bad, bool allow_bad,
               std::vector<LoadedRom>& out, std::vector<std::string>& warnings, std::string& error) {
    out.clear();
    for (const RomSpec& spec : specs) {
        LoadedRom rom;
        rom.spec = spec;
        const std::string path = dir.empty() ? spec.file : dir + "/" + spec.file;
        // The size first: a huge file named like a ROM is never read.
        uint64_t on_disk = 0;
        if (file_size(path, on_disk) && on_disk != spec.size) {
            error = format("ROM '%s' is %llu bytes, expected %u", path.c_str(),
                           static_cast<unsigned long long>(on_disk), spec.size);
            return false;
        }
        if (!read_file(path, rom.data)) {
            error = format("cannot read ROM '%s' (%s)", path.c_str(), spec.label.c_str());
            return false;
        }
        if (rom.data.size() != spec.size) {
            error = format("ROM '%s' is %zu bytes, expected %u", path.c_str(), rom.data.size(), spec.size);
            return false;
        }
        rom.sha256 = sha256_hex(rom.data.data(), rom.data.size());
        rom.verdict = LoadedRom::Verdict::UnknownHash;
        if (!spec.sha256.empty() && rom.sha256 == spec.sha256) rom.verdict = LoadedRom::Verdict::Good;
        for (const KnownBadRom& bad : known_bad) {
            if (bad.sha256 == rom.sha256) {
                rom.verdict = LoadedRom::Verdict::KnownBad;
                rom.note = bad.reason;
            }
        }
        if (rom.verdict == LoadedRom::Verdict::KnownBad) {
            if (!allow_bad) {
                error = format("ROM '%s' is a known-bad dump: %s (use --allow-bad-rom to load it anyway)",
                               path.c_str(), rom.note.c_str());
                return false;
            }
            warnings.push_back(format("loading known-bad ROM '%s': %s", path.c_str(), rom.note.c_str()));
        } else if (rom.verdict == LoadedRom::Verdict::UnknownHash) {
            warnings.push_back(format("ROM '%s' has an unrecognised hash %s", path.c_str(), rom.sha256.c_str()));
        }
        out.push_back(std::move(rom));
    }
    return true;
}

const char* rom_file_state_name(RomFileStatus::State s) {
    switch (s) {
    case RomFileStatus::State::Missing: return "missing";
    case RomFileStatus::State::Unreadable: return "unreadable";
    case RomFileStatus::State::WrongSize: return "wrong size";
    case RomFileStatus::State::KnownBad: return "bad dump";
    case RomFileStatus::State::UnknownHash: return "unknown hash";
    case RomFileStatus::State::Good: return "present";
    }
    return "?";
}

RomFileStatus check_rom_file(const std::string& dir, const RomSpec& spec, const std::vector<KnownBadRom>& known_bad) {
    RomFileStatus st;
    const std::string path = dir.empty() ? spec.file : dir + "/" + spec.file;
    FILE* f = open_utf8(path, "rb");
    if (!f) return st;   // Missing
    std::fclose(f);
    // The size first: a huge file named like a ROM is never read (and a
    // file of the wrong size is not hashed).
    uint64_t on_disk = 0;
    if (file_size(path, on_disk) && on_disk != spec.size) {
        st.state = RomFileStatus::State::WrongSize;
        st.note = format("%llu bytes, expected %u", static_cast<unsigned long long>(on_disk), spec.size);
        return st;
    }
    std::vector<uint8_t> data;
    if (!read_file(path, data)) {
        st.state = RomFileStatus::State::Unreadable;
        return st;
    }
    st.sha256 = sha256_hex(data.data(), data.size());
    if (data.size() != spec.size) {
        st.state = RomFileStatus::State::WrongSize;
        st.note = format("%zu bytes, expected %u", data.size(), spec.size);
        return st;
    }
    st.state = !spec.sha256.empty() && st.sha256 == spec.sha256 ? RomFileStatus::State::Good
                                                                 : RomFileStatus::State::UnknownHash;
    for (const KnownBadRom& bad : known_bad)
        if (bad.sha256 == st.sha256 && (bad.file.empty() || bad.file == spec.file)) {
            st.state = RomFileStatus::State::KnownBad;
            st.note = bad.reason;
        }
    return st;
}

const RomSpec* rom_spec_for_data(const std::vector<RomSpec>& specs, const std::vector<uint8_t>& data) {
    const std::string hash = sha256_hex(data.data(), data.size());
    for (const RomSpec& s : specs)
        if (!s.sha256.empty() && s.size == data.size() && s.sha256 == hash) return &s;
    return nullptr;
}

} // namespace em34xx
