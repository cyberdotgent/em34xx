// SPDX-License-Identifier: GPL-3.0-or-later
// The monitor's `connect HOST[:PORT] [options]` command, parsed into a
// configuration for the TN5250 bridge.
//
// Values not given on the command come from the configuration the emulator
// started with (file and command line). Options:
//   tls                TLS; the default port becomes 992
//   station N          only station address N, as a display
//   device NAME        device name (DEVNAME)
//   type TERMTYPE      terminal type (RFC 1205, e.g. IBM-3477-FC)
//   port N, codepage CP, ca FILE, env NAME=VALUE
//   any other NAME [VALUE] or NAME=VALUE is passed on as the command-line
//   option --tn5250-NAME [VALUE] (e.g. tls-verify none, tls-pin HASH,
//   tls-min 1.0), so every TLS setting the command line knows works here.
#pragma once

#include "config/config.hpp"

#include <string>
#include <vector>

namespace tn34xx {

// args[0] is the command name ("connect"). `base` is the start-up
// configuration; on success `out` is base with the connect settings applied
// and twinax.host set to tn5250.
bool parse_connect_args(const std::vector<std::string>& args, const AppConfig& base, AppConfig& out,
                        std::string& error);

} // namespace tn34xx
