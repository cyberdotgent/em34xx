#include "config/settings.hpp"

#include <toml++/toml.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace tn34xx {

namespace fs = std::filesystem;

double snap_zoom(double z) {
    double best = kZoomLevels[0];
    for (double l : kZoomLevels)
        if (std::fabs(l - z) < std::fabs(best - z)) best = l;
    return best;
}

bool read_text_file(const std::string& path, std::string& out, std::string& error) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_text_file(const std::string& path, const std::string& text, std::string& error) {
    const fs::path target = fs::u8path(path);
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "cannot write " + path;
            return false;
        }
        f << text;
        f.flush();
        if (!f) {
            error = "cannot write " + path;
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);   // replaces an existing file (MoveFileEx on Windows)
    if (ec) {
        fs::remove(tmp, ec);
        error = "cannot replace " + path;
        return false;
    }
    return true;
}

bool parse_settings(const std::string& text, const std::string& origin, AppSettings& out, std::string& error) {
    toml::table root;
    try {
        root = toml::parse(text, origin);
    } catch (const toml::parse_error& e) {
        std::ostringstream os;
        os << origin << ": " << e.description() << " (line " << e.source().begin.line << ")";
        error = os.str();
        return false;
    }
    AppSettings s;
    if (auto v = root["model"].value<std::string>()) s.model = *v;
    if (auto v = root["display"]["subtype"].value<std::string>()) {
        DisplaySubtype d;
        if (parse_display_subtype(*v, d)) s.display = d;
    }
    if (auto v = root["display"]["crt"].value<bool>()) s.crt = *v;
    if (auto v = root["emulator"]["speed"].value<int64_t>()) s.speed_percent = unsigned(*v < 0 ? 0 : *v > 10000 ? 10000 : *v);
    if (auto v = root["tls"]["min_version"].value<std::string>()) {
        TlsVersion t;
        if (parse_tls_version(*v, t) && t != TlsVersion::Any) s.tls_min_version = t;
    }
    if (auto v = root["window"]["zoom"].value<double>()) s.zoom = snap_zoom(*v);
    const toml::table* w = root["window"].as_table();
    if (w) {
        auto x = (*w)["x"].value<int64_t>(), y = (*w)["y"].value<int64_t>();
        auto wd = (*w)["width"].value<int64_t>(), ht = (*w)["height"].value<int64_t>();
        if (x && y && wd && ht && *wd > 0 && *ht > 0 && *wd < 100000 && *ht < 100000 && std::llabs(*x) < 100000 &&
            std::llabs(*y) < 100000) {
            s.window.valid = true;
            s.window.x = int(*x);
            s.window.y = int(*y);
            s.window.width = int(*wd);
            s.window.height = int(*ht);
        }
        if (auto m = (*w)["maximized"].value<bool>()) s.window.maximized = *m;
    }
    out = s;
    return true;
}

std::string settings_to_toml(const AppSettings& s) {
    toml::table window{{"zoom", s.zoom}, {"maximized", s.window.maximized}};
    if (s.window.valid) {
        window.insert("x", int64_t(s.window.x));
        window.insert("y", int64_t(s.window.y));
        window.insert("width", int64_t(s.window.width));
        window.insert("height", int64_t(s.window.height));
    }
    toml::table root{
        {"model", s.model},
        {"display", toml::table{{"subtype", display_subtype_name(s.display)}, {"crt", s.crt}}},
        {"emulator", toml::table{{"speed", int64_t(s.speed_percent)}}},
        {"tls", toml::table{{"min_version", tls_version_name(s.tls_min_version)}}},
        {"window", window},
    };
    std::ostringstream os;
    os << "# tn34xx application settings (written by tn34xxw; docs/frontend-win32.md)\n\n" << root << "\n";
    return os.str();
}

bool load_settings(const std::string& path, AppSettings& out, std::string& error) {
    std::error_code ec;
    if (!fs::exists(fs::u8path(path), ec)) {
        out = AppSettings();
        return true;
    }
    std::string text;
    if (!read_text_file(path, text, error)) return false;
    return parse_settings(text, path, out, error);
}

bool save_settings(const std::string& path, const AppSettings& s, std::string& error) {
    return write_text_file(path, settings_to_toml(s), error);
}

} // namespace tn34xx
