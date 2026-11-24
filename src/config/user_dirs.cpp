// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/user_dirs.hpp"

#include "models/model.hpp"
#include "util/file_io.hpp"
#include "util/strings.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace em34xx {

namespace fs = std::filesystem;

namespace {

fs::path upath(const std::string& utf8) { return fs::u8path(utf8); }

std::string ustr(const fs::path& p) {
#if defined(__cpp_char8_t)
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
#else
    return p.u8string();
#endif
}

std::string join(const std::string& dir, const std::string& name) { return ustr(upath(dir) / upath(name)); }

} // namespace

UserDirs::UserDirs(std::string root) : root_(std::move(root)) {}

std::string user_dirs_root(const std::string& appdata) { return join(appdata, "em34xx"); }

std::string UserDirs::roms() const { return join(root_, "roms"); }
std::string UserDirs::profiles() const { return join(root_, "profiles"); }
std::string UserDirs::settings_file() const { return join(root_, "settings.toml"); }
std::string UserDirs::keymap_file() const { return join(root_, "keymap.toml"); }
std::string UserDirs::nvram_file(const std::string& model) const { return join(root_, model + ".nvram"); }
std::string UserDirs::eeprom_file(const std::string& model) const { return join(root_, model + ".eeprom"); }

bool UserDirs::ensure(std::string& error) const {
    for (const std::string& d : {root_, roms(), profiles()}) {
        std::error_code ec;
        fs::create_directories(upath(d), ec);
        if (ec || !fs::is_directory(upath(d), ec)) {
            error = "cannot create the folder " + d + (ec ? ": " + ec.message() : std::string());
            return false;
        }
    }
    return true;
}

std::vector<std::string> UserDirs::list_profiles() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(upath(profiles()), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path& p = it->path();
        if (to_lower(ustr(p.extension())) != kProfileExtension) continue;
        out.push_back(ustr(p.stem()));
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const std::string la = to_lower(a), lb = to_lower(b);
        return la != lb ? la < lb : a < b;
    });
    return out;
}

std::string UserDirs::profile_path(const std::string& name) const {
    return join(profiles(), name + kProfileExtension);
}

bool UserDirs::profile_exists(const std::string& name) const {
    std::error_code ec;
    return fs::exists(upath(profile_path(name)), ec);
}

bool UserDirs::valid_profile_name(const std::string& name, std::string& why) {
    if (trim(name).empty()) {
        why = "the name is empty";
        return false;
    }
    if (name.size() > 120) {
        why = "the name is too long";
        return false;
    }
    for (unsigned char c : name) {
        if (c < 0x20 || std::string("<>:\"/\\|?*").find(char(c)) != std::string::npos) {
            why = "a name cannot contain < > : \" / \\ | ? * or control characters";
            return false;
        }
    }
    if (name.back() == '.' || name.back() == ' ' || name.front() == ' ') {
        why = "a name cannot start with a space or end with a dot or a space";
        return false;
    }
    std::string base = to_lower(name.substr(0, name.find('.')));
    static const char* const kReserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
                                            "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4",
                                            "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* r : kReserved) {
        if (base == r) {
            why = "\"" + name + "\" is a reserved device name on Windows";
            return false;
        }
    }
    return true;
}

std::string UserDirs::unique_profile_name(const std::string& base) const {
    if (!profile_exists(base)) return base;
    for (int i = 2;; ++i) {
        const std::string n = format("%s (%d)", base.c_str(), i);
        if (!profile_exists(n)) return n;
    }
}

bool UserDirs::rename_profile(const std::string& from, const std::string& to, std::string& error) const {
    std::string why;
    if (!valid_profile_name(to, why)) {
        error = why;
        return false;
    }
    // Renaming to a different case of the same name is allowed.
    if (to_lower(from) != to_lower(to) && profile_exists(to)) {
        error = "a profile named \"" + to + "\" already exists";
        return false;
    }
    std::error_code ec;
    if (from != to && to_lower(from) == to_lower(to)) {
        // A case-only rename: through a temporary name, since a
        // case-insensitive file system may treat it as a no-op.
        const fs::path tmp = upath(profile_path(from + ".renaming"));
        fs::rename(upath(profile_path(from)), tmp, ec);
        if (!ec) fs::rename(tmp, upath(profile_path(to)), ec);
    } else {
        fs::rename(upath(profile_path(from)), upath(profile_path(to)), ec);
    }
    if (ec) {
        error = "cannot rename \"" + from + "\": " + ec.message();
        return false;
    }
    return true;
}

bool UserDirs::copy_profile(const std::string& from, const std::string& to, std::string& error) const {
    std::string why;
    if (!valid_profile_name(to, why)) {
        error = why;
        return false;
    }
    if (profile_exists(to)) {
        error = "a profile named \"" + to + "\" already exists";
        return false;
    }
    std::error_code ec;
    fs::copy_file(upath(profile_path(from)), upath(profile_path(to)), ec);
    if (ec) {
        error = "cannot copy \"" + from + "\": " + ec.message();
        return false;
    }
    return true;
}

bool UserDirs::delete_profile(const std::string& name, std::string& error) const {
    std::error_code ec;
    if (!fs::remove(upath(profile_path(name)), ec) || ec) {
        error = "cannot delete \"" + name + "\"" + (ec ? ": " + ec.message() : std::string(": not found"));
        return false;
    }
    return true;
}

namespace {
// Every image a model can run on: its own set and its variants' sets.
std::vector<RomSpec> all_rom_specs(const Model& model) {
    std::vector<RomSpec> out = model.info().roms;
    for (const ModelVariant& v : model.info().variants) out.insert(out.end(), v.roms.begin(), v.roms.end());
    return out;
}
} // namespace

bool UserDirs::import_rom(const std::string& source, std::string& stored_as, std::string& note,
                          std::string& error) const {
    std::vector<uint8_t> data;
    const fs::path src = upath(source);
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) {
        error = "not a file: " + source;
        return false;
    }
    const auto size = fs::file_size(src, ec);
    if (ec || size > 16u * 1024 * 1024) {
        error = "not a ROM image (larger than 16 MiB): " + source;
        return false;
    }
    if (!read_file(source, data)) {
        error = "cannot read " + source;
        return false;
    }
    stored_as.clear();
    note.clear();
    // One image can serve several models (the 3486 and 3487 share theirs).
    std::string names;
    for (const Model* m : runnable_models()) {
        const std::vector<RomSpec> specs = all_rom_specs(*m);
        if (const RomSpec* spec = rom_spec_for_data(specs, data)) {
            if (names.empty()) stored_as = spec->file;
            names += (names.empty() ? "" : "/") + m->info().name;
            note = "IBM " + names + " " + spec->label + " (known good)";
        }
    }
    if (stored_as.empty()) {
        stored_as = ustr(src.filename());
        note = "not a known-good dump of any model; stored under its own name";
        for (const Model* m : runnable_models())
            for (const RomSpec& spec : all_rom_specs(*m))
                if (to_lower(spec.file) == to_lower(stored_as)) {
                    stored_as = spec.file;
                    note = "named like IBM " + m->info().name + " " + spec.label + ", but its hash is not known";
                }
    }
    const fs::path dest = upath(roms()) / upath(stored_as);
    if (fs::exists(dest, ec) && fs::equivalent(src, dest, ec)) return true;   // already in place
    fs::create_directories(upath(roms()), ec);
    // Copy next to the destination, then replace an older image in one
    // step: a failed copy keeps it. (The stale temporary is removed first:
    // MinGW's libstdc++ refuses copy_options::overwrite_existing on Windows
    // with "File exists".)
    fs::path tmp = dest;
    tmp += ".tmp";
    fs::remove(tmp, ec);
    ec.clear();
    fs::copy_file(src, tmp, ec);
    if (ec) {
        error = "cannot copy " + source + " to " + ustr(dest) + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    if (!replace_file(ustr(tmp), ustr(dest), error)) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool UserDirs::remove_rom(const std::string& file, std::string& error) const {
    if (file.empty() || file.find_first_of("/\\:") != std::string::npos) {
        error = "not a ROM file name: " + file;
        return false;
    }
    std::error_code ec;
    if (!fs::remove(upath(roms()) / upath(file), ec) || ec) {
        error = "cannot remove " + file + (ec ? ": " + ec.message() : std::string(": not found"));
        return false;
    }
    return true;
}

std::vector<RomStatusRow> rom_status(const std::string& rom_dir) {
    std::vector<RomStatusRow> out;
    // Only models a user can run: the 3489 dump is known to be bad, so
    // asking for it would only confuse.
    for (const Model* m : runnable_models()) {
        const ModelInfo& info = m->info();
        auto add = [&](const std::string& name, const std::vector<RomSpec>& specs) {
            for (const RomSpec& spec : specs) {
                RomStatusRow row;
                row.model = name;
                row.model_status = model_status_name(info.status);
                row.spec = spec;
                row.status = check_rom_file(rom_dir, spec, info.known_bad_roms);
                out.push_back(std::move(row));
            }
        };
        // The model's own set when a variant runs it, then the sets of the
        // variants that have their own, under the variant's name.
        bool shared = info.variants.empty();
        for (const ModelVariant& v : info.variants) shared = shared || v.roms.empty();
        if (shared) add(info.name, info.roms);
        for (const ModelVariant& v : info.variants)
            if (!v.roms.empty()) add(v.id, v.roms);
    }
    return out;
}

namespace {
bool specs_ready(const Model& model, const std::vector<RomSpec>& specs, const std::string& rom_dir) {
    for (const RomSpec& spec : specs)
        if (!check_rom_file(rom_dir, spec, model.info().known_bad_roms).usable()) return false;
    return true;
}
} // namespace

bool model_roms_ready(const Model& model, const std::string& rom_dir, const ModelVariant* variant) {
    const ModelInfo& info = model.info();
    if (variant) return specs_ready(model, info.roms_of(*variant), rom_dir);
    if (info.variants.empty()) return specs_ready(model, info.roms, rom_dir);
    return first_ready_variant(model, rom_dir) != nullptr;
}

const ModelVariant* first_ready_variant(const Model& model, const std::string& rom_dir) {
    for (const ModelVariant& v : model.info().variants)
        if (specs_ready(model, model.info().roms_of(v), rom_dir)) return &v;
    return nullptr;
}

std::vector<const Model*> ready_models(const std::string& rom_dir) {
    std::vector<const Model*> out;
    for (const Model* m : runnable_models())
        if (model_roms_ready(*m, rom_dir)) out.push_back(m);
    return out;
}

std::vector<RomStatusRow> unusable_roms(const Model& model, const std::string& rom_dir, const ModelVariant* variant) {
    std::vector<RomStatusRow> out;
    const ModelInfo& info = model.info();
    if (!variant && model_roms_ready(model, rom_dir)) return out;
    if (!variant && !info.variants.empty()) variant = &info.variants.front();
    for (const RomSpec& spec : variant ? info.roms_of(*variant) : info.roms) {
        RomStatusRow row;
        row.model = variant && !variant->roms.empty() ? variant->id : info.name;
        row.model_status = model_status_name(info.status);
        row.spec = spec;
        row.status = check_rom_file(rom_dir, spec, info.known_bad_roms);
        if (!row.status.usable()) out.push_back(std::move(row));
    }
    return out;
}

std::string unusable_roms_text(const Model& model, const std::string& rom_dir, const ModelVariant* variant) {
    std::string text;
    for (const RomStatusRow& r : unusable_roms(model, rom_dir, variant)) {
        text += r.spec.file + " - " + r.spec.label + ": " + rom_file_state_name(r.status.state);
        if (!r.status.note.empty()) text += " (" + r.status.note + ")";
        text += "\n";
    }
    return text;
}

StartModel choose_start_model(const std::string& wanted, const std::string& rom_dir) {
    StartModel out;
    const std::vector<const Model*> ready = ready_models(rom_dir);
    if (!ready.empty()) {
        out.model = ready.front();
        out.variant = first_ready_variant(*out.model, rom_dir);
    }
    const Model* model = nullptr;
    const ModelVariant* variant = nullptr;
    if (wanted.empty() || !find_model_variant(wanted, model, variant)) return out;
    bool runnable = false;
    for (const Model* m : runnable_models()) runnable = runnable || m == model;
    if (!runnable) return out;
    // A bare model name asks for any variant of it that can run.
    const bool bare = to_lower(wanted).find(to_lower(variant->id)) == std::string::npos;
    if (bare && !model_roms_ready(*model, rom_dir, variant))
        if (const ModelVariant* v = first_ready_variant(*model, rom_dir)) variant = v;
    if (model_roms_ready(*model, rom_dir, variant)) {
        out.model = model;
        out.variant = variant;
    } else {
        out.wanted_unready = model;
        out.wanted_variant = variant;
    }
    return out;
}

} // namespace em34xx
