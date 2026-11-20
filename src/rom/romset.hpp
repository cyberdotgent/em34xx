// Loading and identifying ROM images. ROMs are never distributed with the
// emulator; they are looked up by file name in a ROM directory and checked
// against known SHA-256 hashes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tn34xx {

struct RomSpec {
    std::string label;     // e.g. "system ROM (low)"
    std::string file;      // file name inside the ROM directory
    uint32_t size = 0;
    std::string sha256;    // known-good hash; empty if no good dump is known
    uint32_t base = 0;     // physical load address
};

struct KnownBadRom {
    std::string file;
    std::string sha256;
    std::string reason;
};

struct LoadedRom {
    enum class Verdict { Good, UnknownHash, KnownBad };
    RomSpec spec;
    std::vector<uint8_t> data;
    std::string sha256;
    Verdict verdict = Verdict::Good;
    std::string note;
};

const char* rom_verdict_name(LoadedRom::Verdict v);

// Loads every ROM in specs from dir. Fails on missing files or wrong sizes,
// and on known-bad dumps unless allow_bad is set. Unknown hashes produce a
// warning only, since other firmware revisions exist.
bool load_roms(const std::string& dir, const std::vector<RomSpec>& specs,
               const std::vector<KnownBadRom>& known_bad, bool allow_bad,
               std::vector<LoadedRom>& out, std::vector<std::string>& warnings, std::string& error);

bool read_file(const std::string& path, std::vector<uint8_t>& out);

} // namespace tn34xx
