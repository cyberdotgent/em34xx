// SPDX-License-Identifier: GPL-3.0-or-later
// Static description of a terminal model: CPU, address space, ROM set and
// notes. The Machine core needs this; the Model registry that builds
// machines from it lives in src/models (em34xx_machines).
#pragma once

#include "cpu/cpu.hpp"
#include "machine/line_port.hpp"
#include "rom/romset.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace em34xx {

enum class ModelStatus {
    Bringup,        // ROMs good, hardware being reverse engineered
    RomIncomplete,  // only a defective ROM dump is available
    Placeholder,    // registered so the CLI knows it; cannot run yet
};

const char* model_status_name(ModelStatus s);

// A tube a monochrome unit can be shown with. It only changes the picture:
// the hardware and what the unit tells its host are the same whichever tube
// is fitted (observed on real units: an amber tube in a green unit works).
struct Phosphor {
    std::string id;      // "green": what configuration files store
    std::string label;   // "Green": what a front end shows
    uint32_t rgb = 0xFFFFFFFFu;   // full-intensity colour, 0xFFRRGGBB
};

// The standard tubes.
const Phosphor& phosphor_green();
const Phosphor& phosphor_amber();
const Phosphor& phosphor_paper_white();

// One hardware identity of a model: what the user selects ("3477-FG"), what
// the firmware is strapped as, and what the unit announces to its host.
// A model has a monochrome variant, a colour variant or both; a monochrome
// variant is one unit whatever tube it shows (its `phosphors`).
struct ModelVariant {
    std::string id;            // "3477-FG": shown, and stored in configuration files
    std::string description;   // "monochrome"
    bool colour = false;       // the host may send colour, and the picture is in colour
    uint8_t straps = 0;        // configuration strap byte the firmware reads
    std::string tn5250_type;   // terminal type announced to the host: RFC 1205 (TN5250) or,
                               // for a coax unit, RFC 1576/2355 (TN3270, e.g. IBM-3278-2-E)
    std::vector<Phosphor> phosphors;   // tubes a monochrome variant offers, default first; empty for colour
    // The firmware of this variant when it has its own (a colour unit with
    // other ROMs than the monochrome one); empty = the model's (ModelInfo::roms).
    std::vector<RomSpec> roms;

    // The phosphor with this id, or the first one ("" and unknown ids);
    // null for a colour variant.
    const Phosphor* phosphor(const std::string& id) const;
};

// What one of a terminal's sessions can be.
enum class SessionKind {
    Display,            // a display session only
    Printer,            // the printer port only
    DisplayOrPrinter,   // either, as the terminal's setup says
};

// "display", "printer", "display or printer".
const char* session_kind_label(SessionKind kind);

// A switch a model declares for itself (a temporary stand-in that can be
// turned off, an optional device): what --list-models shows and
// --no-<id> / --set <id>=on|off and [emulator.options] set.
struct ModelOption {
    std::string id;            // "vdo-shim"
    std::string description;
    bool default_on = true;
};

struct ModelInfo {
    std::string name;            // "3477"
    std::string description;
    ModelStatus status = ModelStatus::Placeholder;
    CpuLevel cpu = CpuLevel::I8088;
    unsigned address_bits = 20;
    uint64_t clock_hz = 0;       // nominal CPU clock, used for pacing
    LineKind line = LineKind::Twinax;   // what the unit hangs on (machine/line_port.hpp)
    // The sessions the unit can run, in its own order (its first session,
    // its second, ...): what each one can be. Never empty; never more than a
    // cable carries (TwinaxConfig::kMaxStationsPerBus, 7). Whether a slot
    // that can be either is a display or the printer is the terminal's own
    // setting: the controller reads it from the device (docs/twinax-bridge.md).
    std::vector<SessionKind> sessions{SessionKind::Display};
    unsigned max_sessions() const { return unsigned(sessions.size()); }
    // How many of the sessions can be printer sessions at once.
    unsigned max_printer_sessions() const;
    // What can be selected, default first. Never empty for a model that runs.
    std::vector<ModelVariant> variants;
    // Switches of this model, with their defaults (TerminalOptions::switches).
    std::vector<ModelOption> options;
    // The state of a declared switch: what `switches` says, else its default;
    // false for one the model does not declare.
    bool option_on(const std::string& id, const std::map<std::string, bool>& switches) const;
    std::vector<RomSpec> roms;   // the firmware of every variant that names none of its own
    // The images `variant` runs.
    const std::vector<RomSpec>& roms_of(const ModelVariant& variant) const {
        return variant.roms.empty() ? roms : variant.roms;
    }
    std::vector<KnownBadRom> known_bad_roms;
    std::vector<std::string> notes;  // hypotheses and open questions
};

// What a front end shows for a variant: the terminal family's protocol and
// the variant's description, "5250, colour" or "3270, monochrome".
std::string variant_label(const ModelInfo& model, const ModelVariant& variant);

} // namespace em34xx
