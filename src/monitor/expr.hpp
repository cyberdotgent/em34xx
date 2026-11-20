// Expression evaluator for monitor commands.
//
//   numbers      hexadecimal by default (1f00, 0x1f00, 1f00h); #123 is decimal
//   registers    ax..di, al..bh, es cs ss ds, ip, flags, pc (linear CS:IP)
//   memory       byte[addr]  word[addr]   (addr may be seg:off)
//   operators    + - * / % & | ^ ~ << >> == != < <= > >= && || !  ( )
//
// An address is "expr" (linear) or "expr:expr" (segment:offset).
#pragma once

#include <cstdint>
#include <string>

namespace tn34xx {

class Machine;

bool eval_expr(Machine& m, const std::string& text, int64_t& out, std::string& error);
bool eval_address(Machine& m, const std::string& text, uint32_t& linear, std::string& error);

// Register access by name, shared with the "r" command.
bool get_register(Machine& m, const std::string& name, uint16_t& value);
bool set_register(Machine& m, const std::string& name, uint16_t value);

} // namespace tn34xx
