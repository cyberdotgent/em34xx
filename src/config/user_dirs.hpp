// The per-user data folder of an end-user GUI build: settings, ROM images,
// the global keymap and connection profiles in one place. On Windows the
// root is %APPDATA%\tn34xx (tn34xxw finds %APPDATA% with SHGetFolderPathW);
// the root is injected, so tests use a temporary directory.
//
//   <root>/settings.toml     application settings (config/settings.hpp)
//   <root>/keymap.toml       the global host keymap (docs/keyboard.md)
//   <root>/roms/             ROM images, verified by SHA-256 (roms/ROMS.md)
//   <root>/profiles/*.tn34xx connection profiles (config/profile.hpp)
//   <root>/<model>.nvram     battery-backed RAM image per model
//
// Paths are UTF-8 std::strings; std::filesystem::u8path makes them wide on
// Windows.
#pragma once

#include "rom/romset.hpp"

#include <string>
#include <vector>

namespace tn34xx {

class Model;

class UserDirs {
public:
    explicit UserDirs(std::string root);

    const std::string& root() const { return root_; }
    std::string roms() const;
    std::string profiles() const;
    std::string settings_file() const;
    std::string keymap_file() const;
    std::string nvram_file(const std::string& model) const;

    // Create the root, roms/ and profiles/ when missing.
    bool ensure(std::string& error) const;

    // --- profiles ---------------------------------------------------------
    static constexpr const char* kProfileExtension = ".tn34xx";
    // Profile names (file names without the extension), sorted
    // case-insensitively.
    std::vector<std::string> list_profiles() const;
    std::string profile_path(const std::string& name) const;
    bool profile_exists(const std::string& name) const;
    // A name usable as a file name everywhere: not empty, no path
    // separators or characters Windows forbids, no trailing dot or space,
    // not a reserved device name.
    static bool valid_profile_name(const std::string& name, std::string& why);
    // `base`, or "base (2)", "base (3)"... whichever does not exist yet.
    std::string unique_profile_name(const std::string& base) const;
    bool rename_profile(const std::string& from, const std::string& to, std::string& error) const;
    bool copy_profile(const std::string& from, const std::string& to, std::string& error) const;
    bool delete_profile(const std::string& name, std::string& error) const;

    // --- ROMs ----------------------------------------------------------------
    // Copy a ROM image into roms/. A dump that is some model's known-good
    // image is stored under that model's file name, whatever it was called;
    // otherwise under its own file name (`note` then says it was not
    // recognised). `stored_as` is the file name in roms/.
    bool import_rom(const std::string& source, std::string& stored_as, std::string& note,
                    std::string& error) const;
    bool remove_rom(const std::string& file, std::string& error) const;

private:
    std::string root_;
};

// <appdata>/tn34xx
std::string user_dirs_root(const std::string& appdata);

// One line of a ROM manager: a ROM some model needs, and its state in the
// ROM folder.
struct RomStatusRow {
    std::string model;          // "3477"
    std::string model_status;   // "bring-up", "rom-incomplete"...
    RomSpec spec;
    RomFileStatus status;
};
// Every model's ROMs (placeholders without ROM specs have none).
std::vector<RomStatusRow> rom_status(const std::string& rom_dir);
// Whether every ROM `model` needs is present and loadable.
bool model_roms_ready(const Model& model, const std::string& rom_dir);

} // namespace tn34xx
