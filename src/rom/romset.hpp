// SPDX-License-Identifier: GPL-3.0-or-later
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

// One ROM file's state in a ROM directory, without loading the machine:
// what a ROM manager lists.
struct RomFileStatus {
    enum class State {
        Missing,       // no such file
        Unreadable,    // there, but cannot be read
        WrongSize,     // not the size the spec needs
        KnownBad,      // a known-defective dump
        UnknownHash,   // right size, hash not known (another revision?)
        Good,          // the known-good dump
    };
    State state = State::Missing;
    std::string sha256;   // when read
    std::string note;     // known-bad reason, size found...
    // Whether load_roms() would accept it (allow_bad: known-bad too).
    bool usable(bool allow_bad = false) const {
        return state == State::Good || state == State::UnknownHash || (allow_bad && state == State::KnownBad);
    }
};

const char* rom_file_state_name(RomFileStatus::State s);
RomFileStatus check_rom_file(const std::string& dir, const RomSpec& spec, const std::vector<KnownBadRom>& known_bad);
// The spec whose known-good hash `data` has, or null: lets an import find
// the right file name for a dump saved under another name.
const RomSpec* rom_spec_for_data(const std::vector<RomSpec>& specs, const std::vector<uint8_t>& data);

} // namespace tn34xx
