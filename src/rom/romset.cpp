#include "rom/romset.hpp"

#include "util/sha256.hpp"
#include "util/strings.hpp"

#include <cstdio>

namespace tn34xx {

const char* rom_verdict_name(LoadedRom::Verdict v) {
    switch (v) {
    case LoadedRom::Verdict::Good: return "good";
    case LoadedRom::Verdict::UnknownHash: return "unknown";
    case LoadedRom::Verdict::KnownBad: return "KNOWN BAD";
    }
    return "?";
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
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

} // namespace tn34xx
