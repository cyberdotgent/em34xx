// SPDX-License-Identifier: GPL-3.0-or-later
// The end-user GUI's data: the user folder (config/user_dirs.hpp), settings
// (config/settings.hpp) and connection profiles (config/profile.hpp).
#include "config/profile.hpp"
#include "config/settings.hpp"
#include "config/user_dirs.hpp"
#include "models/model.hpp"
#include "util/fingerprint.hpp"
#include "util/sha256.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>

using namespace em34xx;
namespace fs = std::filesystem;

namespace {

// A fresh directory under the system temp folder, removed afterwards.
struct TempRoot {
    fs::path path;
    TempRoot() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("em34xx_userdata_" + std::to_string(rd()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempRoot() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

void write_bytes(const fs::path& p, const std::vector<uint8_t>& data) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

Sha256Digest digest(uint8_t seed) {
    Sha256Digest d{};
    for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(seed + i);
    return d;
}

} // namespace

TEST_CASE("user folder: layout under an injected root") {
    TempRoot tmp;
    const std::string root = user_dirs_root(tmp.str());
    CHECK(fs::path(root) == tmp.path / "em34xx");
    UserDirs dirs(root);
    CHECK(fs::path(dirs.roms()) == tmp.path / "em34xx" / "roms");
    CHECK(fs::path(dirs.profiles()) == tmp.path / "em34xx" / "profiles");
    CHECK(fs::path(dirs.settings_file()) == tmp.path / "em34xx" / "settings.toml");
    CHECK(fs::path(dirs.keymap_file()) == tmp.path / "em34xx" / "keymap.toml");
    CHECK(fs::path(dirs.nvram_file("3477")) == tmp.path / "em34xx" / "3477.nvram");
    std::string err;
    REQUIRE(dirs.ensure(err));
    CHECK(fs::is_directory(dirs.roms()));
    CHECK(fs::is_directory(dirs.profiles()));
    CHECK(dirs.ensure(err));   // idempotent
}

TEST_CASE("user folder: profile files") {
    TempRoot tmp;
    UserDirs dirs(user_dirs_root(tmp.str()));
    std::string err;
    REQUIRE(dirs.ensure(err));
    CHECK(dirs.list_profiles().empty());
    Profile p;
    p.sessions[0].host = "pub400.com";
    REQUIRE(save_profile(dirs.profile_path("beta"), p, err));
    REQUIRE(save_profile(dirs.profile_path("Alpha"), p, err));
    std::ofstream(fs::path(dirs.profiles()) / "notes.txt") << "not a profile";
    CHECK(dirs.list_profiles() == std::vector<std::string>{"Alpha", "beta"});

    CHECK(dirs.unique_profile_name("gamma") == "gamma");
    CHECK(dirs.unique_profile_name("beta") == "beta (2)");
    REQUIRE(dirs.copy_profile("beta", dirs.unique_profile_name("beta"), err));
    CHECK(dirs.unique_profile_name("beta") == "beta (3)");
    CHECK_FALSE(dirs.copy_profile("beta", "Alpha", err));
    CHECK(err.find("already exists") != std::string::npos);

    REQUIRE(dirs.rename_profile("beta (2)", "Delta", err));
    CHECK_FALSE(dirs.rename_profile("Delta", "Alpha", err));
    REQUIRE(dirs.rename_profile("Alpha", "ALPHA", err));   // case-only rename
    CHECK_FALSE(dirs.rename_profile("Delta", "a/b", err));
    REQUIRE(dirs.delete_profile("Delta", err));
    CHECK_FALSE(dirs.delete_profile("Delta", err));
    CHECK(dirs.list_profiles() == std::vector<std::string>{"ALPHA", "beta"});

    std::string why;
    CHECK(UserDirs::valid_profile_name("PUB400 (TLS)", why));
    CHECK(UserDirs::valid_profile_name("Zürich", why));
    CHECK_FALSE(UserDirs::valid_profile_name("", why));
    CHECK_FALSE(UserDirs::valid_profile_name("a:b", why));
    CHECK_FALSE(UserDirs::valid_profile_name("name.", why));
    CHECK_FALSE(UserDirs::valid_profile_name("CON", why));
    CHECK_FALSE(UserDirs::valid_profile_name("nul.txt", why));
}

TEST_CASE("user folder: which model a front end starts as") {
    TempRoot tmp;
    UserDirs dirs(user_dirs_root(tmp.str()));
    std::string err;
    REQUIRE(dirs.ensure(err));
    const Model* m3477 = find_model("3477");
    const Model* m3486 = find_model("3486");
    const Model* m3487 = find_model("3487");
    REQUIRE(m3477);
    REQUIRE(m3486);
    REQUIRE(m3487);

    // No ROMs: nothing can start, whatever is wanted; a wanted runnable
    // model is reported so that its files can be named.
    CHECK(ready_models(dirs.roms()).empty());
    StartModel sm = choose_start_model("", dirs.roms());
    CHECK(sm.model == nullptr);
    CHECK(sm.wanted_unready == nullptr);
    sm = choose_start_model("3487", dirs.roms());
    CHECK(sm.model == nullptr);
    CHECK(sm.wanted_unready == m3487);
    const std::string text = unusable_roms_text(*m3487, dirs.roms());
    CHECK(text.find(m3487->info().roms[0].file) != std::string::npos);
    CHECK(text.find("missing") != std::string::npos);
    CHECK(unusable_roms(*m3477, dirs.roms()).size() == m3477->info().roms.size());

    // Only the 3486/3487 image (right size, unknown hash: usable): both of
    // its models are ready, the 3477 is not, and one set is enough.
    const RomSpec& shared = m3487->info().roms[0];
    write_bytes(fs::path(dirs.roms()) / shared.file, std::vector<uint8_t>(shared.size, 0x11));
    const auto ready = ready_models(dirs.roms());
    REQUIRE(ready.size() == 2);
    CHECK(ready[0] == m3486);
    CHECK(ready[1] == m3487);
    CHECK(unusable_roms_text(*m3487, dirs.roms()).empty());
    sm = choose_start_model("", dirs.roms());
    CHECK(sm.model == m3486);
    CHECK(sm.wanted_unready == nullptr);
    sm = choose_start_model("3487", dirs.roms());
    CHECK(sm.model == m3487);
    CHECK(sm.wanted_unready == nullptr);
    // A profile for the 3477: it is named as wanted, and a ready model is
    // still offered to start with.
    sm = choose_start_model("3477", dirs.roms());
    CHECK(sm.model == m3486);
    CHECK(sm.wanted_unready == m3477);
    // Only one of the 3477's two images: still not ready, and only the
    // other one is listed.
    const RomSpec& first = m3477->info().roms[0];
    write_bytes(fs::path(dirs.roms()) / first.file, std::vector<uint8_t>(first.size, 0x22));
    CHECK_FALSE(model_roms_ready(*m3477, dirs.roms()));
    const auto lacking = unusable_roms(*m3477, dirs.roms());
    REQUIRE(lacking.size() == m3477->info().roms.size() - 1);
    CHECK(lacking[0].spec.file != first.file);
    // Unknown and not yet runnable names are no preference.
    sm = choose_start_model("3489", dirs.roms());
    CHECK(sm.model == m3486);
    CHECK(sm.wanted_unready == nullptr);
    sm = choose_start_model("no-such-model", dirs.roms());
    CHECK(sm.model == m3486);
    CHECK(sm.wanted_unready == nullptr);
}

TEST_CASE("user folder: ROM status and import") {
    TempRoot tmp;
    UserDirs dirs(user_dirs_root(tmp.str()));
    std::string err;
    REQUIRE(dirs.ensure(err));
    const Model* m3477 = find_model("3477");
    REQUIRE(m3477);
    CHECK_FALSE(model_roms_ready(*m3477, dirs.roms()));
    const auto rows = rom_status(dirs.roms());
    size_t rows3477 = 0;
    for (const RomStatusRow& r : rows) {
        CHECK(r.status.state == RomFileStatus::State::Missing);
        if (r.model == "3477") ++rows3477;
    }
    CHECK(rows3477 == m3477->info().roms.size());
    // Models without a good dump are not asked for.
    for (const RomStatusRow& r : rows) CHECK(r.model != "3489");

    // An image of the right size with an unknown hash, imported under its own
    // name: it is "unknown hash", which still runs.
    const RomSpec& spec = m3477->info().roms[0];
    std::vector<uint8_t> data(spec.size, 0x5A);
    const fs::path src = tmp.path / spec.file;
    write_bytes(src, data);
    std::string stored, note;
    REQUIRE(dirs.import_rom(src.string(), stored, note, err));
    CHECK(stored == spec.file);
    CHECK(note.find("hash is not known") != std::string::npos);
    RomFileStatus st = check_rom_file(dirs.roms(), spec, m3477->info().known_bad_roms);
    CHECK(st.state == RomFileStatus::State::UnknownHash);
    CHECK(st.sha256 == sha256_hex(data.data(), data.size()));
    CHECK(st.usable());

    // Wrong size.
    write_bytes(fs::path(dirs.roms()) / spec.file, std::vector<uint8_t>(16, 0));
    st = check_rom_file(dirs.roms(), spec, m3477->info().known_bad_roms);
    CHECK(st.state == RomFileStatus::State::WrongSize);
    CHECK(st.note.find("16 bytes") != std::string::npos);
    CHECK_FALSE(st.usable());

    // Importing over an existing image replaces it and leaves no temporary.
    REQUIRE(dirs.import_rom(src.string(), stored, note, err));
    CHECK(check_rom_file(dirs.roms(), spec, m3477->info().known_bad_roms).state ==
          RomFileStatus::State::UnknownHash);
    CHECK_FALSE(fs::exists(fs::path(dirs.roms()) / (spec.file + ".tmp")));

    // A file nobody needs keeps its name.
    const fs::path other = tmp.path / "other.bin";
    write_bytes(other, {1, 2, 3});
    REQUIRE(dirs.import_rom(other.string(), stored, note, err));
    CHECK(stored == "other.bin");
    CHECK(fs::exists(fs::path(dirs.roms()) / "other.bin"));
    REQUIRE(dirs.remove_rom("other.bin", err));
    CHECK_FALSE(fs::exists(fs::path(dirs.roms()) / "other.bin"));
    CHECK_FALSE(dirs.remove_rom("../x", err));

    // The real dumps, when the repository has them: recognised by hash under
    // any name, and then the model is ready.
    const fs::path repo_roms = fs::path(EM34XX_SOURCE_DIR) / "roms";
    bool have_all = true;
    for (const RomSpec& s : m3477->info().roms) have_all = have_all && fs::exists(repo_roms / s.file);
    if (have_all) {
        for (const RomSpec& s : m3477->info().roms) {
            const fs::path renamed = tmp.path / ("dump-" + s.file);
            fs::copy_file(repo_roms / s.file, renamed, fs::copy_options::overwrite_existing);
            INFO(err);
            REQUIRE(dirs.import_rom(renamed.string(), stored, note, err));
            CHECK(stored == s.file);
            CHECK(note.find("known good") != std::string::npos);
            CHECK(check_rom_file(dirs.roms(), s, {}).state == RomFileStatus::State::Good);
        }
        CHECK(model_roms_ready(*m3477, dirs.roms()));
    }
}

TEST_CASE("settings: defaults, round trip, tolerance") {
    AppSettings s;
    std::string err;
    REQUIRE(parse_settings("", "empty", s, err));
    CHECK(s.zoom == 1.0);
    CHECK(s.speed_percent == 100);
    CHECK_FALSE(s.window.valid);

    s.speed_percent = 0;
    s.startup_open = true;
    s.startup_profile = "PUB400";
    s.tls_min_version = TlsVersion::Tls1_0;
    s.zoom = 1.5;
    s.window = {true, -20, 30, 1024, 800, true};
    AppSettings back;
    REQUIRE(parse_settings(settings_to_toml(s), "rt", back, err));
    CHECK(back.speed_percent == 0);
    CHECK(back.startup_open);
    CHECK(back.startup_profile == "PUB400");
    CHECK(back.tls_min_version == TlsVersion::Tls1_0);
    CHECK(back.zoom == 1.5);
    CHECK(back.window.valid);
    CHECK(back.window.x == -20);
    CHECK(back.window.height == 800);
    CHECK(back.window.maximized);

    REQUIRE(parse_settings("[window]\nzoom = 1.7\nunknown = 3\n[display]\nsubtype = \"plaid\"\n", "t", back, err));
    CHECK(back.zoom == 1.5);
    CHECK(snap_zoom(3.0) == 2.0);
    CHECK_FALSE(parse_settings("[window", "bad", back, err));

    TempRoot tmp;
    const std::string path = (tmp.path / "settings.toml").string();
    REQUIRE(load_settings(path, back, err));   // missing file: defaults
    REQUIRE(save_settings(path, s, err));
    REQUIRE(save_settings(path, s, err));      // replaces
    REQUIRE(load_settings(path, back, err));
    CHECK(back.zoom == 1.5);
}

TEST_CASE("profile: TOML round trip and validation") {
    Profile p;
    p.model = "3477";
    p.display = DisplaySubtype::Amber;
    p.crt = false;
    p.tls_mode = ProfileTlsMode::Fingerprint;
    p.tls_cert_sha256 = format_sha256_fingerprint(digest(1));
    p.tls_min_version = "1.1";
    p.sessions[0] = {"pub400.com", 992, "QPADEV0001", "", true};
    p.sessions[1] = {"other.example", 9992, "", format_sha256_fingerprint(digest(9)), false};
    CHECK(p.validate().empty());
    const std::string text = profile_to_toml(p);
    CHECK(text.find("password") == std::string::npos);   // never stored (the comment says "Passwords")
    Profile back;
    std::string err;
    REQUIRE(parse_profile(text, "rt.em34xx", back, err));
    CHECK(back == p);
    CHECK(back.display == DisplaySubtype::Amber);
    CHECK_FALSE(back.crt);
    CHECK(back.sessions[0].tls);
    CHECK_FALSE(back.sessions[1].tls);
    Profile changed = p;
    changed.display = DisplaySubtype::Color;
    CHECK(changed != p);
    changed = p;
    changed.sessions[1].tls = true;
    CHECK(changed != p);

    TempRoot tmp;
    const std::string path = (tmp.path / "x.em34xx").string();
    REQUIRE(save_profile(path, p, err));
    REQUIRE(load_profile(path, back, err));
    CHECK(back == p);

    // An unused second session is not written, and reads back unused.
    Profile one;
    one.sessions[0].host = "pub400.com";
    REQUIRE(parse_profile(profile_to_toml(one), "one", back, err));
    CHECK(back == one);
    CHECK_FALSE(back.sessions[1].used());

    Profile bad = one;
    bad.sessions[0].host.clear();
    CHECK(bad.validate().find("session 1") != std::string::npos);
    bad = one;
    bad.tls_mode = ProfileTlsMode::Fingerprint;
    CHECK(bad.validate().empty());   // no session uses TLS
    bad.sessions[0].tls = true;
    CHECK_FALSE(bad.validate().empty());
    bad.tls_cert_sha256 = "zz";
    CHECK_FALSE(bad.validate().empty());
    bad = one;
    bad.tls_min_version = "1.9";
    CHECK_FALSE(bad.validate().empty());
    bad = one;
    bad.sessions[0].device_name = "ELEVENCHARS";
    CHECK_FALSE(bad.validate().empty());

    CHECK_FALSE(parse_profile("format = 99\n", "new", back, err));
    CHECK(err.find("newer") != std::string::npos);
    CHECK_FALSE(parse_profile("[tls]\nverify = \"maybe\"\n", "v", back, err));
    CHECK_FALSE(parse_profile("[[session]]\nport = 0\n", "p", back, err));
    CHECK_FALSE(parse_profile("[[session]]\n[[session]]\n[[session]]\n", "3", back, err));
    CHECK_FALSE(parse_profile("[display]\nsubtype = \"plaid\"\n", "d", back, err));

    // Format 1 (em34xx 0.1): one tls.enabled for every session, no [display].
    REQUIRE(parse_profile("format = 1\nmodel = \"3477\"\n[tls]\nenabled = true\nverify = \"system\"\n"
                          "[[session]]\nhost = \"a\"\nport = 992\n[[session]]\nhost = \"b\"\nport = 992\n",
                          "v1", back, err));
    CHECK(back.sessions[0].tls);
    CHECK(back.sessions[1].tls);
    CHECK(back.display == DisplaySubtype::Green);
    CHECK(back.crt);
    CHECK(profile_to_toml(back).find("format = 2") != std::string::npos);
}

TEST_CASE("profile: the connect configuration and trusting a certificate") {
    AppConfig base;
    base.model = "3477";
    Profile p;
    p.sessions[0] = {"pub400.com", 23, "", ""};
    AppConfig c;
    std::string err;
    REQUIRE(profile_connect_config(p, base, TlsVersion::Tls1_2, c, err));
    CHECK(c.twinax.host == TwinaxHostKind::Tn5250);
    REQUIRE(c.twinax.stations.size() == 1);
    CHECK(c.twinax.stations[0].address == 0);
    CHECK(c.tn5250.host == "pub400.com");
    CHECK(c.tn5250.port == 23);
    CHECK_FALSE(c.tn5250.tls);

    p.tls_mode = ProfileTlsMode::AcceptAll;
    p.sessions[0].port = 992;
    p.sessions[0].tls = true;
    p.sessions[1] = {"pub400.com", 992, "SECOND", "", true};
    REQUIRE(profile_connect_config(p, base, TlsVersion::Tls1_0, c, err));
    CHECK(c.tn5250.tls);
    CHECK(c.tn5250.tls_verify == TlsVerify::None);
    CHECK(c.tn5250.tls_min_version == TlsVersion::Tls1_0);   // the application default
    REQUIRE(c.twinax.stations.size() == 2);
    CHECK(c.twinax.stations[1].address == 1);
    CHECK(c.twinax.stations[1].device_name == "SECOND");
    CHECK(c.twinax.stations[1].host.empty());   // same system as session 1

    // Trust permanently: same host -> the profile's fingerprint mode.
    p.tls_mode = ProfileTlsMode::System;
    profile_trust_certificate(p, 1, digest(3));
    CHECK(p.tls_mode == ProfileTlsMode::Fingerprint);
    CHECK(p.tls_cert_sha256 == format_sha256_fingerprint(digest(3)));
    REQUIRE(profile_connect_config(p, base, TlsVersion::Tls1_2, c, err));
    CHECK(c.tn5250.tls_cert_sha256 == p.tls_cert_sha256);
    CHECK(c.tn5250.tls_verify == TlsVerify::System);

    // Session 2 on another system: its own fingerprint, and its own host.
    p.sessions[1].host = "other.example";
    p.sessions[1].port = 9992;
    p.tls_min_version = "1.3";
    profile_trust_certificate(p, 1, digest(7));
    CHECK(p.tls_cert_sha256 == format_sha256_fingerprint(digest(3)));
    CHECK(p.sessions[1].cert_sha256 == format_sha256_fingerprint(digest(7)));
    REQUIRE(profile_connect_config(p, base, TlsVersion::Tls1_2, c, err));
    CHECK(c.tn5250.tls_min_version == TlsVersion::Tls1_3);
    CHECK(c.twinax.stations[1].host == "other.example");
    CHECK(c.twinax.stations[1].port == 9992);
    CHECK(c.twinax.stations[1].tls_cert_sha256 == p.sessions[1].cert_sha256);

    CHECK(c.twinax.stations[1].tls == 1);

    // Mixed: session 1 plain, session 2 with TLS and the profile's pin.
    Profile mixed;
    mixed.tls_mode = ProfileTlsMode::Fingerprint;
    mixed.tls_cert_sha256 = format_sha256_fingerprint(digest(4));
    mixed.sessions[0] = {"pub400.com", 23, "", "", false};
    mixed.sessions[1] = {"pub400.com", 992, "", "", true};
    REQUIRE(profile_connect_config(mixed, base, TlsVersion::Tls1_2, c, err));
    CHECK_FALSE(c.tn5250.tls);
    CHECK(c.tn5250.tls_cert_sha256.empty());
    REQUIRE(c.twinax.stations.size() == 2);
    CHECK(c.twinax.stations[1].tls == 1);
    CHECK(c.twinax.stations[1].port == 992);
    CHECK(c.twinax.stations[1].tls_cert_sha256 == mixed.tls_cert_sha256);
    Tn5250Config s2 = c.tn5250;
    apply_station_tls(s2, c.twinax.stations[1]);
    CHECK(s2.tls);
    CHECK(s2.tls_cert_sha256 == mixed.tls_cert_sha256);
    // And the other way round: session 2 plain.
    mixed.sessions[0].tls = true;
    mixed.sessions[0].port = 992;
    mixed.sessions[1].tls = false;
    mixed.sessions[1].port = 23;
    REQUIRE(profile_connect_config(mixed, base, TlsVersion::Tls1_2, c, err));
    CHECK(c.tn5250.tls);
    CHECK(c.twinax.stations[1].tls == 0);
    s2 = c.tn5250;
    apply_station_tls(s2, c.twinax.stations[1]);
    CHECK_FALSE(s2.tls);
    CHECK(s2.validate().empty());

    Profile invalid;
    CHECK_FALSE(profile_connect_config(invalid, base, TlsVersion::Tls1_2, c, err));
}

TEST_CASE("models offered to end users") {
    const auto models = runnable_models();
    REQUIRE_FALSE(models.empty());
    CHECK(models[0]->info().name == "3477");
    for (const Model* m : models) CHECK(m->info().status == ModelStatus::Bringup);
}
