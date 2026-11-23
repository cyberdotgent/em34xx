// SPDX-License-Identifier: GPL-3.0-or-later
#include "input/host_keymap.hpp"
#include "input/set3_keyboard.hpp"
#include "util/strings.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace em34xx;

namespace {

// HID usages used below.
constexpr uint32_t kA = 0x04, kEnter = 0x28, kEsc = 0x29, kTab = 0x2B, kF1 = 0x3A, kF2 = 0x3B,
                   kPrtSc = 0x46, kPause = 0x48, kInsert = 0x49, kHome = 0x4A, kPgUp = 0x4B,
                   kDelete = 0x4C, kEnd = 0x4D, kPgDn = 0x4E, kKpMinus = 0x56, kKpPlus = 0x57,
                   kKpEnter = 0x58, kLCtrl = 0xE0, kLShift = 0xE1, kLAlt = 0xE2, kRCtrl = 0xE4,
                   kRShift = 0xE5, kRAlt = 0xE6;

// Drives an engine and records the terminal key events as "+12" / "-12".
struct Rig {
    HostKeymapEngine engine{&Set3Keyboard::hid_to_set3};
    KeyboardType type;
    std::vector<std::string> out;

    explicit Rig(KeyboardType t, const std::string& map = "pcomm", bool shift_fkeys = true) : type(t) {
        auto km = std::make_shared<Keymap>();
        REQUIRE(builtin_keymap(map, shift_fkeys, *km));
        engine.set_keymap(km);
    }
    Rig(KeyboardType t, std::shared_ptr<Keymap> km) : type(t) { engine.set_keymap(std::move(km)); }

    void key(uint32_t hid, bool down) {
        engine.host_key(type, hid, down, [this](uint8_t c, bool p) { out.push_back(format("%c%02X", p ? '+' : '-', c)); });
    }
    void tap(uint32_t hid) { key(hid, true); key(hid, false); }
    std::vector<std::string> take() {
        std::vector<std::string> v;
        v.swap(out);
        return v;
    }
};

using Ev = std::vector<std::string>;

std::shared_ptr<Keymap> parse(const std::string& text) {
    auto km = std::make_shared<Keymap>();
    std::string error;
    const bool ok = parse_keymap(text, "test.toml", true, *km, error);
    INFO(error);
    REQUIRE(ok);
    return km;
}

bool same_rule(const KeymapRule& a, const KeymapRule& b) {
    if (a.hid != b.hid || a.shift != b.shift || a.ctrl != b.ctrl || a.alt != b.alt || a.tap != b.tap ||
        a.positional != b.positional || a.applies != b.applies)
        return false;
    for (size_t i = 0; i < 2; ++i)
        if (a.applies[i] && !a.positional && a.to[i] != b.to[i]) return false;
    return true;
}

void check_same_keymap(const Keymap& a, const Keymap& b) {
    REQUIRE(a.rules().size() == b.rules().size());
    for (size_t i = 0; i < a.rules().size(); ++i) {
        INFO("rule " << i << " (" << b.rules()[i].source << ")");
        CHECK(same_rule(a.rules()[i], b.rules()[i]));
    }
}

} // namespace

TEST_CASE("keymap: unmapped keys press the key at the same position") {
    Rig r(KeyboardType::Terminal122, "positional");
    r.tap(kA);
    r.tap(kEnter);
    CHECK(r.take() == Ev{"+1C", "-1C", "+5A", "-5A"});
    // Shift and Alt pass through as the terminal's own modifier keys.
    r.key(kRShift, true);
    r.tap(kA);
    r.key(kRShift, false);
    r.key(kLAlt, true);
    r.key(kLAlt, false);
    CHECK(r.take() == Ev{"+59", "+1C", "-1C", "-59", "+19", "-19"});
    // A key without any mapping produces nothing.
    r.tap(0x9999);
    CHECK(r.take().empty());
}

TEST_CASE("keymap: Shift+F1 releases the terminal Shift around F13") {
    Rig r(KeyboardType::Terminal122, "positional");
    r.key(kLShift, true);
    r.key(kF1, true);
    r.key(kF1, false);
    r.key(kLShift, false);
    CHECK(r.take() == Ev{"+12", "-12", "+08", "-08", "+12", "-12"});

    // Both Shift keys held: both are released and restored.
    r.key(kLShift, true);
    r.key(kRShift, true);
    r.key(kF2, true);
    CHECK(r.take() == Ev{"+12", "+59", "-12", "-59", "+10"});
    r.key(kF2, false);
    CHECK(r.take() == Ev{"-10", "+12", "+59"});
    r.key(kLShift, false);
    r.key(kRShift, false);
    CHECK(r.take() == Ev{"-12", "-59"});

    // Without the shift_fkeys rules Shift+F1 is just Shift and F1.
    Rig p(KeyboardType::Terminal122, "positional", false);
    p.key(kLShift, true);
    p.tap(kF1);
    p.key(kLShift, false);
    CHECK(p.take() == Ev{"+12", "+07", "-07", "-12"});
}

TEST_CASE("keymap: release undoes exactly what the press did") {
    Rig r(KeyboardType::Terminal122, "positional");
    // Shift released before F1: still F13 that goes up, and Shift was
    // already up for the terminal, so nothing more happens.
    r.key(kLShift, true);
    r.key(kF1, true);
    r.key(kLShift, false);
    CHECK(r.take() == Ev{"+12", "-12", "+08"});
    r.key(kF1, false);
    CHECK(r.take() == Ev{"-08"});

    // F1 pressed unshifted, Shift pressed while it is down: F1 is released,
    // not F13.
    r.key(kF1, true);
    r.key(kLShift, true);
    r.key(kF1, false);
    r.key(kLShift, false);
    CHECK(r.take() == Ev{"+07", "+12", "-07", "-12"});

    // Shift pressed and released while F13 is down is invisible.
    r.key(kRShift, true);
    r.key(kF1, true);
    r.key(kLShift, true);
    r.key(kLShift, false);
    CHECK(r.take() == Ev{"+59", "-59", "+08"});
    r.key(kF1, false);
    r.key(kRShift, false);
    CHECK(r.take() == Ev{"-08", "+59", "-59"});

    // Host auto-repeat (a second press) is ignored; a stray release too.
    r.key(kA, true);
    r.key(kA, true);
    r.key(kA, false);
    r.key(kA, false);
    CHECK(r.take() == Ev{"+1C", "-1C"});
}

TEST_CASE("keymap: overlapping rules, most specific wins, later wins ties") {
    auto km = parse(R"(
base = "none"
[[map]]
host = "a"
mods = "*"
to = "0x21"
[[map]]
host = "a"
mods = "shift"
to = "0x22"
[[map]]
host = "a"
mods = "lshift"
to = "0x23"
[[map]]
host = "a"
mods = "shift"
to = "0x24"
[[map]]
host = "a"
mods = ["shift", "*"]
to = "0x25"
)");
    Rig r(KeyboardType::Terminal122, km);
    r.tap(kA);                       // only "*" matches
    CHECK(r.take() == Ev{"+21", "-21"});
    r.key(kRShift, true);
    r.tap(kA);                       // shift twice (0x22, 0x24): the later wins
    r.key(kRShift, false);
    CHECK(r.take() == Ev{"+59", "+24", "-24", "-59"});
    r.key(kLShift, true);
    r.tap(kA);                       // lshift is more specific than shift
    r.key(kLShift, false);
    CHECK(r.take() == Ev{"+12", "+23", "-23", "-12"});
    r.key(kRShift, true);
    r.key(kLCtrl, true);
    r.tap(kA);                       // shift+ctrl: only "shift, *" matches
    r.key(kLCtrl, false);
    r.key(kRShift, false);
    CHECK(r.take() == Ev{"+59", "+11", "+25", "-25", "-11", "-59"});
    r.key(kLAlt, true);
    r.tap(kA);                       // alt: "*"
    r.key(kLAlt, false);
    CHECK(r.take() == Ev{"+19", "+21", "-21", "-19"});
}

TEST_CASE("keymap: tap keys act as Ctrl when combined") {
    Rig r(KeyboardType::Terminal122);
    // Right Ctrl alone: Enter, sent on release.
    r.key(kRCtrl, true);
    CHECK(r.take().empty());
    r.key(kRCtrl, false);
    CHECK(r.take() == Ev{"+58", "-58"});
    // Left Ctrl alone: Reset.
    r.tap(kLCtrl);
    CHECK(r.take() == Ev{"+11", "-11"});
    // Ctrl+Pause: Print, and no Enter.
    r.key(kRCtrl, true);
    r.tap(kPause);
    r.key(kRCtrl, false);
    CHECK(r.take() == Ev{"+03", "-03"});
    // Alt + tapped Left Ctrl: Quit (Alt+Reset).
    r.key(kLAlt, true);
    r.tap(kLCtrl);
    r.key(kLAlt, false);
    CHECK(r.take() == Ev{"+19", "+11", "-11", "-19"});
}

TEST_CASE("keymap: a tap survives host repeats and stray events") {
    Rig r(KeyboardType::Terminal122);
    // Host auto-repeat of the tap key itself (a second down without an up,
    // as some hosts send even when the front end filters repeat events).
    r.key(kRCtrl, true);
    r.key(kRCtrl, true);
    r.key(kRCtrl, true);
    // A release of a key that was never pressed (focus changes can do this).
    r.key(kA, false);
    r.key(kRCtrl, false);
    CHECK(r.take() == Ev{"+58", "-58"});
    // A key held from before (fast typing rolls into Enter) and released
    // while the tap key is down does not cancel the tap.
    r.key(kA, true);
    r.key(kRCtrl, true);
    r.key(kA, false);
    r.key(kRCtrl, false);
    CHECK(r.take() == Ev{"+1C", "-1C", "+58", "-58"});
    // A release without a press of the tap key sends nothing.
    r.key(kRCtrl, false);
    CHECK(r.take().empty());
    // Another key pressed while it is held: Ctrl, not Enter.
    r.key(kRCtrl, true);
    r.tap(kA);
    r.key(kRCtrl, false);
    CHECK(r.take() == Ev{"+1C", "-1C"});
    // Shift held across the tap: Enter is sent with the terminal Shift
    // released around it, then pressed again.
    r.key(kLShift, true);
    r.tap(kRCtrl);
    r.key(kLShift, false);
    CHECK(r.take() == Ev{"+12", "-12", "+58", "-58", "+12", "-12"});
}

TEST_CASE("keymap: Alt held through a rule that releases it") {
    Rig r(KeyboardType::Terminal122);
    r.key(kLAlt, true);
    r.key(kF1, true);             // Alt+F1: Help (0B, no Alt)
    CHECK(r.take() == Ev{"+19", "-19", "+0B"});
    r.key(kF1, false);
    CHECK(r.take() == Ev{"-0B", "+19"});
    r.key(kLAlt, false);
    CHECK(r.take() == Ev{"-19"});
}

TEST_CASE("keymap: PCOMM defaults on the 122-key keyboard") {
    Rig r(KeyboardType::Terminal122);
    r.tap(kEsc);
    CHECK(r.take() == Ev{"+05", "-05"});           // Attn
    r.key(kLShift, true);
    r.tap(kEsc);                                    // SysRq = Shift+Attn
    r.key(kLShift, false);
    CHECK(r.take() == Ev{"+12", "+05", "-05", "-12"});
    r.tap(kEnter);
    CHECK(r.take() == Ev{"+5A", "-5A"});           // Field Exit
    r.key(kRShift, true);
    r.tap(kEnter);                                  // New Line key, unshifted
    r.key(kRShift, false);
    CHECK(r.take() == Ev{"+59", "-59", "+64", "-64", "+59", "-59"});
    r.tap(kPause);
    CHECK(r.take() == Ev{"+19", "+06", "-06", "-19"});   // Clear (Alt)
    r.tap(kPrtSc);
    CHECK(r.take() == Ev{"+03", "-03"});           // Print
    r.tap(kPgUp);
    CHECK(r.take() == Ev{"+12", "+60", "-60", "-12"});   // Roll Down = Shift+Down
    r.tap(kPgDn);
    CHECK(r.take() == Ev{"+12", "+63", "-63", "-12"});   // Roll Up = Shift+Up
    r.tap(kInsert);
    r.tap(kDelete);
    CHECK(r.take() == Ev{"+65", "-65", "+6D", "-6D"});
    r.key(kLShift, true);
    r.tap(kInsert);                                 // Dup
    r.key(kLShift, false);
    CHECK(r.take() == Ev{"+12", "-12", "+6E", "-6E", "+12", "-12"});
    r.key(kLShift, true);
    r.tap(kTab);                                    // Backtab: Field Backspace key
    r.key(kLShift, false);
    CHECK(r.take() == Ev{"+12", "-12", "+67", "-67", "+12", "-12"});
    r.tap(kHome);
    CHECK(r.take() == Ev{"+19", "+62", "-62", "-19"});   // Home (Alt+Rule key)
    r.tap(kEnd);
    CHECK(r.take().empty());                        // Erase EOF: none
    r.key(kLAlt, true);
    r.tap(kEnd);                                    // Erase Input
    r.key(kLAlt, false);
    CHECK(r.take() == Ev{"+19", "-19", "+0C", "-0C", "+19", "-19"});
    r.tap(kKpPlus);
    r.tap(kKpMinus);
    r.tap(kKpEnter);
    CHECK(r.take() == Ev{"+79", "-79", "+7C", "-7C", "+5A", "-5A"});
    r.key(kRAlt, true);                             // AltGr+End: Erase Input
    r.tap(kEnd);
    r.key(kRAlt, false);
    CHECK(r.take() == Ev{"+39", "-39", "+0C", "-0C", "+39", "-39"});
}

TEST_CASE("keymap: PCOMM defaults on the Enhanced keyboard") {
    Rig r(KeyboardType::Enhanced);
    r.tap(kEsc);
    CHECK(r.take() == Ev{"+08", "-08"});
    r.key(kLShift, true);
    r.tap(kEsc);                                    // SysRq = Alt+Print
    r.key(kLShift, false);
    CHECK(r.take() == Ev{"+12", "-12", "+19", "+57", "-57", "-19", "+12", "-12"});
    r.key(kLShift, true);
    r.tap(kF1);                                     // F13 = Shift+F1, Shift kept
    r.key(kLShift, false);
    CHECK(r.take() == Ev{"+12", "+07", "-07", "-12"});
    r.tap(kPgUp);
    r.tap(kPgDn);
    CHECK(r.take() == Ev{"+6F", "-6F", "+6D", "-6D"});
    r.tap(kKpPlus);
    r.tap(kKpEnter);
    CHECK(r.take() == Ev{"+7B", "-7B", "+5A", "-5A"});
    r.tap(kEnd);
    CHECK(r.take() == Ev{"+65", "-65"});
    r.key(kLCtrl, true);
    r.tap(kHome);                                   // Rule = Alt+PgDn
    r.key(kLCtrl, false);
    CHECK(r.take() == Ev{"+19", "+6D", "-6D", "-19"});
}

TEST_CASE("keymap: Set3Keyboard applies the keymap") {
    Set3Keyboard k(KeyboardType::Terminal122, 0xBF, 0xBF);
    auto km = std::make_shared<Keymap>();
    REQUIRE(builtin_keymap("pcomm", true, *km));
    k.set_keymap(km);
    k.power_on();
    while (k.has_output()) k.read_output();
    HostKeyEvent ev;
    ev.hid_usage = kRCtrl;
    ev.pressed = true;
    k.host_key(ev);
    ev.pressed = false;
    k.host_key(ev);
    std::vector<uint8_t> out;
    while (k.has_output()) out.push_back(k.read_output());
    CHECK(out == std::vector<uint8_t>{0x58});   // Enter (make only by default)
}

TEST_CASE("keymap files: built-ins written out match") {
    const std::string dir = EM34XX_SOURCE_DIR "/keymaps/";
    for (bool sf : {true, false}) {
        Keymap file, builtin;
        std::string error;
        INFO(error);
        REQUIRE(load_keymap(dir + "pcomm-like.toml", sf, file, error));
        REQUIRE(builtin_keymap("pcomm", sf, builtin));
        check_same_keymap(file, builtin);
    }
    Keymap file, builtin;
    std::string error;
    REQUIRE(load_keymap(dir + "positional.toml", true, file, error));
    REQUIRE(builtin_keymap("positional", true, builtin));
    check_same_keymap(file, builtin);
}

TEST_CASE("keymap files: base, extension and names") {
    Keymap km;
    std::string error;
    CHECK(load_keymap("pcomm", true, km, error));
    CHECK(load_keymap("positional", true, km, error));
    CHECK_FALSE(load_keymap("/nonexistent/keymap.toml", true, km, error));

    // Default base is pcomm; a later rule replaces a default one.
    auto ext = parse(R"(
[[map]]
host = "Escape"
to = "reset"
[[map]]
host = "hid:0x04"
mods = "LCtrl"
to = 0x22
keyboard = "enhanced"
[[map]]
host = "Keypad Enter"
mods = "*"
to = "positional"
)");
    Rig r(KeyboardType::Enhanced, ext);
    r.tap(kEsc);
    CHECK(r.take() == Ev{"+11", "-11"});
    r.tap(kKpEnter);
    CHECK(r.take() == Ev{"+79", "-79"});
    r.key(kLCtrl, true);
    r.tap(kA);
    r.key(kLCtrl, false);
    CHECK(r.take() == Ev{"+22", "-22"});
    Rig r122(KeyboardType::Terminal122, ext);
    r122.key(kLCtrl, true);
    r122.tap(kA);                                  // enhanced-only rule
    r122.key(kLCtrl, false);
    CHECK(r122.take() == Ev{"+1C", "-1C"});

    // Every terminal function resolves on both keyboard types.
    for (const std::string& n : terminal_function_names()) {
        std::array<TerminalStroke, 2> to{};
        std::array<bool, 2> ok{};
        INFO(n);
        CHECK(terminal_target_from_name(n, to, ok));
        CHECK(ok[0]);
        CHECK(ok[1]);
    }
    uint32_t hid = 0;
    CHECK(host_key_from_name("KP_Enter", hid));
    CHECK(hid == 0x58);
    CHECK(host_key_from_name("Right Ctrl", hid));
    CHECK(hid == 0xE4);
    CHECK(host_key_from_name("F24", hid));
    CHECK(hid == 0x73);
}

TEST_CASE("keymap files: errors name the line") {
    auto fails = [](const std::string& text, const std::string& needle) {
        Keymap km;
        std::string error;
        CHECK_FALSE(parse_keymap(text, "k.toml", true, km, error));
        INFO(error);
        CHECK(error.find(needle) != std::string::npos);
    };
    fails("base = \"qwerty\"\n", "base must be");
    fails("colour = 1\n", "unknown key 'colour'");
    fails("[[map]]\nhost = \"nokey\"\nto = \"attn\"\n", "k.toml:1: unknown host key 'nokey'");
    fails("\n[[map]]\nhost = \"a\"\nto = \"warp\"\n", "k.toml:2: unknown terminal key 'warp'");
    fails("[[map]]\nhost = \"a\"\nmods = \"hyper\"\nto = \"attn\"\n", "unknown modifier 'hyper'");
    fails("[[map]]\nhost = \"a\"\n", "needs host and to");
    fails("[[map]]\nhost = \"a\"\nto = \"attn\"\nshift = \"maybe\"\n", "shift must be");
    fails("[[map]]\nhost = \"a\"\nto = \"attn\"\nkeyboard = \"3270\"\n", "keyboard must be");
    fails("[[map]]\nhost = \"a\"\nto = \"attn\"\nwhat = 1\n", "unknown key 'what'");
    fails("[[map]\n", "k.toml");
}

TEST_CASE("mac keymap: PCOMM with Return as Enter, keypad Enter as Field Exit") {
    // The file form builds the built-in map.
    const KeymapFile mac = mac_keymap_file();
    Keymap from_text, builtin;
    std::string error;
    INFO(error);
    REQUIRE(parse_keymap(keymap_file_to_toml(mac), "mac.toml", true, from_text, error));
    REQUIRE(builtin_keymap("mac", true, builtin));
    check_same_keymap(from_text, builtin);

    // It differs from PCOMM in the plain Return rule only, in place.
    const KeymapFile pc = pcomm_keymap_file();
    REQUIRE(mac.entries.size() == pc.entries.size());
    size_t differ = 0;
    for (size_t i = 0; i < pc.entries.size(); ++i) {
        const KeymapEntry& m = mac.entries[i];
        if (m.host == "KP_Enter") CHECK(m.to == "fieldexit");          // fn+Return
        if (m.host == "Enter" && m.mods == "shift") CHECK(m.to == "newline");
        if (m == pc.entries[i]) continue;
        ++differ;
        CHECK(pc.entries[i].host == "Enter");
        CHECK(pc.entries[i].mods.empty());
        CHECK(pc.entries[i].to == "fieldexit");
        CHECK(m.to == "enter");
    }
    CHECK(differ == 1);

    // As a base in a keymap file and by name.
    Keymap km;
    CHECK(parse_keymap("base = \"mac\"\n", "k.toml", true, km, error));
    CHECK(load_keymap("mac", true, km, error));
}

TEST_CASE("keymap files as text: round trip, PCOMM default, names") {
    // The PCOMM map as a file builds the built-in map, also after a round trip.
    const KeymapFile pc = pcomm_keymap_file();
    CHECK(pc.base == "positional");
    REQUIRE_FALSE(pc.entries.empty());
    const std::string text = keymap_file_to_toml(pc, "em34xx keymap\nline two");
    CHECK(text.rfind("# em34xx keymap\n# line two\n", 0) == 0);
    Keymap from_text, builtin;
    std::string error;
    INFO(error);
    REQUIRE(parse_keymap(text, "pcomm.toml", true, from_text, error));
    REQUIRE(builtin_keymap("pcomm", true, builtin));
    check_same_keymap(from_text, builtin);

    KeymapFile back;
    REQUIRE(parse_keymap_file(text, "pcomm.toml", back, error));
    CHECK(back.base == pc.base);
    REQUIRE(back.entries.size() == pc.entries.size());
    for (size_t i = 0; i < back.entries.size(); ++i) CHECK(back.entries[i] == pc.entries[i]);

    // The shipped file reads as entries too; numeric forms become text.
    KeymapFile f;
    REQUIRE(parse_keymap_file("base = \"none\"\n[[map]]\nhost = 0x04\nmods = [\"lctrl\", \"shift\"]\nto = 0x22\n"
                              "keyboard = \"enhanced\"\nshift = \"release\"\ntap = true\n",
                              "t", f, error));
    REQUIRE(f.entries.size() == 1);
    CHECK(f.entries[0].host == "a");
    CHECK(f.entries[0].mods == "lctrl shift");
    CHECK(f.entries[0].to == "0x22");
    CHECK(f.entries[0].keyboard == "enhanced");
    CHECK(f.entries[0].shift == "release");
    CHECK(f.entries[0].tap);
    CHECK_FALSE(parse_keymap_file("[[map]]\nhost = \"nosuchkey\"\nto = \"attn\"\n", "t", f, error));

    KeymapEntry e;
    e.host = "F1";
    e.mods = "alt";
    e.to = "help";
    CHECK(check_keymap_entry(e, error));
    e.to = "bogus";
    CHECK_FALSE(check_keymap_entry(e, error));
    CHECK(error.find("bogus") != std::string::npos);

    // Every named key reads back as itself.
    for (uint32_t hid = 0x04; hid <= 0xE7; ++hid) {
        uint32_t back_hid = 0;
        const std::string n = host_key_name(hid);
        REQUIRE(host_key_from_name(n, back_hid));
        CHECK(back_hid == hid);
    }
    CHECK(host_key_name(0x58) == "KP_Enter");
    CHECK(host_mods_text(kModLShift | kModRCtrl) == "shift ctrl");
    CHECK(host_mods_text(kModRAlt) == "altgr");
    CHECK(host_mods_text(0).empty());
}
