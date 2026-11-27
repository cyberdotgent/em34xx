// SPDX-License-Identifier: GPL-3.0-or-later
#include "models/coax_terminal.hpp"

#include "bus/bus.hpp"
#include "input/host_keymap.hpp"
#include "input/keyboard_ids.hpp"
#include "input/set3_keyboard.hpp"
#include "machine/machine.hpp"
#include "models/model.hpp"

#include <memory>
#include <stdexcept>

namespace em34xx {

namespace {

// Owns the board state the register devices share and the keyboard; a
// device only so the machine keeps them alive.
struct BoardHolder : Device {
    BoardHolder(Bus& bus, I80186Peripherals* cpu_io, uint64_t clock_hz, uint8_t sense, const CoaxBoardLayout& layout)
        : Device("coax-board"), board(bus, cpu_io, clock_hz, sense, layout) {}
    Board3483 board;
    std::unique_ptr<Set3Keyboard> keyboard;
};

} // namespace

void build_coax_terminal(Machine& m, const ModelInfo& info, const CoaxBoardLayout& layout, I80186Peripherals* cpu_io,
                         uint8_t sense) {
    Bus& bus = m.bus();
    const TerminalOptions& opt = m.options();
    auto& holder = m.add_device<BoardHolder>(bus, cpu_io, info.clock_hz, sense, layout);
    if (!cpu_io) bus.set_interrupt_controller(&holder.board);
    Board3483::Display& disp = m.add_device<Board3483::Display>(holder.board);
    Board3483::Coax& coax = m.add_device<Board3483::Coax>(holder.board);
    Machine* machine = &m;
    coax.set_line([machine] { return machine->coax_host(); });
    Board3483::System& sys = m.add_device<Board3483::System>(holder.board);
    m.add_device<Board3483::Window>(holder.board);

    // The 122-key keyboard on serial channel A [hypothesis: the 3483's POST
    // expects ID BF BF and talks to channel A in its keyboard test].
    uint8_t id1 = 0xBF, id2 = 0xBF;
    std::string error;
    if (!resolve_keyboard_id(opt, id1, id2, error)) throw std::runtime_error(error);
    KeyboardType type = KeyboardType::Terminal122;
    parse_keyboard_type(opt.keyboard_type, type);
    holder.keyboard = std::make_unique<Set3Keyboard>(type, id1, id2);
    holder.keyboard->set_shift_fkeys(opt.shift_fkeys);
    // No ID after the self-test result: the 3483's POST polls the keyboard
    // after resetting it (C000:ACCC) and takes any byte already waiting for
    // a key held at power-on, which opens Offline Setup (the documented way
    // into setup); it asks for the ID itself (F2) right after [observed:
    // with the ID appended every boot opened Offline Setup].
    holder.keyboard->set_bat_sends_id(false);
    // The 3270 legends (Attn, Clear, PA1-PA3, Erase EOF ...), and the
    // built-in 5250 keymaps as their 3270 counterparts.
    holder.keyboard->set_legends_3270(true);
    auto keymap = std::make_shared<Keymap>();
    if (!load_keymap(keymap_for_3270(opt.keymap), opt.shift_fkeys, *keymap, error)) throw std::runtime_error(error);
    holder.keyboard->set_keymap(std::move(keymap));
    sys.attach_keyboard(holder.keyboard.get(), info.clock_hz);
    sys.set_audio(&m.audio());
    m.set_keyboard(holder.keyboard.get());
    bus.map_mmio("display", layout.display, 0x100, &disp);
    bus.map_mmio("coax", layout.coax, 0x100, &coax);
    bus.map_mmio("system", layout.system, 0x100, &sys);
    bus.observe_writes(0x00000, 0x10000);
    bus.observe_writes(layout.sram, 0x10000);
    bus.observe_writes(layout.display, 0x100);
    m.set_display(&disp);
}

} // namespace em34xx
